#include "parallel_measure.h"
#include "layout_computer.h"
#include "parallel_for.h"
#include "profiler.h"
#include "task_scheduler.h"
#include <algorithm>
#include <atomic>
#include <windows.h>

namespace layout {

namespace {

// 戻り値は例外で計測できず dirty のまま再試行を待つノード数。
int MeasureChunk(
    const std::pmr::vector<Node>& nodes,
    LayoutCache& cache,
    float content_width,
    const Theme& theme,
    const ITextMeasurer& backend,
    std::span<const size_t> chunk_indices,
    MeasureViewportRange viewport)
{
    // 例外はノード単位で捕まえる。chunk 単位だと失敗ノードを特定できず、同じ chunk の残りも計測されない。
    // 失敗回数は worker が担当する自エントリにだけ書くので同期は要らない。
    // DiagramEntry は計測中に追加・削除されないので、worker から読むだけなら同期は要らない。
    int failed = 0;
    for (const size_t i : chunk_indices) {
        auto& entry = cache[i];
        const DiagramEntry* bitmap = IsBitmapDrawNode(nodes[i]) ? cache.FindDiagram(i) : nullptr;
        try {
            MeasureEntry(backend, nodes[i], entry, NodeContentWidth(nodes[i], theme, content_width), viewport, cache.Top(i), bitmap, theme);
        } catch (...) {
            // 諦めた後に再び dirty になって失敗し続けても一周しないよう、上限で頭打ちにする。
            if (entry.measure_failures < LayoutCache::kMaxMeasureAttempts) {
                ++entry.measure_failures;
            }
            if (entry.measure_failures >= LayoutCache::kMaxMeasureAttempts) {
                entry.layout_dirty = false;
            }
            else {
                ++failed;
            }
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
    MeasureViewportRange measure_vp,
    TaskScheduler* scheduler)
{
    if (!indices.empty()) {
        const auto [lo, hi] = std::ranges::minmax(indices);
        cache.NoteMaterialized(lo, hi);
    }
    std::atomic<int> failed_node_count{ 0 };
    // 1 ノードの計測 (数十 µs) に比べ chunk 取得の fetch_add は無視できるので、1 ノード単位で配って
    // 1 画面分 (数十ノード) の dirty でも全 worker に行き渡らせる。
    ParallelFor(scheduler, indices.size(), 1, [&](size_t begin, size_t end) {
        MENDO_PROFILE("MeasureNode.chunk");
        const int failed_in_chunk = MeasureChunk(nodes, cache, content_width, theme, backend, indices.subspan(begin, end - begin), measure_vp);
        if (failed_in_chunk > 0) {
            failed_node_count.fetch_add(failed_in_chunk, std::memory_order_relaxed);
        }
    });
    const int failed = failed_node_count.load(std::memory_order_relaxed);
    if (failed > 0) {
        MENDO_TRACE("MeasureIndicesParallel: node measure threw exception");
    }
    MENDO_PLOT("layout.parallel.error_count", static_cast<int64_t>(failed));
    return failed;
}

} // namespace layout
