#include "layout.h"
#include "document.h"
#include "parallel_for.h"
#include "profiler.h"
#include "task_scheduler.h"
#include "viewport_manager.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory_resource>
#include <ranges>
#include <utility>

namespace layout {

namespace {

// 戻り値は計測できず (例外か、書式やレイアウトを作れなかった) dirty のまま再試行を待つノード数。
// skip が真を返したノードには手を付けない (dirty のまま)。
int MeasureChunk(
    const std::pmr::vector<Node>& nodes,
    LayoutCache& cache,
    float content_width,
    const Theme& theme,
    const ITextMeasurer& backend,
    std::span<const size_t> chunk_indices,
    MeasureViewportRange viewport,
    auto&& skip)
{
    // 例外はノード単位で捕まえる。chunk 単位だと失敗ノードを特定できず、同じ chunk の残りも計測されない。
    // 失敗回数は worker が担当する自エントリにだけ書くので同期は要らない。
    // DiagramEntry は計測中に追加・削除されないので、worker から読むだけなら同期は要らない。
    int failed = 0;
    for (const size_t i : chunk_indices) {
        if (skip()) {
            continue;
        }
        auto& entry = cache[i];
        const DiagramEntry* bitmap = IsBitmapDrawNode(nodes[i]) ? cache.FindDiagram(i) : nullptr;
        bool measured = false;
        try {
            measured = MeasureEntry(backend, nodes[i], entry, NodeContentWidth(nodes[i], theme, content_width), viewport, cache.Top(i), bitmap, theme);
        } catch (...) {
        }
        if (!measured && entry.RecordMeasureFailure(LayoutCache::kMaxMeasureAttempts)) {
            ++failed;
        }
    }
    return failed;
}

} // namespace

int MeasureIndicesParallel(
    const std::pmr::vector<Node>& nodes,
    LayoutCache& cache,
    float content_width,
    const Theme& theme,
    const ITextMeasurer& backend,
    std::span<const size_t> indices,
    MeasureViewportRange viewport,
    TaskScheduler* scheduler,
    LayoutDeadline deadline)
{
    if (!indices.empty()) {
        const auto [lo, hi] = std::ranges::minmax(indices);
        cache.NoteMaterialized(lo, hi);
    }
    std::atomic<int> failed_node_count{ 0 };
    std::atomic<int> skipped_node_count{ 0 };
    std::atomic<size_t> taken{ 0 };
    const auto skip = [&] {
        if (deadline == kNoLayoutDeadline || taken.fetch_add(1, std::memory_order_relaxed) == 0 ||
            std::chrono::steady_clock::now() < deadline) {
            return false;
        }
        skipped_node_count.fetch_add(1, std::memory_order_relaxed);
        return true;
    };
    // 1 ノードの計測 (数十 µs) に比べ chunk 取得の fetch_add は無視できるので、1 ノード単位で配って
    // 1 画面分 (数十ノード) の dirty でも全 worker に行き渡らせる。
    ParallelFor(scheduler, indices.size(), 1, [&](size_t begin, size_t end) {
        MENDO_PROFILE("MeasureNode.chunk");
        const int failed_in_chunk = MeasureChunk(nodes, cache, content_width, theme, backend, indices.subspan(begin, end - begin), viewport, skip);
        if (failed_in_chunk > 0) {
            failed_node_count.fetch_add(failed_in_chunk, std::memory_order_relaxed);
        }
    });
    const int failed = failed_node_count.load(std::memory_order_relaxed);
    if (failed > 0) {
        MENDO_TRACE("MeasureIndicesParallel: node measure threw exception");
    }
    MENDO_PLOT("layout.parallel.error_count", static_cast<int64_t>(failed));
    return failed + skipped_node_count.load(std::memory_order_relaxed);
}

} // namespace layout

using layout::AdvanceNodeY;
using layout::EstimateInvisibleNodeHeight;
using layout::GetSpacingAbove;
using layout::GetSpacingBelow;
using layout::NodeContentWidth;
using layout::RecomputeYPositions;

bool LayoutEngine::Init(ITextMeasurer* measurer, const Theme& theme)
{
    measurer_ = measurer;
    theme_ = &theme;
    measurer_->UpdateTheme(theme);
    return measurer_->RecreateFormats();
}

bool LayoutEngine::RecreateFormats()
{
    if (!measurer_) {
        return false;
    }
    formats_changed_ = last_viewport_width_.has_value();
    return measurer_->RecreateFormats();
}

bool LayoutEngine::Reestimate(const std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width)
{
    MENDO_PROFILE("LayoutEngine::Reestimate");
    const auto node_count = nodes.size();
    cache.Resize(node_count);

    // 初回は幅を覚えるだけ。計測済みのノードがまだ無いので、全ノードを dirty にし直す必要はない。
    const bool width_changed = std::exchange(formats_changed_, false) ||
                               (last_viewport_width_ && std::abs(viewport_width - *last_viewport_width_) > kWidthChangeThreshold);
    if (width_changed || !last_viewport_width_) {
        last_viewport_width_ = viewport_width;
    }
    // 未推定のキャッシュ (位置が全て 0) では可視範囲が決まらないので、幅変更と同じく組み直す。
    const bool unpositioned = node_count > 0 && cache.Bottom(node_count - 1) <= 0.0f;
    // 別の幅やズームでの推定は折り返しの行数が合わず、最初の画面で実際と違う数のノードを計測してしまう。
    const float content_width = theme_->ContentWidth(viewport_width);
    const auto basis = cache.GetEstimateBasis();
    const bool basis_changed = basis.zoom != theme_->zoom || std::abs(basis.content_width - content_width) > kWidthChangeThreshold;
    if (width_changed || unpositioned || basis_changed) {
        EstimateDirtyHeights(nodes, cache, content_width, width_changed);
        cache.SetHasDirtyNodes(true);
        cache.SetEstimateBasis({ content_width, theme_->zoom });
    }
    MENDO_PLOT("layout.compute.width_changed", static_cast<int64_t>(width_changed));
    MENDO_PLOT("layout.compute.node_count", static_cast<int64_t>(node_count));
    return width_changed;
}

void LayoutEngine::EstimateDirtyHeights(const std::pmr::vector<Node>& nodes, LayoutCache& cache, float content_width, bool mark_all_dirty)
{
    MENDO_PROFILE("LayoutEngine::EstimateDirtyHeights");
    float y = theme_->margin_top;
    for (size_t i = 0; i < nodes.size(); i++) {
        const auto& node = nodes[i];
        auto& entry = cache[i];
        if (mark_all_dirty) {
            entry.layout_dirty = true;
        }
        if (entry.layout_dirty) {
            EstimateInvisibleNodeHeight(node, entry, *theme_, NodeContentWidth(node, *theme_, content_width));
        }
        cache.SetTop(i, AdvanceNodeY(y, GetSpacingAbove(node, *theme_), entry.height, GetSpacingBelow(node, *theme_)));
    }
}

LayoutEngine::PassResult LayoutEngine::MeasurePass(
    const std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width, MeasureViewportRange range, LayoutDeadline deadline)
{
    MENDO_PROFILE("LayoutEngine::MeasurePass");
    const float content_width = theme_->ContentWidth(viewport_width);
    PassResult result;
    bool height_changed = false;

    assert(cache.size() == nodes.size());
    const auto node_count = nodes.size();
    const auto lo = static_cast<size_t>(FindFirstVisibleNodeIndex(cache, range.top));
    // height_changed のときだけ意味を持つ。
    size_t last_measured = lo;

    std::pmr::vector<size_t> dirty_indices;
    for (size_t i = lo; i < node_count; i++) {
        auto& entry = cache[i];
        const float entry_top = cache.Top(i);
        if (entry_top > range.bottom) {
            break;
        }
        if (entry.layout_dirty) {
            dirty_indices.push_back(i);
            last_measured = i;
        }
        else if (const auto* tl = entry.table_layout(); tl && tl->HasEvictedRows()) {
            cache.NoteMaterialized(i, i);
            const auto restored = RestoreEntryTableRows(*measurer_, nodes[i], entry, NodeContentWidth(nodes[i], *theme_, content_width), range.ToLocal(entry_top));
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
        const int left = layout::MeasureIndicesParallel(nodes, cache, content_width, *theme_, *measurer_, dirty_indices, range, layout_scheduler_, deadline);
        // 計測できなかったノードと期限で残したノードは、次の呼び出しで計測させる。
        result.remaining |= left > 0;
        height_changed = true;
    }

    if (height_changed) {
        RecomputeYPositions(nodes, cache, *theme_, lo, last_measured);
        result.updated = true;
    }
    MENDO_PLOT("layout.pass.dirty_count", static_cast<int64_t>(dirty_indices.size()));
    return result;
}

bool LayoutEngine::EnsureVisibleLayout(const std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width, float viewport_top, float viewport_bottom)
{
    MENDO_PROFILE("LayoutEngine::EnsureVisibleLayout");
    const MeasureViewportRange range{ viewport_top, viewport_bottom };
    // 計測に失敗し続けるノードがあっても止まるよう回数に上限を設ける。
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

    // 範囲外は走査しないので dirty が残っていると保守的に仮定する。
    const auto node_count = nodes.size();
    const auto vr = ComputeVisibleNodeRange(cache, viewport_top, viewport_bottom);
    const bool has_outside = vr.first > 0 || vr.last_plus_1 < node_count;
    const bool any_dirty = has_outside || std::ranges::any_of(std::views::iota(vr.first, vr.last_plus_1), [&cache](size_t i) {
        return cache[i].layout_dirty;
    });
    cache.SetHasDirtyNodes(any_dirty);
    return true;
}

bool LayoutEngine::ProcessDirtyBatch(
    const std::pmr::vector<Node>& nodes, LayoutCache& cache,
    float viewport_width, LayoutDeadline deadline,
    layout::ViewportClip clip)
{
    MENDO_PROFILE("LayoutEngine::ProcessDirtyBatch");
    // 遠方の dirty はスクロール時に EnsureVisibleLayout で計測する。
    // last_viewport_width_ は Reestimate が再計測を決めた幅のまま据え置く。ここで進めると
    // 2px 未満のリサイズとダーティ処理が交互に続いたとき、可視ノードが古い幅のまま取り残される。
    const bool remaining = MeasurePass(nodes, cache, viewport_width, clip.Range(), deadline).remaining;
    cache.SetHasDirtyNodes(remaining);
    return remaining;
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
        cache.EvictInvisibleTableRows(doc.GetTableNodeIndices(), scroll_y, scroll_y + height, 0.0f);
    }
    engine_.EnsureVisibleLayout(doc.GetNodes(), cache, width, scroll_y, scroll_y + height);
    viewport_.ApplyScrollTarget(cache);
}

bool LayoutService::ProcessDirtyBatch(const Document& doc, LayoutCache& cache, float width, LayoutDeadline deadline, ViewportLimit viewport)
{
    viewport_.EnsureScrollTarget(cache);
    // height <= 0 なら ViewportClip::active() が false になりクリップは無効。
    const bool more = engine_.ProcessDirtyBatch(doc.GetNodes(), cache, width, deadline, { viewport_.GetScrollY(), viewport.height, viewport.buffer_screens });
    viewport_.ApplyScrollTarget(cache);
    return more;
}

bool LayoutService::EnsureRangeLayout(const Document& doc, LayoutCache& cache, float width, float top, float bottom)
{
    viewport_.EnsureScrollTarget(cache);
    const bool updated = engine_.EnsureVisibleLayout(doc.GetNodes(), cache, width, top, bottom);
    viewport_.ApplyScrollTarget(cache);
    return updated;
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

void LayoutService::RecomputeAfterDiagram(const Document& doc, LayoutCache& cache, const Theme& theme,
                                          layout::HeightChangeRange changed) noexcept
{
    // 画面より上の画像・図の高さが変わっても、見ている内容が動かないようにする。
    viewport_.EnsureScrollTarget(cache);
    if (!changed.empty()) {
        RecomputeYPositions(doc.GetNodes(), cache, theme, changed.first, changed.last);
    }
    viewport_.ApplyScrollTarget(cache);
}
