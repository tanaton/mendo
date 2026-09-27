#pragma once
#include "dirty_scheduler.h"
#include <span>

class TaskScheduler;

namespace mendo::layout {

// indices のノードを scheduler の worker で並列計測する。scheduler が null か少数なら呼び出しスレッドで直列に計測する。
// 戻り値は例外で計測できなかったノード数。
int MeasureIndicesParallel(
    std::pmr::vector<Node>& nodes,
    LayoutCache& cache,
    float content_width,
    const Theme& theme,
    const IMeasureBackend& backend,
    std::span<const size_t> indices,
    MeasureViewportRange viewport,
    TaskScheduler* scheduler,
    size_t min_parallel = 32);

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
