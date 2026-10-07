#include "dirty_scheduler.h"
#include "layout_computer.h"
#include "profiler.h"
#include <chrono>

namespace mendo::layout {

DirtyBatchResult RunSerial(
    std::pmr::vector<Node>& nodes,
    LayoutCache& cache,
    float content_width,
    const Theme& theme,
    const IMeasureBackend& backend,
    ViewportClip clip,
    SerialBudget budget)
{
    MENDO_PROFILE("layout::RunSerial");
    DirtyBatchResult result;
    const auto node_count = nodes.size();
    const MeasureViewportRange range = clip.Range();

    const bool has_time_budget = (budget.time_us > 0);
    const bool has_batch_limit = (budget.max_nodes > 0);
    const auto start = has_time_budget ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    // text_top は単調なので、帯の開始を二分探索し下端超過で break する
    const auto plan_begin = static_cast<size_t>(FindFirstVisibleNodeIndex(cache, node_count, range.top));
    for (size_t i = plan_begin; i < node_count; i++) {
        auto& entry = cache[i];
        const float entry_top = cache.Top(i);
        if (entry_top > range.bottom) {
            break;
        }
        if (!ViewportClip::ShouldMeasure(entry, entry_top, range)) {
            continue;
        }

        // MeasureNode の前に判定し超過分を抑えるが、進行保証のため最低1ノードは処理する。
        if (has_time_budget && result.processed > 0) {
            const auto elapsed = std::chrono::steady_clock::now() - start;
            if (std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() >= budget.time_us) {
                result.reason = StopReason::TimeBudget;
                break;
            }
        }

        if (result.first_processed == std::numeric_limits<size_t>::max()) {
            result.first_processed = i;
        }
        const float indent = NodeIndent(nodes[i], theme);
        cache.NoteMaterialized(i);
        MeasureEntry(backend, nodes[i], entry, content_width - indent, nullptr, range, entry_top);
        result.last_processed = i;
        ++result.processed;

        if (has_batch_limit && result.processed >= budget.max_nodes) {
            result.reason = StopReason::BatchLimit;
            break;
        }
    }

    if (result.reason == StopReason::NoneDirty && result.processed > 0) {
        result.reason = StopReason::Done;
    }
    MENDO_PLOT("layout.dirty_batch.processed", static_cast<int64_t>(result.processed));
    return result;
}

} // namespace mendo::layout
