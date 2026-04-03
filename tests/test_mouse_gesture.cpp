#include <gtest/gtest.h>
#include "mouse_gesture.h"

class MouseGestureTest : public ::testing::Test {
protected:
    MouseGesture gesture_;
};

// ─── 初期状態 ───

TEST_F(MouseGestureTest, InitiallyIdle)
{
    EXPECT_EQ(gesture_.GetPhase(), GesturePhase::Idle);
    EXPECT_EQ(gesture_.GetDirection(), GestureDirection::None);
    EXPECT_FALSE(gesture_.IsGestureActive());
    EXPECT_FALSE(gesture_.IsOverlayVisible());
    EXPECT_TRUE(gesture_.GetTrailPoints().empty());
}

// ─── 小さな移動 → コンテキストメニュー ───

TEST_F(MouseGestureTest, SmallMoveThenReleaseShowsContextMenu)
{
    gesture_.OnRButtonDown(100.0f, 100.0f);
    EXPECT_EQ(gesture_.GetPhase(), GesturePhase::Pressed);

    // ごくわずかに移動（< 閾値）
    gesture_.OnMouseMove(105.0f, 102.0f);
    EXPECT_EQ(gesture_.GetPhase(), GesturePhase::Pressed);
    EXPECT_FALSE(gesture_.IsGestureActive());

    auto result = gesture_.OnRButtonUp();
    EXPECT_EQ(result, GestureResult::ShowContextMenu);
    EXPECT_EQ(gesture_.GetPhase(), GesturePhase::Idle);
}

// ─── 移動なし → コンテキストメニュー ───

TEST_F(MouseGestureTest, NoMoveThenReleaseShowsContextMenu)
{
    gesture_.OnRButtonDown(100.0f, 100.0f);
    auto result = gesture_.OnRButtonUp();
    EXPECT_EQ(result, GestureResult::ShowContextMenu);
}

// ─── 右ジェスチャー → 進む ───

TEST_F(MouseGestureTest, RightGestureReturnsForward)
{
    gesture_.OnRButtonDown(100.0f, 100.0f);
    gesture_.OnMouseMove(200.0f, 100.0f);  // 右に100px
    EXPECT_TRUE(gesture_.IsGestureActive());
    EXPECT_EQ(gesture_.GetDirection(), GestureDirection::Right);

    auto result = gesture_.OnRButtonUp();
    EXPECT_EQ(result, GestureResult::Forward);
    // アクション発火 → Idleにリセット、オーバーレイ消失
    EXPECT_EQ(gesture_.GetPhase(), GesturePhase::Idle);
    EXPECT_FALSE(gesture_.IsOverlayVisible());
}

// ─── 左ジェスチャー → 戻る ───

TEST_F(MouseGestureTest, LeftGestureReturnsBack)
{
    gesture_.OnRButtonDown(200.0f, 100.0f);
    gesture_.OnMouseMove(50.0f, 100.0f);  // 左に150px
    EXPECT_TRUE(gesture_.IsGestureActive());
    EXPECT_EQ(gesture_.GetDirection(), GestureDirection::Left);

    auto result = gesture_.OnRButtonUp();
    EXPECT_EQ(result, GestureResult::Back);
    EXPECT_EQ(gesture_.GetPhase(), GesturePhase::Idle);
    EXPECT_FALSE(gesture_.IsOverlayVisible());
}

// ─── 垂直移動 → None ───

TEST_F(MouseGestureTest, VerticalGestureReturnsNone)
{
    gesture_.OnRButtonDown(100.0f, 100.0f);
    gesture_.OnMouseMove(100.0f, 200.0f);  // 下に100px
    // トラッキングに入るが、方向はNone（|dx| <= |dy|）
    EXPECT_TRUE(gesture_.IsGestureActive());
    EXPECT_EQ(gesture_.GetDirection(), GestureDirection::None);
    EXPECT_FALSE(gesture_.IsOverlayVisible());

    auto result = gesture_.OnRButtonUp();
    EXPECT_EQ(result, GestureResult::None);
    EXPECT_EQ(gesture_.GetPhase(), GesturePhase::Idle);
}

// ─── トラッキング中に方向が決定されたらオーバーレイ表示 ───

TEST_F(MouseGestureTest, OverlayVisibleDuringTrackingWithDirection)
{
    gesture_.OnRButtonDown(100.0f, 100.0f);
    gesture_.OnMouseMove(200.0f, 100.0f);  // 右
    EXPECT_TRUE(gesture_.IsGestureActive());
    EXPECT_TRUE(gesture_.IsOverlayVisible());
    EXPECT_FLOAT_EQ(gesture_.GetOverlayAlpha(), 1.0f);
}

TEST_F(MouseGestureTest, OverlayHiddenDuringTrackingWithNoDirection)
{
    gesture_.OnRButtonDown(100.0f, 100.0f);
    gesture_.OnMouseMove(100.0f, 200.0f);  // 垂直 → 方向なし
    EXPECT_TRUE(gesture_.IsGestureActive());
    EXPECT_FALSE(gesture_.IsOverlayVisible());
}

TEST_F(MouseGestureTest, OverlayTogglesAsDirectionChanges)
{
    gesture_.OnRButtonDown(100.0f, 100.0f);
    // 右に移動 → オーバーレイ表示
    gesture_.OnMouseMove(200.0f, 100.0f);
    EXPECT_TRUE(gesture_.IsOverlayVisible());

    // 垂直に移動 → オーバーレイ非表示
    gesture_.OnMouseMove(100.0f, 200.0f);
    EXPECT_FALSE(gesture_.IsOverlayVisible());

    // 左に移動 → オーバーレイ再表示
    gesture_.OnMouseMove(0.0f, 100.0f);
    EXPECT_TRUE(gesture_.IsOverlayVisible());
    EXPECT_EQ(gesture_.GetDirection(), GestureDirection::Left);
}

TEST_F(MouseGestureTest, OverlayGoneAfterRButtonUp)
{
    gesture_.OnRButtonDown(100.0f, 100.0f);
    gesture_.OnMouseMove(200.0f, 100.0f);
    EXPECT_TRUE(gesture_.IsOverlayVisible());

    gesture_.OnRButtonUp();
    EXPECT_FALSE(gesture_.IsOverlayVisible());
    EXPECT_EQ(gesture_.GetPhase(), GesturePhase::Idle);
}

// ─── 斜め移動の閾値 ───

TEST_F(MouseGestureTest, DiagonalWithMoreHorizontalIsDirectional)
{
    gesture_.OnRButtonDown(100.0f, 100.0f);
    // 右に50px、下に20px → |dx| > |dy|、距離 > 閾値
    gesture_.OnMouseMove(150.0f, 120.0f);
    EXPECT_TRUE(gesture_.IsGestureActive());
    EXPECT_EQ(gesture_.GetDirection(), GestureDirection::Right);
    EXPECT_TRUE(gesture_.IsOverlayVisible());
}

TEST_F(MouseGestureTest, DiagonalWithMoreVerticalIsNone)
{
    gesture_.OnRButtonDown(100.0f, 100.0f);
    // 右に20px、下に50px → |dx| <= |dy|
    gesture_.OnMouseMove(120.0f, 150.0f);
    EXPECT_TRUE(gesture_.IsGestureActive());
    EXPECT_EQ(gesture_.GetDirection(), GestureDirection::None);
    EXPECT_FALSE(gesture_.IsOverlayVisible());
}

// ─── 軌跡ポイントの蓄積 ───

TEST_F(MouseGestureTest, TrailPointsAccumulate)
{
    gesture_.OnRButtonDown(0.0f, 0.0f);
    EXPECT_EQ(gesture_.GetTrailPoints().size(), 1u);  // 開始点

    // 閾値を超えて移動しトラッキングに入る
    gesture_.OnMouseMove(40.0f, 0.0f);
    EXPECT_TRUE(gesture_.IsGestureActive());

    // 移動を続ける
    gesture_.OnMouseMove(50.0f, 0.0f);
    gesture_.OnMouseMove(60.0f, 0.0f);
    gesture_.OnMouseMove(70.0f, 0.0f);

    EXPECT_GE(gesture_.GetTrailPoints().size(), 3u);
}

TEST_F(MouseGestureTest, TrailSubsamplingSkipsTinyMoves)
{
    gesture_.OnRButtonDown(0.0f, 0.0f);
    gesture_.OnMouseMove(40.0f, 0.0f);  // トラッキング開始

    size_t count_after_threshold = gesture_.GetTrailPoints().size();

    // MIN_POINT_DISTANCE未満の移動
    gesture_.OnMouseMove(40.5f, 0.0f);
    gesture_.OnMouseMove(41.0f, 0.0f);

    EXPECT_EQ(gesture_.GetTrailPoints().size(), count_after_threshold);
}

// ─── リセットですべての状態をクリア ───

TEST_F(MouseGestureTest, ResetClearsEverything)
{
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

// ─── Idle状態でRButtonUpするとNoneを返す ───

TEST_F(MouseGestureTest, RButtonUpInIdleReturnsNone)
{
    auto result = gesture_.OnRButtonUp();
    EXPECT_EQ(result, GestureResult::None);
}

// ─── Idle状態でのMouseMoveは無視される ───

TEST_F(MouseGestureTest, MouseMoveInIdleIsIgnored)
{
    gesture_.OnMouseMove(100.0f, 100.0f);
    EXPECT_EQ(gesture_.GetPhase(), GesturePhase::Idle);
    EXPECT_TRUE(gesture_.GetTrailPoints().empty());
}
