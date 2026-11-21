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
    // 見積もった行高さで evict されていた行を実測し直して、表の高さ (TotalHeight) が変わった。
    bool height_changed = false;
};

// entry に触れずに初めから組んだ表の幾何 (ITextMeasurer::BuildTable)。
struct BuiltTable {
    // 高さは table->TotalHeight()。
    std::unique_ptr<TableLayoutData> table;
    // 寸法がまだ無いインライン数式か画像を仮に組んだ。
    bool pending_inline = false;
};

// 計測は const で、IDWriteTextLayout は per-call 生成、ファクトリと書式はワーカーごとに分ける (DWriteTextMeasurer) ので、
// 同一インスタンスを複数ワーカーから const 経由で叩いて良い。lifecycle 系 (RecreateFormats / SetTheme / SetScheduler) は
// non-const で UI スレッドからのみ呼ぶ。並列計測には const 参照で渡すので、計測中に lifecycle 系は呼べない。
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

// 測れたら組んだ幅 (NodeLayoutWidth) を cached_width に残し、同じ幅へ戻ったときに EstimateInvisibleNodeHeight が
// 推定値で上書きしないようにする。測れなければ何も確定せず、推定の高さを実測値として残さない。
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
    if (result.height_changed) {
        const auto* tl = entry.table_layout();
        entry.CommitMeasured(tl->last_applied_max_width, tl->TotalHeight());
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

// 巨大な表の最初の計測 (全セルの自然幅と行の高さ) を計測ワーカーで行い、UI スレッドを止めない (表は 1 ノードなので遅延
// レイアウトの期限で区切れない)。組み終えるまで表は推定の高さで中身を描かない (極端に大きい表はほぼ無いので、ユーザーが
// よしとした)。組む表のエントリは dirty のまま残し、MeasurePass が飛ばす。文書を差し替えても止めるだけで待たない。
class TableMeasureJobs {
public:
    // セル数がこれを超える表をワーカーで組む。これ以下なら UI スレッドで測っても数十 ms まで (BM_GiantBlock/table5k_first)。
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

    void SetWake(WorkerWake wake)
    {
        wake_ = std::move(wake);
    }
    void SetDocument(std::shared_ptr<const DocumentData> doc);
    bool Defer(std::span<const Node> nodes, size_t node, const NodeLayoutEntry& entry, float max_width);
    void NoteInlineChanged() noexcept;
    std::vector<Result> TakeResults();
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
    WorkerWake wake_;
    WorkerLatch latch_;
};

class LayoutEngine {
public:
    // 小刻みな WM_SIZE で全ノード再レイアウトが頻発するのを防ぐ幅変化の閾値。
    static constexpr float kWidthChangeThreshold = 2.0f;

    bool Init(ITextMeasurer* measurer, const Theme& theme);
    void SetTableJobs(TableMeasureJobs* jobs) noexcept
    {
        table_jobs_ = jobs;
    }

    bool Reestimate(std::span<const Node> nodes, LayoutCache& cache, float viewport_width);
    bool EnsureVisibleLayout(
        std::span<const Node> nodes, LayoutCache& cache, float viewport_width,
        double viewport_top, double viewport_bottom);
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

    // Init は UI スレッドからだけ呼ぶ。MeasureNode は measurer の scheduler の worker から const で並列に呼ばれる。
    ITextMeasurer* measurer_ = nullptr;
    TableMeasureJobs* table_jobs_ = nullptr;
};

// LayoutEngine と ViewportManager を薄く包み、スクロール target を管理しながらレイアウトする。
class LayoutService {
public:
    LayoutService(LayoutEngine& engine, ViewportManager& viewport) noexcept
        : engine_(engine), viewport_(viewport)
    {
    }

    // 推定し直した Y で見ている位置を求めてから、その可視範囲を計測する。
    void ViewportLayout(const Document& doc, LayoutCache& cache, float width, float height);

    LayoutEngine::PassResult ProcessDirtyBatch(
        const Document& doc, LayoutCache& cache, float width, LayoutDeadline deadline,
        float height, float buffer_screens);
    bool EnsureVisibleLayout(const Document& doc, LayoutCache& cache, float width, float height);
    void EnsureNodeLayout(const Document& doc, LayoutCache& cache, float width, int node);
    void RecomputeAfterDiagram(const Document& doc, LayoutCache& cache, IndexRange changed) noexcept;
    struct PlacedTables {
        bool placed = false;
        bool stale_inline = false;
    };
    // ワーカーで組み終えた表 (TableMeasureJobs::TakeResults) を置き、見ている位置を保って Y を組み直す
    PlacedTables PlaceMeasuredTables(const Document& doc, LayoutCache& cache, float width, std::vector<TableMeasureJobs::Result> results);

private:
    template <class Op>
    auto KeepingAnchor(LayoutCache& cache, Op&& op);
    bool EnsureRangeLayout(const Document& doc, LayoutCache& cache, float width, double top, double bottom);

    LayoutEngine& engine_;
    ViewportManager& viewport_;
};
