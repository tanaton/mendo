#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "measure_backend.h"
#include "theme.h"
#include <cstdint>
#include <limits>
#include <memory_resource>

namespace mendo::layout {

// 可視範囲クリップ。top<0 ならクリップなし (全 dirty を対象)。
struct ViewportClip {
    float top = -1.0f;
    float height = -1.0f;
    float buffer_screens = 5.0f;
};

// 1 回の RunSerial で消費可能な予算。0 = 無制限。
struct DirtyBudget {
    int max_nodes = 0;
    int time_us = 0;
};

enum class StopReason : uint8_t {
    NoneDirty,   // dirty が 0 件
    Done,        // 全 dirty を処理しきった
    BatchLimit,  // max_nodes に到達
    TimeBudget,  // time_us を超過
};

struct DirtyBatchResult {
    int processed = 0;
    size_t first_processed = std::numeric_limits<size_t>::max();
    size_t last_processed = 0;
    StopReason reason = StopReason::NoneDirty;
    // viewport buffer 内に未処理 dirty が残ったか (BatchLimit/TimeBudget 中断時のみ true)。
    // Done 時は false。viewport_top<0 (no clip) でも、中断したなら true。
    bool any_nearby_skipped = false;
};

// 「どの dirty ノードを per-node 計測するか」の選定と「いつ止めるか」の予算管理を担う。
// 計測は IMeasureBackend::MeasureNode に委譲する。状態を持たない値型。
class DirtyScheduler {
public:
    // 単一スレッド版: 選定即計測。現状の LayoutEngine::ProcessDirtyBatch と等価。
    DirtyBatchResult RunSerial(std::pmr::vector<Node>& nodes,
                               LayoutCache& cache,
                               float content_width,
                               const Theme& theme,
                               const IMeasureBackend& backend,
                               ViewportClip clip,
                               DirtyBudget budget) const;
};

} // namespace mendo::layout
