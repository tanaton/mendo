#include "dirty_scheduler.h"
#include "layout_computer.h"
#include "profiler.h"
#include <chrono>

namespace mendo::layout {

DirtyBatchResult DirtyScheduler::RunSerial(std::pmr::vector<Node>& nodes,
                                           LayoutCache& cache,
                                           float content_width,
                                           const Theme& theme,
                                           const IMeasureBackend& backend,
                                           ViewportClip clip,
                                           DirtyBudget budget) const
{
    MENDO_PROFILE("DirtyScheduler::RunSerial");
    DirtyBatchResult result;
    const auto node_count = nodes.size();

    const bool has_viewport_limit = (clip.top >= 0.0f && clip.height > 0.0f);
    const float limit_top = has_viewport_limit ? clip.top - clip.height * clip.buffer_screens : 0.0f;
    const float limit_bottom = has_viewport_limit ? clip.top + clip.height + clip.height * clip.buffer_screens : 0.0f;

    const bool has_time_budget = (budget.time_us > 0);
    const bool has_batch_limit = (budget.max_nodes > 0);
    const auto start = has_time_budget ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    for (size_t i = 0; i < node_count; i++) {
        auto& entry = cache[i];
        if (!entry.layout_dirty) {
            continue;
        }

        if (has_viewport_limit && IsOffscreen(entry.text_top, entry.height, limit_top, limit_bottom)) {
            continue;
        }

        // MeasureNode の前に判定し超過分を抑えるが、進行保証のため最低1ノードは処理する。
        if (has_time_budget && result.processed > 0) {
            const auto elapsed = std::chrono::steady_clock::now() - start;
            if (std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() >= budget.time_us) {
                result.reason = StopReason::TimeBudget;
                result.any_nearby_skipped = true;
                break;
            }
        }

        if (result.first_processed == std::numeric_limits<size_t>::max()) {
            result.first_processed = i;
        }
        const float indent = NodeIndent(nodes[i], theme);
        backend.MeasureNode(nodes[i], entry, content_width - indent);
        result.last_processed = i;
        ++result.processed;

        if (has_batch_limit && result.processed >= budget.max_nodes) {
            result.reason = StopReason::BatchLimit;
            result.any_nearby_skipped = true;
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
