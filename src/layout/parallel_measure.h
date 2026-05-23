#pragma once
#include "dirty_scheduler.h"

class TaskScheduler;

namespace mendo::layout {

DirtyBatchResult RunParallel(
    std::pmr::vector<Node>& nodes,
    LayoutCache& cache,
    float content_width,
    const Theme& theme,
    const IMeasureBackend& backend,
    ViewportClip clip,
    ParallelBudget budget,
    TaskScheduler& scheduler);

} // namespace mendo::layout
