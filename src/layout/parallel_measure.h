#pragma once
#include "layout_cache.h"
#include "measure_backend.h"
#include <cstdint>
#include <limits>
#include <span>

class TaskScheduler;
struct Theme;

namespace mendo::layout {

// top<0 ならクリップなし。
struct ViewportClip {
    float top = -1.0f;
    float height = -1.0f;
    float buffer_screens = 0.0f;

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

enum class StopReason : uint8_t {
    NoneDirty,  // dirty が 0 件
    Done,       // 全 dirty を処理しきった
    BatchLimit, // max_nodes に到達
    Error,      // 例外で一部ノードが未計測
};

struct DirtyBatchResult {
    int processed = 0;
    size_t first_processed = std::numeric_limits<size_t>::max();
    size_t last_processed = 0;
    StopReason reason = StopReason::NoneDirty;

    // viewport buffer 内に未処理 dirty が残ったか。viewport_top<0 (no clip) でも、中断したなら true。
    constexpr bool any_nearby_skipped() const noexcept
    {
        return reason == StopReason::BatchLimit || reason == StopReason::Error;
    }
};

// indices のノードを scheduler の worker で並列計測する。scheduler が null なら呼び出しスレッドで直列に計測する。
// 戻り値は例外で計測できなかったノード数。
int MeasureIndicesParallel(
    std::pmr::vector<Node>& nodes,
    LayoutCache& cache,
    float content_width,
    const Theme& theme,
    const IMeasureBackend& backend,
    std::span<const size_t> indices,
    MeasureViewportRange viewport,
    TaskScheduler* scheduler);

// clip 内の dirty を最大 max_nodes 件 (0 = 無制限) 計測する。scheduler が null なら呼び出しスレッドで直列に計測する。
// worker 側に checkpoint が無いので時間予算は持たない。
DirtyBatchResult RunParallel(
    std::pmr::vector<Node>& nodes,
    LayoutCache& cache,
    float content_width,
    const Theme& theme,
    const IMeasureBackend& backend,
    ViewportClip clip,
    int max_nodes,
    TaskScheduler* scheduler);

} // namespace mendo::layout
