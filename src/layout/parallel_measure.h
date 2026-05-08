#pragma once
#include "dirty_scheduler.h"

class TaskScheduler;

namespace mendo::layout {

// per-node 並列レイアウト計測。MeasureNode を TaskScheduler に分散実行する。
// CodeBlock の Tokenize 結果は UI スレッドで node_index 昇順に集約後 Node に書き戻す
// (Node 副作用の race を回避)。
// 小規模 dirty (< kMinDirtyForParallel) は dispatch コストを避けて inline 直列化。
// budget が ParallelBudget 型なのは「time_budget は並列版では機能しない」を型レベルで明示するため。
DirtyBatchResult RunParallel(std::pmr::vector<Node>& nodes,
                             LayoutCache& cache,
                             float content_width,
                             const Theme& theme,
                             const IMeasureBackend& backend,
                             ViewportClip clip,
                             ParallelBudget budget,
                             TaskScheduler& scheduler);

} // namespace mendo::layout
