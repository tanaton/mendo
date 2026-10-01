#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "measure_backend.h"
#include "theme.h"
#include <cstdint>
#include <limits>
#include <memory_resource>

namespace mendo::layout {

// top<0 ならクリップなし。
struct ViewportClip {
    float top = -1.0f;
    float height = -1.0f;
    float buffer_screens = 5.0f;

    constexpr bool active() const noexcept
    {
        return top >= 0.0f && height > 0.0f;
    }

    // dirty 選定で「処理対象として残すべきか」を返す共通述語。range は Range() の結果を渡す。
    static constexpr bool ShouldMeasure(const NodeLayoutEntry& e, float top, MeasureViewportRange range) noexcept
    {
        return e.layout_dirty && !IsOffscreen(top, e.height, range.top, range.bottom);
    }

    // 非クリップ時は ±∞ なので、帯の二分探索・下端 break・オフスクリーン判定がすべて素通りになる。
    constexpr MeasureViewportRange Range() const noexcept
    {
        if (!active()) {
            return {};
        }
        const float buffer = height * buffer_screens;
        return { top - buffer, top + height + buffer };
    }
};

// 0 = 無制限。
struct SerialBudget {
    int max_nodes = 0;
    int time_us = 0;
};

// 1 回の RunParallel で消費可能な予算。並列版は time_budget を持たない:
// Plan/Dispatch/Wait の各 phase に checkpoint を入れる枠組みが無く、worker 側 polling コストが
// 利得を上回るため。time 制御が必要なら呼び出し側で RunSerial にフォールバックすること。
struct ParallelBudget {
    int max_nodes = 0;
};

enum class StopReason : uint8_t {
    NoneDirty,  // dirty が 0 件
    Done,       // 全 dirty を処理しきった
    BatchLimit, // max_nodes に到達
    TimeBudget, // time_us を超過
    Error,      // worker 例外で一部 chunk が未処理 (RunParallel のみ)
};

struct DirtyBatchResult {
    int processed = 0;
    size_t first_processed = std::numeric_limits<size_t>::max();
    size_t last_processed = 0;
    StopReason reason = StopReason::NoneDirty;

    // viewport buffer 内に未処理 dirty が残ったか。viewport_top<0 (no clip) でも、中断したなら true。
    constexpr bool any_nearby_skipped() const noexcept
    {
        return reason == StopReason::BatchLimit || reason == StopReason::TimeBudget || reason == StopReason::Error;
    }
};

DirtyBatchResult RunSerial(
    std::pmr::vector<Node>& nodes,
    LayoutCache& cache,
    float content_width,
    const Theme& theme,
    const IMeasureBackend& backend,
    ViewportClip clip,
    SerialBudget budget);

} // namespace mendo::layout
