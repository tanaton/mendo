#include "parallel_measure.h"
#include "layout_computer.h"
#include "parallel_for.h"
#include "profiler.h"
#include "task_scheduler.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <windows.h>

namespace mendo::layout {

namespace {

// 戻り値は例外で計測できず dirty のまま再試行を待つノード数。
int MeasureChunk(
    std::pmr::vector<Node>& nodes,
    LayoutCache& cache,
    float content_width,
    const Theme& theme,
    const IMeasureBackend& backend,
    std::span<const size_t> chunk_indices,
    MeasureViewportRange viewport)
{
    // 例外はノード単位で捕まえる。chunk 単位だと失敗ノードを特定できず、同じ chunk の残りも計測されない。
    // 失敗回数は worker が担当する自エントリにだけ書くので同期は要らない。
    int failed = 0;
    for (const size_t i : chunk_indices) {
        auto& entry = cache[i];
        const float indent = NodeIndent(nodes[i], theme);
        try {
            MeasureEntry(backend, nodes[i], entry, content_width - indent, viewport, cache.Top(i));
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
    std::pmr::vector<Node>& nodes,
    LayoutCache& cache,
    float content_width,
    const Theme& theme,
    const IMeasureBackend& backend,
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
        OutputDebugStringW(L"[mendo] MeasureIndicesParallel: node measure threw exception\n");
    }
    MENDO_PLOT("layout.parallel.error_count", static_cast<int64_t>(failed));
    return failed;
}

DirtyBatchResult RunParallel(
    std::pmr::vector<Node>& nodes,
    LayoutCache& cache,
    float content_width,
    const Theme& theme,
    const IMeasureBackend& backend,
    ViewportClip clip,
    int max_nodes,
    TaskScheduler* scheduler)
{
    MENDO_PROFILE("layout::RunParallel");
    DirtyBatchResult result;
    const auto node_count = nodes.size();
    const MeasureViewportRange range = clip.Range();
    const bool has_batch_limit = (max_nodes > 0);

    std::pmr::vector<size_t> indices;
    {
        MENDO_PROFILE("RunParallel.Plan");
        // 上限が無ければ最悪ケース (全ノード dirty) で予約し、push_back 中の再確保を避ける。
        indices.reserve(has_batch_limit ? std::min(node_count, static_cast<size_t>(max_nodes)) : node_count);
        // text_top は単調なので、帯の開始は二分探索で求め、下端超過で break する。
        // 全走査 + reserve(node_count) は 100MB 級文書で 16ms タイマーごとに
        // 数 MB の確保と全エントリ読みを繰り返してしまう。
        const auto plan_begin = static_cast<size_t>(FindFirstVisibleNodeIndex(cache, node_count, range.top));
        for (size_t i = plan_begin; i < node_count; i++) {
            const float entry_top = cache.Top(i);
            if (entry_top > range.bottom) {
                break;
            }
            if (!ViewportClip::ShouldMeasure(cache[i], entry_top, range)) {
                continue;
            }
            indices.push_back(i);
            if (has_batch_limit && static_cast<int>(indices.size()) >= max_nodes) {
                result.reason = StopReason::BatchLimit;
                break;
            }
        }
    }

    if (indices.empty()) {
        result.reason = StopReason::NoneDirty;
        return result;
    }

    if (result.reason == StopReason::NoneDirty) {
        result.reason = StopReason::Done;
    }
    result.first_processed = indices.front();
    result.last_processed = indices.back();
    result.processed = static_cast<int>(indices.size());

    const int failed = MeasureIndicesParallel(nodes, cache, content_width, theme, backend, indices, range, scheduler);
    if (failed > 0) {
        // 失敗分は processed から外し、any_nearby_skipped() 経由で次フレーム再試行に乗せる。
        result.processed -= failed;
        if (result.reason == StopReason::Done) {
            result.reason = StopReason::Error;
        }
    }

    MENDO_PLOT("layout.parallel.dirty_count", static_cast<int64_t>(result.processed));
    return result;
}

} // namespace mendo::layout
