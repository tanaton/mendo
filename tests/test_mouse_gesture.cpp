#include <gtest/gtest.h>
#include "app_state.h"
#include "mouse_gesture.h"
#include "swipe_detector.h"

namespace {

void ExpectIdle(const MouseGesture& g)
{
    EXPECT_EQ(g.GetPhase(), GesturePhase::Idle);
    EXPECT_EQ(g.GetDirection(), NavDirection::None);
    EXPECT_FALSE(g.IsGestureActive());
    EXPECT_FALSE(g.IsOverlayVisible());
    EXPECT_TRUE(g.GetTrailPoints().empty());
}

} // namespace

class MouseGestureTest : public ::testing::Test {
protected:
    MouseGesture gesture_;
};

// ─── Idle では移動も離すのも無視する ───

TEST_F(MouseGestureTest, IdleIgnoresMoveAndRelease)
{
    ExpectIdle(gesture_);
    EXPECT_FALSE(gesture_.OnMouseMove(100.0f, 100.0f));
    ExpectIdle(gesture_);
    EXPECT_EQ(gesture_.OnRButtonUp(), GestureResult::None);
}

// ─── 押して動かして離す ───

// 閾値未満の移動は右クリック扱いでメニューを出す。閾値を超えると追跡に入り、
// 横向きが勝てば戻る/進む、縦向きが勝てば方向なしでメニューを出す (イベントを握り潰してメニューを失わない)。
// 離したら Idle に戻り、オーバーレイも消える。
TEST_F(MouseGestureTest, ReleaseResultFollowsMove)
{
    struct Case {
        const char* name;
        float down_x;
        float move_x;
        float move_y;
        GesturePhase phase;
        NavDirection direction;
        GestureResult result;
    };
    constexpr Case kCases[] = {
        { "NoMove", 100.0f, 100.0f, 100.0f, GesturePhase::Pressed, NavDirection::None, GestureResult::ShowContextMenu },
        { "SmallMove", 100.0f, 105.0f, 102.0f, GesturePhase::Pressed, NavDirection::None, GestureResult::ShowContextMenu },
        { "Right", 100.0f, 200.0f, 100.0f, GesturePhase::Tracking, NavDirection::Forward, GestureResult::Forward },
        { "Left", 200.0f, 50.0f, 100.0f, GesturePhase::Tracking, NavDirection::Back, GestureResult::Back },
        { "Vertical", 100.0f, 100.0f, 200.0f, GesturePhase::Tracking, NavDirection::None, GestureResult::ShowContextMenu },
        // 右に 50、下に 20
        { "DiagonalMoreHorizontal", 100.0f, 150.0f, 120.0f, GesturePhase::Tracking, NavDirection::Forward, GestureResult::Forward },
        // 右に 20、下に 50
        { "DiagonalMoreVertical", 100.0f, 120.0f, 150.0f, GesturePhase::Tracking, NavDirection::None, GestureResult::ShowContextMenu },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        gesture_.OnRButtonDown(c.down_x, 100.0f);
        EXPECT_EQ(gesture_.GetPhase(), GesturePhase::Pressed);
        gesture_.OnMouseMove(c.move_x, c.move_y);
        EXPECT_EQ(gesture_.GetPhase(), c.phase);
        EXPECT_EQ(gesture_.IsGestureActive(), c.phase == GesturePhase::Tracking);
        EXPECT_EQ(gesture_.GetDirection(), c.direction);
        EXPECT_EQ(gesture_.IsOverlayVisible(), c.direction != NavDirection::None);

        EXPECT_EQ(gesture_.OnRButtonUp(), c.result);
        ExpectIdle(gesture_);
    }
}

// ─── トラッキング中の方向変化に合わせてオーバーレイを切り替える ───

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
    EXPECT_EQ(gesture_.GetDirection(), NavDirection::Back);
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
    ExpectIdle(gesture_);
}

// 描画に表れる変化 (点の追加・方向の変化) があったときだけ true を返す。
TEST_F(MouseGestureTest, MouseMoveReportsOnlyVisibleChanges)
{
    gesture_.OnRButtonDown(100.0f, 100.0f);
    EXPECT_FALSE(gesture_.OnMouseMove(105.0f, 100.0f)) << "閾値未満では軌跡を描かない";
    EXPECT_TRUE(gesture_.OnMouseMove(150.0f, 100.0f)) << "追跡開始で点が増える";
    EXPECT_FALSE(gesture_.OnMouseMove(151.0f, 100.0f)) << "点の間隔未満で方向も同じ";
    EXPECT_TRUE(gesture_.OnMouseMove(160.0f, 100.0f));
}

// ─── ResolveGestureOverlay: マウスジェスチャーとスワイプの表示の合成 ───

TEST(GestureOverlay, Idle)
{
    MouseGesture gesture;
    SwipeDetector swipe;
    auto gs = ResolveGestureOverlay(gesture, swipe);
    EXPECT_EQ(gs.trail, nullptr);
    EXPECT_EQ(gs.direction, NavDirection::None);
}

TEST(GestureOverlay, TrackingLeft)
{
    MouseGesture gesture;
    SwipeDetector swipe;
    gesture.OnRButtonDown(100.0f, 100.0f);
    gesture.OnMouseMove(20.0f, 100.0f); // 左に80px移動

    auto gs = ResolveGestureOverlay(gesture, swipe);
    ASSERT_NE(gs.trail, nullptr);
    EXPECT_FALSE(gs.trail->empty());
    EXPECT_EQ(gs.direction, NavDirection::Back);
}

TEST(GestureOverlay, SwipeFallback_WhenGestureIdle)
{
    MouseGesture gesture;
    SwipeDetector swipe;
    swipe.OnHWheel(500, 1000);

    auto gs = ResolveGestureOverlay(gesture, swipe);
    EXPECT_EQ(gs.trail, nullptr);
    EXPECT_EQ(gs.direction, NavDirection::Back);
}

// 右へのジェスチャーは、逆向きのスワイプがあってもジェスチャーの向きを出す。
TEST(GestureOverlay, GestureTakesPrecedenceOverSwipe)
{
    MouseGesture gesture;
    SwipeDetector swipe;
    gesture.OnRButtonDown(100.0f, 100.0f);
    gesture.OnMouseMove(200.0f, 100.0f);
    swipe.OnHWheel(-500, 1000);

    auto gs = ResolveGestureOverlay(gesture, swipe);
    EXPECT_EQ(gs.direction, NavDirection::Forward);
}
