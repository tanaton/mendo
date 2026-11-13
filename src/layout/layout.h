#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "layout_computer.h"
#include "theme.h"
#include "worker_latch.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

class Document;
struct DocumentData;
class TaskScheduler;
class ViewportManager;

struct TableRestoreResult {
    bool restored = false;
    // 見積もった行高さで evict されていた行を実測し直して高さが変わったときの新しい高さ。
    std::optional<float> height;
};

// entry に触れずに初めから組んだ表の幾何 (ITextMeasurer::BuildTable)。
struct BuiltTable {
    std::unique_ptr<TableLayoutData> table;
    float height = 0.0f;
    // 寸法がまだ無いインライン数式か画像を仮に組んだ。
    bool pending_inline = false;
};

// 計測は const で、IDWriteTextLayout は per-call 生成、ファクトリと書式はワーカーごとに分ける (DWriteTextMeasurer) ため、
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

    // 測った高さを返す。測れなければ nullopt (例外と同じく再試行しない)。
    // entry にはレイアウトなどの計測の産物だけを書き、高さと dirty の確定は MeasureEntry (CommitMeasured) に任せる。
    // 並列計測では worker ごとに別ノード・別 entry を渡す。
    // 図のノード (NodeType::Diagram) は渡さない。高さは MeasureEntry が DiagramNodeHeight で決める。
    virtual std::optional<float> MeasureNode(const Node& node, NodeLayoutEntry& entry, float max_width) const = 0;
    // dirty でないテーブルの、viewport (エントリ上端からのローカル座標) 内の evict 行だけセルを再生成する。
    // layout_dirty を経由しないため、可視中に毎フレーム全行を再計測することがない。
    // 幅が再計測のしきい値 (LayoutEngine::kWidthChangeThreshold) 未満で変わっていても、表は dirty にならないので今の幾何で戻す。
    // 高さが変わったときの確定は呼び出し側 (CommitMeasured) に任せる。
    virtual TableRestoreResult RestoreEvictedTableRows(const Node& /*node*/, NodeLayoutEntry& /*entry*/, YRange /*viewport*/) const
    {
        return {};
    }
    // 表の幾何を entry に触れずに初めから組む (巨大な表をワーカーで組む TableMeasureJobs が使う)。stop が立つか、
    // 測れなければ table が null。
    virtual BuiltTable BuildTable(const Node& /*node*/, float /*max_width*/, std::stop_token /*stop*/) const
    {
        return {};
    }

protected:
    const Theme* theme_ = nullptr;
    TaskScheduler* scheduler_ = nullptr;
};

// 測れたら組んだ幅 (NodeLayoutWidth) を cached_width に残し、同じ幅へ戻った際に
// EstimateInvisibleNodeHeight が推定値で上書きしないようにする。
// 測れなければ何も確定せず、推定の高さを実測値として残さない。
// diagram は図のノードの DiagramEntry (無ければ nullptr)。
inline void MeasureEntry(
    const ITextMeasurer& measurer, const Node& node, NodeLayoutEntry& entry, float node_width, const DiagramEntry* diagram)
{
    const std::optional<float> height = node.type == NodeType::Diagram
                                            ? layout::DiagramNodeHeight(diagram, node_width, entry.height, measurer.GetTheme())
                                            : measurer.MeasureNode(node, entry, node_width);
    if (height) {
        entry.CommitMeasured(layout::NodeLayoutWidth(node, node_width), *height);
    }
}

// RestoreEvictedTableRows で可視の evict 行を戻し、行の実測で高さが変わったら確定する。viewport はローカル座標。
// 組み直していないので、確定する幅は今の幅ではなく表の幾何を組んだ幅。
inline TableRestoreResult RestoreEntryTableRows(const ITextMeasurer& measurer, const Node& node, NodeLayoutEntry& entry, YRange viewport)
{
    const TableRestoreResult result = measurer.RestoreEvictedTableRows(node, entry, viewport);
    if (result.height) {
        entry.CommitMeasured(entry.table_layout()->last_applied_max_width, *result.height);
    }
    return result;
}

// 計測を打ち切る時刻。過ぎても 1 ノード目は計測するので、呼び出しごとに必ず進む。
using LayoutDeadline = std::chrono::steady_clock::time_point;
inline constexpr LayoutDeadline kNoLayoutDeadline = LayoutDeadline::max();

namespace layout {

// indices (昇順) のノードを backend の scheduler の worker で並列計測する。scheduler が null なら呼び出しスレッドで直列に計測する。
// 戻り値は期限を過ぎて手を付けず dirty のまま残したノード数。例外で測れなかったノードは再試行しない。
int MeasureIndicesParallel(
    std::span<const Node> nodes,
    LayoutCache& cache,
    float content_width,
    const ITextMeasurer& backend,
    std::span<const size_t> indices,
    LayoutDeadline deadline);

} // namespace layout

// 巨大な表の最初の計測 (全セルの自然幅と行の高さ) を計測ワーカーで行い、UI スレッドを止めない (性能レビュー A6)。
// 表は 1 ノードなので遅延レイアウトの期限 (4ms) では区切れず、全セルを測り終えるまで入力も描画も止まっていた
// (5 万行 6 列で 420ms、2 万行 6 列の長い文の表で 576ms)。組み終えるまで表は推定の高さのまま中身を描かない
// (極端に大きい表は現実にはほぼ無いので、それでよいとユーザーが判断した。2026-11-13)。
// 組む表のエントリは dirty のまま残し、組んでいる間は LayoutEngine::MeasurePass が飛ばす。リロードで新しい文書へ
// 付け替わったエントリ (CarryOverUnchanged) は、新しい文書で組み直す。
// UI スレッドから呼ぶ。ワーカーは文書を共有で持つので、文書を差し替えても止めるだけで待たない。
class TableMeasureJobs {
public:
    // セル数がこれを超える表をワーカーで組む。これ以下なら UI スレッドで測っても止まるのは数十 ms まで
    // (3 万セルの表で 44ms。BM_GiantBlock/table5k_first)。
    static constexpr size_t kMinCells = 10000;

    struct Result {
        uint64_t job = 0;
        size_t node = 0;
        // 測れなければ table が null。
        BuiltTable built;
        // 組む間に数式か画像の寸法が届いた (NoteInlineChanged)。
        bool inline_changed = false;
    };

    // measurer は scheduler のワーカーで表を組む。どちらも TableMeasureJobs より長生きする。
    TableMeasureJobs(TaskScheduler& scheduler, const ITextMeasurer& measurer) noexcept : scheduler_(scheduler), measurer_(measurer)
    {
    }
    ~TableMeasureJobs()
    {
        CancelAll();
    }
    TableMeasureJobs(const TableMeasureJobs&) = delete;
    TableMeasureJobs& operator=(const TableMeasureJobs&) = delete;

    // 組み終えたときに、組んだワーカーで呼ぶ。UI スレッドへ知らせるだけにする。
    void SetWake(std::move_only_function<void()> wake)
    {
        wake_ = std::move(wake);
    }
    // 組む表を持つ文書。差し替えたら呼ぶ。前の文書の組みは止める。
    void SetDocument(std::shared_ptr<const DocumentData> doc);
    // nodes[node] が今の文書のセル数 kMinCells 超の表で、幾何をまだ持たなければ、max_width で組ませて (組んでいる最中なら
    // そのままにして) true。
    bool Defer(std::span<const Node> nodes, size_t node, const NodeLayoutEntry& entry, float max_width);
    // 数式か画像の寸法が届いた。組んでいる表は、届く前の寸法で組んでいるかもしれない。
    void NoteInlineChanged() noexcept;
    // 今の文書で組み終えた表。止めた組みと前の文書の組みは捨てる。
    std::vector<Result> TakeResults();
    // 組みを止め、ワーカーが抜けるまで待つ。組むワーカーは書式とテーマを読むので、変える前と計測ワーカーを止める前に呼ぶ。
    void CancelAll();

private:
    struct Running {
        uint64_t job = 0;
        size_t node = 0;
        std::stop_source stop;
        bool inline_changed = false;
    };

    // 組んでいる表を止めて忘れる。待たない。
    void StopRunning() noexcept;

    TaskScheduler& scheduler_;
    const ITextMeasurer& measurer_;
    std::shared_ptr<const DocumentData> doc_;
    // 今の文書で組んでいる表。
    std::vector<Running> running_;
    uint64_t last_job_ = 0;
    PostedInbox<Result> results_;
    std::move_only_function<void()> wake_;
    WorkerLatch latch_;
};

class LayoutEngine {
public:
    // 小刻みな WM_SIZE で全ノード再レイアウトが頻発するのを防ぐ幅変化の閾値。
    static constexpr float kWidthChangeThreshold = 2.0f;

    // theme は measurer に渡し、以後は measurer が持つものを使う。
    bool Init(ITextMeasurer* measurer, const Theme& theme);
    // 巨大な表の最初の計測をワーカーに回す。無ければ UI スレッドで測る (テストとベンチ)。
    void SetTableJobs(TableMeasureJobs* jobs) noexcept
    {
        table_jobs_ = jobs;
    }
    // 本文幅かズームがキャッシュの推定時 (EstimateBasis) から変わったら、全ノードを dirty にして推定値で Y を組み直す
    // (DirectWrite は呼ばない)。戻り値は全ノードを dirty にしたか。
    bool Reestimate(std::span<const Node> nodes, LayoutCache& cache, float viewport_width);
    // [viewport_top, viewport_bottom] に入る dirty を計測し、退避したテーブル行を戻す。
    // 計測で高さが変わると範囲に入るノードも変わるため、範囲内の dirty が無くなるまで繰り返す。
    // 戻り値は高さ・位置・テーブル行のいずれかが変わったか。
    bool EnsureVisibleLayout(
        std::span<const Node> nodes, LayoutCache& cache, float viewport_width,
        float viewport_top, float viewport_bottom);
    struct PassResult {
        // 高さ・位置・テーブル行のいずれかが変わった。
        bool updated = false;
        // 期限で、範囲内の dirty を計測しきれなかった。
        bool remaining = false;
    };
    // range (文書座標) 内の dirty を deadline まで計測する。
    PassResult ProcessDirtyBatch(
        std::span<const Node> nodes, LayoutCache& cache,
        float viewport_width, LayoutDeadline deadline, YRange range);
    const Theme& GetTheme() const noexcept
    {
        return measurer_->GetTheme();
    }

private:
    // 範囲内の dirty を deadline まで計測し、退避したテーブル行を戻して Y を組み直す。
    PassResult MeasurePass(
        std::span<const Node> nodes, LayoutCache& cache, float viewport_width,
        YRange range, LayoutDeadline deadline);

    // Init は UI スレッドからのみ呼び、
    // MeasureNode は const 経由で measurer の scheduler の worker から並列呼び出しされる。
    ITextMeasurer* measurer_ = nullptr;
    TableMeasureJobs* table_jobs_ = nullptr;
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
    LayoutEngine::PassResult ProcessDirtyBatch(
        const Document& doc, LayoutCache& cache, float width, LayoutDeadline deadline,
        float height, float buffer_screens);
    bool EnsureVisibleLayout(const Document& doc, LayoutCache& cache, float width, float height);
    // 画面外の node も計測する。見ている位置は保つ。
    bool EnsureNodeLayout(const Document& doc, LayoutCache& cache, float width, int node);
    void RecomputeAfterDiagram(const Document& doc, LayoutCache& cache, IndexRange changed) noexcept;
    struct PlacedTables {
        bool placed = false;
        // 数式か画像を含む表で、組む間に寸法が届いたものがあった。仮に組んだものとして置いたので、組み直させる。
        bool stale_inline = false;
    };
    // ワーカーで組み終えた表 (TableMeasureJobs::TakeResults) を置き、見ている位置を保って Y を組み直す (置いた表の中を見ていたら、
    // 表の中の同じ割合の位置)。width は本文ペインの幅。
    // 今の幅で組んだ表は高さを確定し、幅が変わっていたら dirty のまま置いて、次の計測で列幅だけ組み直させる (自然幅は幅によらない)。
    PlacedTables PlaceMeasuredTables(const Document& doc, LayoutCache& cache, float width, std::vector<TableMeasureJobs::Result> results);

private:
    // 前後で scroll target を確定・適用し、見ている位置を保ったまま op を行う。op が値を返せばそれを返す。
    template <class Op>
    auto KeepingAnchor(LayoutCache& cache, Op&& op);
    // [top, bottom] を計測する。見ている位置は保つ。
    bool EnsureRangeLayout(const Document& doc, LayoutCache& cache, float width, float top, float bottom);

    LayoutEngine& engine_;
    ViewportManager& viewport_;
};
