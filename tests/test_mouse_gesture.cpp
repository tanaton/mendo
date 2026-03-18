#include <gtest/gtest.h>
#include "mouse_gesture.h"

class MouseGestureTest : public ::testing::Test {
protected:
    MouseGesture gesture_;
};

// ─── Initial state ───

TEST_F(MouseGestureTest, InitiallyIdle) {
    EXPECT_EQ(gesture_.GetPhase(), GesturePhase::Idle);
    EXPECT_EQ(gesture_.GetDirection(), GestureDirection::None);
    EXPECT_FALSE(gesture_.IsGestureActive());
    EXPECT_FALSE(gesture_.IsOverlayVisible());
    EXPECT_TRUE(gesture_.GetTrailPoints().empty());
}

// ─── Small movement → context menu ───

TEST_F(MouseGestureTest, SmallMoveThenReleaseShowsContextMenu) {
    gesture_.OnRButtonDown(100.0f, 100.0f);
    EXPECT_EQ(gesture_.GetPhase(), GesturePhase::Pressed);

    // Move a tiny amount (< threshold)
    gesture_.OnMouseMove(105.0f, 102.0f);
    EXPECT_EQ(gesture_.GetPhase(), GesturePhase::Pressed);
    EXPECT_FALSE(gesture_.IsGestureActive());

    auto result = gesture_.OnRButtonUp();
    EXPECT_EQ(result, GestureResult::ShowContextMenu);
    EXPECT_EQ(gesture_.GetPhase(), GesturePhase::Idle);
}

// ─── No movement → context menu ───

TEST_F(MouseGestureTest, NoMoveThenReleaseShowsContextMenu) {
    gesture_.OnRButtonDown(100.0f, 100.0f);
    auto result = gesture_.OnRButtonUp();
    EXPECT_EQ(result, GestureResult::ShowContextMenu);
}

// ─── Right gesture → Forward ───

TEST_F(MouseGestureTest, RightGestureReturnsForward) {
    gesture_.OnRButtonDown(100.0f, 100.0f);
    gesture_.OnMouseMove(200.0f, 100.0f);  // 100px right
    EXPECT_TRUE(gesture_.IsGestureActive());
    EXPECT_EQ(gesture_.GetDirection(), GestureDirection::Right);

    auto result = gesture_.OnRButtonUp();
    EXPECT_EQ(result, GestureResult::Forward);
    // Action fires → reset to Idle, overlay gone
    EXPECT_EQ(gesture_.GetPhase(), GesturePhase::Idle);
    EXPECT_FALSE(gesture_.IsOverlayVisible());
}

// ─── Left gesture → Back ───

TEST_F(MouseGestureTest, LeftGestureReturnsBack) {
    gesture_.OnRButtonDown(200.0f, 100.0f);
    gesture_.OnMouseMove(50.0f, 100.0f);  // 150px left
    EXPECT_TRUE(gesture_.IsGestureActive());
    EXPECT_EQ(gesture_.GetDirection(), GestureDirection::Left);

    auto result = gesture_.OnRButtonUp();
    EXPECT_EQ(result, GestureResult::Back);
    EXPECT_EQ(gesture_.GetPhase(), GesturePhase::Idle);
    EXPECT_FALSE(gesture_.IsOverlayVisible());
}

// ─── Vertical movement → None ───

TEST_F(MouseGestureTest, VerticalGestureReturnsNone) {
    gesture_.OnRButtonDown(100.0f, 100.0f);
    gesture_.OnMouseMove(100.0f, 200.0f);  // 100px down
    // Enters tracking, but direction is None (|dx| <= |dy|)
    EXPECT_TRUE(gesture_.IsGestureActive());
    EXPECT_EQ(gesture_.GetDirection(), GestureDirection::None);
    EXPECT_FALSE(gesture_.IsOverlayVisible());

    auto result = gesture_.OnRButtonUp();
    EXPECT_EQ(result, GestureResult::None);
    EXPECT_EQ(gesture_.GetPhase(), GesturePhase::Idle);
}

// ─── Overlay shown during Tracking when direction is determined ───

TEST_F(MouseGestureTest, OverlayVisibleDuringTrackingWithDirection) {
    gesture_.OnRButtonDown(100.0f, 100.0f);
    gesture_.OnMouseMove(200.0f, 100.0f);  // Right
    EXPECT_TRUE(gesture_.IsGestureActive());
    EXPECT_TRUE(gesture_.IsOverlayVisible());
    EXPECT_FLOAT_EQ(gesture_.GetOverlayAlpha(), 1.0f);
}

TEST_F(MouseGestureTest, OverlayHiddenDuringTrackingWithNoDirection) {
    gesture_.OnRButtonDown(100.0f, 100.0f);
    gesture_.OnMouseMove(100.0f, 200.0f);  // Vertical → no direction
    EXPECT_TRUE(gesture_.IsGestureActive());
    EXPECT_FALSE(gesture_.IsOverlayVisible());
}

TEST_F(MouseGestureTest, OverlayTogglesAsDirectionChanges) {
    gesture_.OnRButtonDown(100.0f, 100.0f);
    // Move right → overlay on
    gesture_.OnMouseMove(200.0f, 100.0f);
    EXPECT_TRUE(gesture_.IsOverlayVisible());

    // Move to vertical → overlay off
    gesture_.OnMouseMove(100.0f, 200.0f);
    EXPECT_FALSE(gesture_.IsOverlayVisible());

    // Move left → overlay on again
    gesture_.OnMouseMove(0.0f, 100.0f);
    EXPECT_TRUE(gesture_.IsOverlayVisible());
    EXPECT_EQ(gesture_.GetDirection(), GestureDirection::Left);
}

TEST_F(MouseGestureTest, OverlayGoneAfterRButtonUp) {
    gesture_.OnRButtonDown(100.0f, 100.0f);
    gesture_.OnMouseMove(200.0f, 100.0f);
    EXPECT_TRUE(gesture_.IsOverlayVisible());

    gesture_.OnRButtonUp();
    EXPECT_FALSE(gesture_.IsOverlayVisible());
    EXPECT_EQ(gesture_.GetPhase(), GesturePhase::Idle);
}

// ─── Diagonal movement threshold ───

TEST_F(MouseGestureTest, DiagonalWithMoreHorizontalIsDirectional) {
    gesture_.OnRButtonDown(100.0f, 100.0f);
    // Move 50px right, 20px down → |dx| > |dy|, distance > threshold
    gesture_.OnMouseMove(150.0f, 120.0f);
    EXPECT_TRUE(gesture_.IsGestureActive());
    EXPECT_EQ(gesture_.GetDirection(), GestureDirection::Right);
    EXPECT_TRUE(gesture_.IsOverlayVisible());
}

TEST_F(MouseGestureTest, DiagonalWithMoreVerticalIsNone) {
    gesture_.OnRButtonDown(100.0f, 100.0f);
    // Move 20px right, 50px down → |dx| <= |dy|
    gesture_.OnMouseMove(120.0f, 150.0f);
    EXPECT_TRUE(gesture_.IsGestureActive());
    EXPECT_EQ(gesture_.GetDirection(), GestureDirection::None);
    EXPECT_FALSE(gesture_.IsOverlayVisible());
}

// ─── Trail point accumulation ───

TEST_F(MouseGestureTest, TrailPointsAccumulate) {
    gesture_.OnRButtonDown(0.0f, 0.0f);
    EXPECT_EQ(gesture_.GetTrailPoints().size(), 1u);  // start point

    // Move past threshold to enter Tracking
    gesture_.OnMouseMove(40.0f, 0.0f);
    EXPECT_TRUE(gesture_.IsGestureActive());

    // Continue moving
    gesture_.OnMouseMove(50.0f, 0.0f);
    gesture_.OnMouseMove(60.0f, 0.0f);
    gesture_.OnMouseMove(70.0f, 0.0f);

    EXPECT_GE(gesture_.GetTrailPoints().size(), 3u);
}

TEST_F(MouseGestureTest, TrailSubsamplingSkipsTinyMoves) {
    gesture_.OnRButtonDown(0.0f, 0.0f);
    gesture_.OnMouseMove(40.0f, 0.0f);  // Enter tracking

    size_t count_after_threshold = gesture_.GetTrailPoints().size();

    // Move less than MIN_POINT_DISTANCE
    gesture_.OnMouseMove(40.5f, 0.0f);
    gesture_.OnMouseMove(41.0f, 0.0f);

    EXPECT_EQ(gesture_.GetTrailPoints().size(), count_after_threshold);
}

// ─── Reset clears all state ───

TEST_F(MouseGestureTest, ResetClearsEverything) {
    gesture_.OnRButtonDown(100.0f, 100.0f);
    gesture_.OnMouseMove(200.0f, 100.0f);
    EXPECT_TRUE(gesture_.IsOverlayVisible());

    gesture_.Reset();

    EXPECT_EQ(gesture_.GetPhase(), GesturePhase::Idle);
    EXPECT_EQ(gesture_.GetDirection(), GestureDirection::None);
    EXPECT_FALSE(gesture_.IsGestureActive());
    EXPECT_FALSE(gesture_.IsOverlayVisible());
    EXPECT_TRUE(gesture_.GetTrailPoints().empty());
}

// ─── RButtonUp in Idle returns None ───

TEST_F(MouseGestureTest, RButtonUpInIdleReturnsNone) {
    auto result = gesture_.OnRButtonUp();
    EXPECT_EQ(result, GestureResult::None);
}

// ─── MouseMove in Idle is ignored ───

TEST_F(MouseGestureTest, MouseMoveInIdleIsIgnored) {
    gesture_.OnMouseMove(100.0f, 100.0f);
    EXPECT_EQ(gesture_.GetPhase(), GesturePhase::Idle);
    EXPECT_TRUE(gesture_.GetTrailPoints().empty());
}
