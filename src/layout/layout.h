#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "layout_computer.h"
#include "theme.h"
#include <chrono>
#include <memory_resource>
#include <optional>
#include <span>

class Document;
class TaskScheduler;
class ViewportManager;

struct TableRestoreResult {
    bool restored = false;
    // 見積もった行高さで evict されていた行を実測し直して高さが変わったときの新しい高さ。
    std::optional<float> height;
};

// IDWriteFactory はスレッドセーフで IDWriteTextLayout は per-call 生成のため、
// 同一インスタンスを複数ワーカーから const 経由で叩いて良い。
// lifecycle 系 (RecreateFormats / SetTheme / SetScheduler) は non-const で UI スレッドからのみ呼ぶ。
// 並列計測には const 参照で渡すので、計測中に lifecycle 系は呼べない。
class ITextMeasurer {
public:
    virtual ~ITextMeasurer() = default;

    virtual bool RecreateFormats() = 0;

    // 計測に使うテーマ。ズームや色の切り替えは同じ Theme を書き換えるので、渡すのは 1 回でよい。
    void SetTheme(const Theme& theme) noexcept
    {
        theme_ = &theme;
    }
    const Theme& GetTheme() const noexcept
    {
        return *theme_;
    }
    // 並列計測 (ノード単位 / 巨大テーブルのセル単位) に使う scheduler。nullptr なら呼び出しスレッドで直列に計測する。
    // scheduler の Shutdown より前に nullptr へ戻して参照を切る契約。
    void SetScheduler(TaskScheduler* scheduler) noexcept
    {
        scheduler_ = scheduler;
    }
    TaskScheduler* Scheduler() const noexcept
    {
        return scheduler_;
    }

    // 測った高さを返す。測れなければ nullopt で、エントリは dirty のまま再試行を待つ (例外と同じく上限で諦める)。
    // entry にはレイアウトなどの計測の産物だけを書き、高さと dirty の確定は MeasureEntry (CommitMeasured) に任せる。
    // 並列計測では worker ごとに別ノード・別 entry を渡す。
    // viewport (エントリ上端からのローカル座標) はテーブルで行単位の部分復元判定に用いる。
    // ビットマップを描くノード (IsBitmapDrawNode) は渡さない。高さは MeasureEntry が BitmapNodeHeight で決める。
    virtual std::optional<float> MeasureNode(
        const Node& node, NodeLayoutEntry& entry, float max_width,
        YRange viewport) const = 0;
    // 幅不変のまま行単位 evict されたテーブルの、viewport 内の evict 行だけセルを再生成する。
    // layout_dirty を経由しないため、可視中に毎フレーム全行を再計測することがない。
    // 高さが変わったときの確定は呼び出し側 (CommitMeasured) に任せる。
    virtual TableRestoreResult RestoreEvictedTableRows(const Node& /*node*/, NodeLayoutEntry& /*entry*/, float /*max_width*/,
                                                       YRange /*viewport*/) const
    {
        return {};
    }

protected:
    const Theme* theme_ = nullptr;
    TaskScheduler* scheduler_ = nullptr;
};

// 測れたら組んだ幅 (NodeLayoutWidth) と実測値を cached_width/height に残し、同じ幅へ戻った際に
// EstimateInvisibleNodeHeight が推定値で上書きしないようにする。失敗回数も 0 に戻し、以後の一時的な失敗を初回として扱う。
// 測れなければ何も確定せず、推定の高さを実測値として残さない。
// viewport は文書座標で受け取り、entry_top 基準のローカル座標に直してバックエンドへ渡す。
// bitmap は画像/図ノードの DiagramEntry (無ければ nullptr)。戻り値は測れたか。
inline bool MeasureEntry(
    const ITextMeasurer& measurer, const Node& node, NodeLayoutEntry& entry, float node_width,
    YRange viewport, float entry_top, const DiagramEntry* bitmap, const Theme& theme)
{
    const std::optional<float> height = IsBitmapDrawNode(node)
                                            ? layout::BitmapNodeHeight(bitmap, node_width, entry.height, theme)
                                            : measurer.MeasureNode(node, entry, node_width, viewport.ToLocal(entry_top));
    if (height) {
        entry.CommitMeasured(layout::NodeLayoutWidth(node, node_width), *height);
    }
    return height.has_value();
}

// RestoreEvictedTableRows で可視の evict 行を戻し、行の実測で高さが変わったら確定する。viewport はローカル座標。
inline TableRestoreResult RestoreEntryTableRows(
    const ITextMeasurer& measurer, const Node& node, NodeLayoutEntry& entry, float node_width, YRange viewport)
{
    const TableRestoreResult result = measurer.RestoreEvictedTableRows(node, entry, node_width, viewport);
    if (result.height) {
        entry.CommitMeasured(layout::NodeLayoutWidth(node, node_width), *result.height);
    }
    return result;
}

// 計測を打ち切る時刻。過ぎても 1 ノード目は計測するので、呼び出しごとに必ず進む。
using LayoutDeadline = std::chrono::steady_clock::time_point;
inline constexpr LayoutDeadline kNoLayoutDeadline = LayoutDeadline::max();

namespace layout {

// indices のノードを scheduler の worker で並列計測する。scheduler が null なら呼び出しスレッドで直列に計測する。
// 戻り値は dirty のまま残したノード数 (例外で計測できなかったか、期限を過ぎて手を付けなかった)。
int MeasureIndicesParallel(
    const std::pmr::vector<Node>& nodes,
    LayoutCache& cache,
    float content_width,
    const Theme& theme,
    const ITextMeasurer& backend,
    std::span<const size_t> indices,
    YRange viewport,
    TaskScheduler* scheduler,
    LayoutDeadline deadline);

} // namespace layout

class LayoutEngine {
public:
    // 小刻みな WM_SIZE で全ノード再レイアウトが頻発するのを防ぐ幅変化の閾値。
    static constexpr float kWidthChangeThreshold = 2.0f;

    // theme は measurer に渡し、以後は measurer が持つものを使う。
    bool Init(ITextMeasurer* measurer, const Theme& theme);
    // 本文幅かズームがキャッシュの推定時 (EstimateBasis) から変わったら、全ノードを dirty にして推定値で Y を組み直す
    // (DirectWrite は呼ばない)。戻り値は全ノードを dirty にしたか。
    bool Reestimate(const std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width);
    // [viewport_top, viewport_bottom] に入る dirty を計測し、退避したテーブル行を戻す。
    // 計測で高さが変わると範囲に入るノードも変わるため、範囲内の dirty が無くなるまで繰り返す。
    // 戻り値は高さ・位置・テーブル行のいずれかが変わったか。
    bool EnsureVisibleLayout(
        const std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width,
        float viewport_top, float viewport_bottom);
    // range (文書座標) 内の dirty を deadline まで計測する。range 内に dirty が残れば true。
    bool ProcessDirtyBatch(
        const std::pmr::vector<Node>& nodes, LayoutCache& cache,
        float viewport_width, LayoutDeadline deadline, YRange range);
    const Theme& GetTheme() const noexcept
    {
        return measurer_->GetTheme();
    }

private:
    struct PassResult {
        bool updated = false;
        // 期限か計測失敗で、範囲内の dirty を計測しきれなかった。
        bool remaining = false;
    };
    // 範囲内の dirty を deadline まで計測し、退避したテーブル行を戻して Y を組み直す。
    PassResult MeasurePass(
        const std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width,
        YRange range, LayoutDeadline deadline);

    // Init は UI スレッドからのみ呼び、
    // MeasureNode は const 経由で measurer の scheduler の worker から並列呼び出しされる。
    ITextMeasurer* measurer_ = nullptr;
};

// LayoutEngine + ViewportManager の組み合わせを薄くラップし、
// スクロール target 管理付きのレイアウト操作を提供する。
class LayoutService {
public:
    LayoutService(LayoutEngine& engine, ViewportManager& viewport) noexcept
        : engine_(engine), viewport_(viewport)
    {
    }

    // 推定し直した Y で見ている位置を求めてから、その可視範囲を計測する。
    void ViewportLayout(const Document& doc, LayoutCache& cache, float width, float height);

    // ProcessDirtyBatch / EnsureVisibleLayout は見ている位置を保つ (前後で scroll target を確定・適用する)。
    // ProcessDirtyBatch は可視範囲の上下 height * buffer_screens までの dirty を計測する。height が 0 でも全域にはしない。
    bool ProcessDirtyBatch(
        const Document& doc, LayoutCache& cache, float width, LayoutDeadline deadline,
        float height, float buffer_screens);
    bool EnsureVisibleLayout(const Document& doc, LayoutCache& cache, float width, float height);
    // 画面外の node も計測する。見ている位置は保つ。
    bool EnsureNodeLayout(const Document& doc, LayoutCache& cache, float width, int node);
    void RecomputeAfterDiagram(const Document& doc, LayoutCache& cache, layout::HeightChangeRange changed) noexcept;

private:
    // 前後で scroll target を確定・適用し、見ている位置を保ったまま op を行う。op の結果を返す。
    template <class Op>
    bool KeepingAnchor(LayoutCache& cache, Op&& op);
    // [top, bottom] を計測する。見ている位置は保つ。
    bool EnsureRangeLayout(const Document& doc, LayoutCache& cache, float width, float top, float bottom);

    LayoutEngine& engine_;
    ViewportManager& viewport_;
};
