#include <gtest/gtest.h>
#include "gesture_overlay.h"

TEST(GestureOverlay, Idle)
{
    MouseGesture gesture;
    SwipeDetector swipe;
    auto gs = ResolveGestureOverlay(gesture, swipe);
    EXPECT_FALSE(gs.trail_active);
    EXPECT_FALSE(gs.overlay_visible);
    EXPECT_EQ(gs.direction, 0);
    ASSERT_NE(gs.trail_points, nullptr);
    EXPECT_TRUE(gs.trail_points->empty());
}

TEST(GestureOverlay, TrackingLeft)
{
    MouseGesture gesture;
    SwipeDetector swipe;
    gesture.OnRButtonDown(100.0f, 100.0f);
    gesture.OnMouseMove(20.0f, 100.0f); // 左に80px移動

    auto gs = ResolveGestureOverlay(gesture, swipe);
    EXPECT_TRUE(gs.trail_active);
    EXPECT_TRUE(gs.overlay_visible);
    EXPECT_EQ(gs.direction, -1);
}

TEST(GestureOverlay, TrackingRight)
{
    MouseGesture gesture;
    SwipeDetector swipe;
    gesture.OnRButtonDown(100.0f, 100.0f);
    gesture.OnMouseMove(200.0f, 100.0f);

    auto gs = ResolveGestureOverlay(gesture, swipe);
    EXPECT_EQ(gs.direction, 1);
}

TEST(GestureOverlay, SwipeFallback_WhenGestureIdle)
{
    MouseGesture gesture;
    SwipeDetector swipe;
    swipe.OnHWheel(500, 1000);

    auto gs = ResolveGestureOverlay(gesture, swipe);
    EXPECT_FALSE(gs.trail_active);
    EXPECT_TRUE(gs.overlay_visible);
    EXPECT_EQ(gs.direction, -1);
}

TEST(GestureOverlay, GestureTakesPrecedenceOverSwipe)
{
    MouseGesture gesture;
    SwipeDetector swipe;
    gesture.OnRButtonDown(100.0f, 100.0f);
    gesture.OnMouseMove(200.0f, 100.0f);
    swipe.OnHWheel(-500, 1000);

    auto gs = ResolveGestureOverlay(gesture, swipe);
    EXPECT_TRUE(gs.overlay_visible);
    EXPECT_EQ(gs.direction, 1);
}
