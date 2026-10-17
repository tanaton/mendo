#pragma once
#include "layout_cache.h"
#include "text_measurer.h"
#include <span>

class TaskScheduler;
struct Theme;

namespace layout {

// top<0 ならクリップなし。
struct ViewportClip {
    float top = -1.0f;
    float height = -1.0f;
    float buffer_screens = 0.0f;

    constexpr bool active() const noexcept
    {
        return top >= 0.0f && height > 0.0f;
    }

    // 非クリップ時は ±∞ なので、帯の二分探索・下端 break がすべて素通りになる。
    constexpr MeasureViewportRange Range() const noexcept
    {
        if (!active()) {
            return {};
        }
        const float buffer = height * buffer_screens;
        return { top - buffer, top + height + buffer };
    }
};

// indices のノードを scheduler の worker で並列計測する。scheduler が null なら呼び出しスレッドで直列に計測する。
// 戻り値は例外で計測できなかったノード数。
int MeasureIndicesParallel(
    const std::pmr::vector<Node>& nodes,
    LayoutCache& cache,
    float content_width,
    const Theme& theme,
    const ITextMeasurer& backend,
    std::span<const size_t> indices,
    MeasureViewportRange viewport,
    TaskScheduler* scheduler);

} // namespace layout
