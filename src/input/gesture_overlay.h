#pragma once
#include "mouse_gesture.h"
#include "swipe_detector.h"
#include "render_params.h"

inline GestureRenderState ResolveGestureOverlay(
    const MouseGesture& gesture,
    const SwipeDetector& swipe) noexcept
{
    GestureRenderState gs;
    gs.trail_active = gesture.IsGestureActive();
    gs.trail_points = &gesture.GetTrailPoints();
    // マウスジェスチャーを優先する。None ならオーバーレイは出さない。
    gs.direction = gesture.IsOverlayVisible() ? gesture.GetDirection() : swipe.GetOverlayDirection();
    return gs;
}
