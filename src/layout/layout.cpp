#include "layout.h"
#include "document.h"
#include "task_scheduler.h"
#include "profiler.h"
#include "utility.h"
#include "viewport_manager.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <ranges>

namespace layout {

int MeasureIndicesParallel(
    std::span<const Node> nodes,
    LayoutCache& cache,
    float content_width,
    const ITextMeasurer& backend,
    std::span<const size_t> indices,
    LayoutDeadline deadline)
{
    assert(std::ranges::is_sorted(indices));
    const Theme& theme = backend.GetTheme();
    if (!indices.empty()) {
        cache.NoteMaterialized(indices.front(), indices.back() + 1);
    }
    // 1 ノードの計測 (数百 µs) に比べ worker を起こす手間 (8 本で約 30µs) と chunk の取得は小さいので、1 ノード単位で配って
    // 1 画面分 (数十ノード) の dirty でも全 worker に行き渡らせる。scheduler が無ければ全体が 1 chunk になる。
    std::atomic<int> left{ 0 };
    ParallelFor(backend.Scheduler(), indices.size(), 1, [&](size_t begin, size_t end) {
        MENDO_PROFILE("MeasureNode.chunk");
        for (const size_t i : indices.subspan(begin, end - begin)) {
            if (i != indices.front() && std::chrono::steady_clock::now() >= deadline) {
                left.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            auto& entry = cache[i];
            // DiagramEntry は計測中に追加・削除されないので、worker から読むだけなら同期は要らない。
            const DiagramEntry* diagram = nodes[i].type == NodeType::Diagram ? cache.FindDiagram(i) : nullptr;
            // 例外はノード単位で捕まえ、同じ chunk の残りも計測する。
            try {
                MeasureEntry(backend, nodes[i], entry, NodeContentWidth(nodes[i], theme, content_width), diagram);
            } catch (...) {
                MENDO_TRACE("MeasureIndicesParallel: node measure threw exception");
            }
            // 測れなかったノードも再試行しない。失敗の実体はレイアウトや確保の失敗で数 ms では回復せず、残すと描画のたびに
            // 測り直して失敗し続ける。次の幅変更やズームで改めて試す。
            entry.layout_dirty = false;
        }
    });
    MENDO_PLOT("layout.parallel.left_count", static_cast<int64_t>(left.load(std::memory_order_relaxed)));
    return left.load(std::memory_order_relaxed);
}

} // namespace layout

using layout::NodeContentWidth;
using layout::RecomputeYPositions;

void TableMeasureJobs::StopRunning() noexcept
{
    for (auto& r : running_) {
        r.stop.request_stop();
    }
    running_.clear();
}

void TableMeasureJobs::SetDocument(std::shared_ptr<const DocumentData> doc)
{
    StopRunning();
    doc_ = std::move(doc);
}

bool TableMeasureJobs::Defer(std::span<const Node> nodes, size_t node, const NodeLayoutEntry& entry, float max_width)
{
    const NodeTableData* tbl = nodes[node].table_data();
    if (!tbl || entry.table_layout() || static_cast<size_t>(tbl->row_count) * tbl->col_count <= kMinCells) {
        return false;
    }
    // 今の文書のノードでなければ (SetDocument の前) その場で測らせる。
    if (!doc_ || std::span<const Node>{ doc_->parsed.nodes }.data() != nodes.data()) {
        return false;
    }
    if (std::ranges::contains(running_, node, &Running::node)) {
        return true;
    }
    Running& r = running_.emplace_back(Running{ .job = ++last_job_, .node = node });
    const bool posted = scheduler_.Post([this, doc = doc_, node, max_width, job = r.job, stop = r.stop.get_token(), guard = latch_.Acquire()] {
        if (stop.stop_requested()) {
            return;
        }
        Result result{ .job = job, .node = node };
        try {
            result.built = measurer_.BuildTable(std::span<const Node>{ doc->parsed.nodes }[node], max_width, stop);
            if (stop.stop_requested()) {
                return;
            }
        } catch (...) {
            MENDO_TRACE("TableMeasureJobs: BuildTable threw exception");
        }
        // 測れなかった表 (table が null) も知らせ、推定の高さのまま dirty を下ろさせる (UI スレッドで測るときと同じく
        // 再試行しない)。
        if (results_.Push(std::move(result)) && wake_) {
            wake_();
        }
    });
    if (!posted) {
        running_.pop_back();
        return false;
    }
    return true;
}

void TableMeasureJobs::NoteInlineChanged() noexcept
{
    for (auto& r : running_) {
        r.inline_changed = true;
    }
}

std::vector<TableMeasureJobs::Result> TableMeasureJobs::TakeResults()
{
    std::vector<Result> results = results_.TakeAll();
    std::vector<Result> current;
    for (auto& result : results) {
        const auto it = std::ranges::find(running_, result.job, &Running::job);
        if (it == running_.end()) {
            continue;
        }
        result.inline_changed = it->inline_changed;
        running_.erase(it);
        current.push_back(std::move(result));
    }
    return current;
}

void TableMeasureJobs::CancelAll()
{
    StopRunning();
    // 前の文書の組み (SetDocument で止めただけのもの) も待つ。
    latch_.Wait();
    // 止める前に組み終えていた分も捨てる。
    results_.TakeAll();
}

bool LayoutEngine::Init(ITextMeasurer* measurer, const Theme& theme)
{
    measurer_ = measurer;
    measurer_->SetTheme(theme);
    return measurer_->RecreateFormats();
}

bool LayoutEngine::Reestimate(std::span<const Node> nodes, LayoutCache& cache, float viewport_width)
{
    MENDO_PROFILE("LayoutEngine::Reestimate");
    assert(cache.size() == nodes.size());

    // 別の幅やズームでの推定は折り返しの行数が合わず、最初の画面で実際と違う数のノードを計測してしまう。
    // 幅を知らずに推定したキャッシュ (EstimateBasis が 0) も、ここで表示幅に合わせて推定し直す。
    const Theme& theme = GetTheme();
    const float content_width = theme.ContentWidth(viewport_width);
    const auto basis = cache.GetEstimateBasis();
    const bool basis_changed = basis.zoom != theme.zoom || std::abs(basis.content_width - content_width) > kWidthChangeThreshold;
    if (basis_changed) {
        layout::ReestimateNodeHeights(nodes, cache, theme, content_width);
        cache.SetHasDirtyNodes(true);
        cache.SetEstimateBasis({ content_width, theme.zoom });
    }
    MENDO_PLOT("layout.compute.width_changed", static_cast<int64_t>(basis_changed));
    MENDO_PLOT("layout.compute.node_count", static_cast<int64_t>(nodes.size()));
    return basis_changed;
}

LayoutEngine::PassResult LayoutEngine::MeasurePass(
    std::span<const Node> nodes, LayoutCache& cache, float viewport_width, YRange range, LayoutDeadline deadline)
{
    MENDO_PROFILE("LayoutEngine::MeasurePass");
    const Theme& theme = GetTheme();
    const float content_width = theme.ContentWidth(viewport_width);
    PassResult result;
    bool height_changed = false;

    assert(cache.size() == nodes.size());
    const auto node_count = nodes.size();
    const auto lo = static_cast<size_t>(FindFirstVisibleNodeIndex(cache, range.top));
    // height_changed のときだけ意味を持つ。
    size_t last_measured = lo;

    std::vector<size_t> dirty_indices;
    for (size_t i = lo; i < node_count; i++) {
        auto& entry = cache[i];
        const float entry_top = cache.Top(i);
        if (entry_top > range.bottom) {
            break;
        }
        if (entry.layout_dirty) {
            // 巨大な表はワーカーで組ませ、組み終えるまで dirty のまま飛ばす (TableMeasureJobs)。
            if (table_jobs_ && table_jobs_->Defer(nodes, i, entry, NodeContentWidth(nodes[i], theme, content_width))) {
                continue;
            }
            dirty_indices.push_back(i);
            last_measured = i;
        }
        else if (const auto* tl = entry.table_layout(); tl && tl->HasEvictedRows()) {
            cache.NoteMaterialized(i, i + 1);
            const auto restored = RestoreEntryTableRows(*measurer_, nodes[i], entry, range.ToLocal(entry_top));
            result.updated |= restored.restored;
            if (restored.height) {
                height_changed = true;
                last_measured = i;
            }
        }
    }

    if (!dirty_indices.empty()) {
        // 未計測領域へのジャンプやスクロールバードラッグでは 1 画面分 (数十〜百ノード) を
        // 毎フレーム計測するため、scheduler があれば並列化する。
        const int left = layout::MeasureIndicesParallel(nodes, cache, content_width, *measurer_, dirty_indices, deadline);
        // 期限で残したノードは、次の呼び出しで計測させる。
        result.remaining |= left > 0;
        height_changed = true;
    }

    if (height_changed) {
        RecomputeYPositions(nodes, cache, theme, lo, last_measured);
        result.updated = true;
    }
    MENDO_PLOT("layout.pass.dirty_count", static_cast<int64_t>(dirty_indices.size()));
    return result;
}

bool LayoutEngine::EnsureVisibleLayout(std::span<const Node> nodes, LayoutCache& cache, float viewport_width, float viewport_top, float viewport_bottom)
{
    MENDO_PROFILE("LayoutEngine::EnsureVisibleLayout");
    const YRange range{ viewport_top, viewport_bottom };
    // 測ったノードが推定より縮むと次のノードが範囲に入るので繰り返す。1 回の手間に上限を設ける。
    constexpr int kMaxMeasurePasses = 8;
    bool updated = false;
    for (int pass = 0; pass < kMaxMeasurePasses; ++pass) {
        if (!MeasurePass(nodes, cache, viewport_width, range, kNoLayoutDeadline).updated) {
            break;
        }
        updated = true;
    }
    if (!updated) {
        return false;
    }

    // 範囲外は走査しないので dirty が残っていると保守的に仮定する。見えている表の範囲外の退避した行も同じで、残さないと
    // 大きな表だけの文書では先読みが走らず、めくるたびに描画の中で行を戻す (2 万行 6 列で PageDown 1 回 0.3〜0.4ms)。
    const auto node_count = nodes.size();
    const auto vr = ComputeVisibleNodeRange(cache, viewport_top, viewport_bottom);
    const bool has_outside = vr.begin > 0 || vr.end < node_count;
    const bool any_dirty = has_outside || std::ranges::any_of(std::views::iota(vr.begin, vr.end), [&cache](size_t i) {
        const auto* table = cache[i].table_layout();
        return cache[i].layout_dirty || (table && table->HasEvictedRows());
    });
    cache.SetHasDirtyNodes(any_dirty);
    return true;
}

LayoutEngine::PassResult LayoutEngine::ProcessDirtyBatch(
    std::span<const Node> nodes, LayoutCache& cache,
    float viewport_width, LayoutDeadline deadline,
    YRange range)
{
    MENDO_PROFILE("LayoutEngine::ProcessDirtyBatch");
    // 遠方の dirty はスクロール時に EnsureVisibleLayout で計測する。
    // 推定の幅 (EstimateBasis) は Reestimate が再計測を決めた幅のまま据え置く。ここで進めると
    // 2px 未満のリサイズとダーティ処理が交互に続いたとき、可視ノードが古い幅のまま取り残される。
    const PassResult result = MeasurePass(nodes, cache, viewport_width, range, deadline);
    cache.SetHasDirtyNodes(result.remaining);
    return result;
}

template <class Op>
auto LayoutService::KeepingAnchor(LayoutCache& cache, Op&& op)
{
    viewport_.EnsureScrollTarget(cache);
    // op の結果を返してから当て直す (op が値を返さなくてもよい)。
    const ScopeGuard apply([&]() noexcept { viewport_.ApplyScrollTarget(cache); });
    return op();
}

void LayoutService::ViewportLayout(const Document& doc, LayoutCache& cache, float width, float height)
{
    const bool width_changed = engine_.Reestimate(doc.GetNodes(), cache, width);
    // ズームや幅変更で推定し直すと Y がずれるので、計測の前に見ていた位置を当て直す。
    // 古い scroll_y のまま計測すると、見えない範囲を 1 画面分ムダに計測してしまう。
    viewport_.ApplyScrollTarget(cache);
    const float scroll_y = viewport_.GetScrollY();
    if (width_changed) {
        // 幅が変わると、残っている表の行 (上下 8 画面分) は全部組み直しになる。画面の外の行は捨てて、見えたときに作らせる
        // (2 万行 6 列の表の中で幅を変えたときの可視範囲のレイアウトが 3.7ms → 0.55ms)。
        cache.EvictInvisibleTableRows(doc.GetTableNodeIndices(), layout::BufferedRange(scroll_y, height, 0.0f));
    }
    engine_.EnsureVisibleLayout(doc.GetNodes(), cache, width, scroll_y, scroll_y + height);
    viewport_.ApplyScrollTarget(cache);
}

LayoutEngine::PassResult LayoutService::ProcessDirtyBatch(const Document& doc, LayoutCache& cache, float width, LayoutDeadline deadline, float height, float buffer_screens)
{
    return KeepingAnchor(cache, [&] {
        // 高さ 0 (ウィンドウをタイトルバーまで縮めた) でも全域にしない。巨大な表の退避した行を全部戻してしまう。
        return engine_.ProcessDirtyBatch(doc.GetNodes(), cache, width, deadline, layout::BufferedRange(viewport_.GetScrollY(), height, buffer_screens));
    });
}

bool LayoutService::EnsureRangeLayout(const Document& doc, LayoutCache& cache, float width, float top, float bottom)
{
    return KeepingAnchor(cache, [&] { return engine_.EnsureVisibleLayout(doc.GetNodes(), cache, width, top, bottom); });
}

bool LayoutService::EnsureVisibleLayout(const Document& doc, LayoutCache& cache, float width, float height)
{
    const float scroll_y = viewport_.GetScrollY();
    return EnsureRangeLayout(doc, cache, width, scroll_y, scroll_y + height);
}

bool LayoutService::EnsureNodeLayout(const Document& doc, LayoutCache& cache, float width, int node)
{
    if (node < 0 || static_cast<size_t>(node) >= cache.size() || !cache[node].layout_dirty) {
        return false;
    }
    const float top = cache.Top(static_cast<size_t>(node));
    return EnsureRangeLayout(doc, cache, width, top, top);
}

void LayoutService::RecomputeAfterDiagram(const Document& doc, LayoutCache& cache, IndexRange changed) noexcept
{
    // 画面より上の図の高さが変わっても、見ている内容が動かないようにする。
    KeepingAnchor(cache, [&] {
        RecomputeYPositions(doc.GetNodes(), cache, engine_.GetTheme(), changed.begin, changed.end - 1);
    });
}

LayoutService::PlacedTables LayoutService::PlaceMeasuredTables(const Document& doc, LayoutCache& cache, float width, std::vector<TableMeasureJobs::Result> results)
{
    MENDO_PROFILE("LayoutService::PlaceMeasuredTables");
    PlacedTables placed;
    if (results.empty()) {
        return placed;
    }
    const auto nodes = doc.GetNodes();
    const Theme& theme = engine_.GetTheme();
    const float content_width = theme.ContentWidth(width);
    // 画面より上の表の高さが変わっても、見ている内容が動かないようにする。
    KeepingAnchor(cache, [&] {
        IndexRange committed;
        for (auto& r : results) {
            auto& entry = cache[r.node];
            // 組む間に dirty でなくなるのは、別の経路で測ったときだけ (今は無い)。
            if (!entry.layout_dirty || entry.table_layout()) {
                continue;
            }
            placed.placed = true;
            if (!r.built.table) {
                entry.layout_dirty = false;
                continue;
            }
            const Node& node = nodes[r.node];
            const float node_width = NodeContentWidth(node, theme, content_width);
            // 届く前の寸法で組んだかもしれない数式と画像は、寸法を待って仮に組んだものとして組み直させる。
            const bool stale = r.inline_changed && (node.has_images() || std::ranges::binary_search(doc.GetMathNodeIndices(), r.node));
            entry.pending_inline = r.built.pending_inline || stale;
            placed.stale_inline |= stale;
            const bool current_width = r.built.table->GeometryValidAt(node_width);
            entry.set_table_layout(std::move(r.built.table));
            cache.NoteMaterialized(r.node, r.node + 1);
            if (current_width) {
                // 推定の高さのまま表の中を見ていたら、表の中の同じ割合の位置を見せる。推定は折り返しを少なく見積もるので、
                // ピクセルの位置を保つと別の行へずれる (リロードの差分の位置 CalcScrollYForDiff も、表の中は割合で決めている)。
                if (const ScrollTarget target = viewport_.GetScrollTarget(); target.node == static_cast<int>(r.node) && entry.height > 0.0f) {
                    viewport_.SetScrollTarget(target.node, target.offset * r.built.height / entry.height);
                }
                entry.CommitMeasured(layout::NodeLayoutWidth(node, node_width), r.built.height);
                committed.Add(r.node);
            }
            else {
                cache.SetHasDirtyNodes(true);
            }
        }
        if (!committed.empty()) {
            RecomputeYPositions(nodes, cache, theme, committed.begin, committed.end - 1);
        }
    });
    return placed;
}
