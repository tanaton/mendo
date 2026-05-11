#pragma once
#include "mouse_gesture.h"
#include "swipe_detector.h"
#include "render_params.h"

// マウスジェスチャ非アクティブ時はタッチパッドスワイプ用オーバーレイにフォールバックする。
inline GestureRenderState ResolveGestureOverlay(
    const MouseGesture& gesture,
    const SwipeDetector& swipe) noexcept
{
    GestureRenderState gs;
    gs.trail_active = gesture.IsGestureActive();
    gs.trail_points = &gesture.GetTrailPoints();
    gs.overlay_visible = gesture.IsOverlayVisible();
    gs.direction =
        (gesture.GetDirection() == GestureDirection::Left)    ? -1
        : (gesture.GetDirection() == GestureDirection::Right) ? 1
                                                              : 0;
    gs.overlay_alpha = gesture.GetOverlayAlpha();

    if (!gs.overlay_visible && swipe.IsOverlayVisible()) {
        gs.overlay_visible = true;
        gs.direction = swipe.GetOverlayDirection();
        gs.overlay_alpha = swipe.GetOverlayAlpha();
    }
    return gs;
}
