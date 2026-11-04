#include <gtest/gtest.h>
#include "app_state.h"
#include "mouse_gesture.h"
#include "swipe_detector.h"
#include <vector>

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

// ---- SwipeDetector ----

class SwipeDetectorTest : public ::testing::Test {
protected:
    SwipeDetector detector_;
    uint64_t now_ = 1000;  // 任意の開始時刻
};

// ─── 初期状態 ───

TEST_F(SwipeDetectorTest, InitiallyNoSwipe)
{
    EXPECT_FALSE(detector_.IsOverlayVisible());
    EXPECT_EQ(detector_.GetOverlayDirection(), NavDirection::None);
    EXPECT_EQ(detector_.Commit(), NavDirection::None);
}

// ─── 閾値未満は None ───

TEST_F(SwipeDetectorTest, BelowThresholdReturnsNone)
{
    // ホイール 1 ノッチと、閾値のすぐ下。
    for (const int delta : { 120, SwipeDetector::TRIGGER_THRESHOLD - 1 }) {
        SCOPED_TRACE(delta);
        SwipeDetector det;
        det.OnHWheel(delta, now_);
        EXPECT_FALSE(det.IsOverlayVisible());
        EXPECT_EQ(det.GetOverlayDirection(), NavDirection::None);
        EXPECT_EQ(det.Commit(), NavDirection::None);
    }
}

// ─── 右スワイプ（正の delta）で Back、左スワイプ（負の delta）で Forward ───

// 蓄積が閾値に届いたらオーバーレイを出す。OnHWheel 自体は発火せず、Commit で発動して状態を消す。
TEST_F(SwipeDetectorTest, SwipeShowsOverlayAndCommitsOnce)
{
    for (const int sign : { +1, -1 }) {
        const NavDirection expected = sign > 0 ? NavDirection::Back : NavDirection::Forward;
        // 2 回に分けて届く、ちょうど閾値、閾値の倍
        for (const auto& deltas : { std::vector{ 200, 200 }, std::vector{ SwipeDetector::TRIGGER_THRESHOLD }, std::vector{ SwipeDetector::TRIGGER_THRESHOLD * 2 } }) {
            SCOPED_TRACE(::testing::Message() << "sign=" << sign << " first=" << deltas.front() << " count=" << deltas.size());
            SwipeDetector det;
            for (size_t i = 0; i < deltas.size(); ++i) {
                det.OnHWheel(sign * deltas[i], now_ + i * 10);
            }
            EXPECT_TRUE(det.IsOverlayVisible());
            EXPECT_EQ(det.GetOverlayDirection(), expected);
            EXPECT_EQ(det.Commit(), expected);

            EXPECT_FALSE(det.IsOverlayVisible());
            EXPECT_EQ(det.GetOverlayDirection(), NavDirection::None);
            EXPECT_EQ(det.Commit(), NavDirection::None);
        }
    }
}

// ─── 複数回の小さいイベントで蓄積 ───

TEST_F(SwipeDetectorTest, SmallDeltasAccumulate)
{
    // TRIGGER_THRESHOLD / 50 = 8 ずつ、50回
    int small = SwipeDetector::TRIGGER_THRESHOLD / 50;
    for (int i = 0; i < 50; ++i) {
        detector_.OnHWheel(small, now_ + i * 10);
    }
    EXPECT_EQ(detector_.Commit(), NavDirection::Back);
}

// ─── 軸ロック: 縦スクロール直後は水平入力を無視 ───

TEST_F(SwipeDetectorTest, AxisLockIgnoresHWheelAfterVScroll)
{
    detector_.NotifyVScroll(now_);

    // 軸ロック期間内
    detector_.OnHWheel(SwipeDetector::TRIGGER_THRESHOLD, now_ + 100);
    EXPECT_EQ(detector_.Commit(), NavDirection::None);
}

TEST_F(SwipeDetectorTest, AxisLockExpiresAfterTimeout)
{
    detector_.NotifyVScroll(now_);

    // 軸ロック期間を過ぎた後は通常通り動作
    uint64_t after_lock = now_ + SwipeDetector::AXIS_LOCK_MS;
    detector_.OnHWheel(SwipeDetector::TRIGGER_THRESHOLD, after_lock);
    EXPECT_EQ(detector_.Commit(), NavDirection::Back);
}

// ─── タイムアウト: 長時間の無活動で蓄積リセット ───

TEST_F(SwipeDetectorTest, ResetAfterInactivity)
{
    detector_.OnHWheel(300, now_);

    // タイムアウト後に新しいイベント → 蓄積リセットされてから加算
    uint64_t after_timeout = now_ + SwipeDetector::RESET_TIMEOUT_MS + 1;
    detector_.OnHWheel(50, after_timeout);
    // 50 のみで TRIGGER_THRESHOLD に満たないので None
    EXPECT_EQ(detector_.Commit(), NavDirection::None);
}

TEST_F(SwipeDetectorTest, NoResetWithinTimeout)
{
    detector_.OnHWheel(300, now_);

    // タイムアウト内なら蓄積は継続
    detector_.OnHWheel(100, now_ + SwipeDetector::RESET_TIMEOUT_MS);
    EXPECT_EQ(detector_.Commit(), NavDirection::Back);  // 300 + 100 = 400 >= threshold
}

// ─── 方向の打ち消し ───

TEST_F(SwipeDetectorTest, OppositeDirectionsCancelOut)
{
    detector_.OnHWheel(300, now_);
    detector_.OnHWheel(-300, now_ + 10);
    EXPECT_EQ(detector_.Commit(), NavDirection::None);
}

// ─── 連続ナビゲーション: Commit後すぐに次のスワイプが可能 ───

TEST_F(SwipeDetectorTest, ConsecutiveSwipes)
{
    detector_.OnHWheel(SwipeDetector::TRIGGER_THRESHOLD, now_);
    EXPECT_EQ(detector_.Commit(), NavDirection::Back);

    detector_.OnHWheel(-SwipeDetector::TRIGGER_THRESHOLD, now_ + 100);
    EXPECT_EQ(detector_.Commit(), NavDirection::Forward);
}

// ─── NotifyVScrollが蓄積をリセットする ───

TEST_F(SwipeDetectorTest, VScrollResetsDelta)
{
    detector_.OnHWheel(300, now_);
    detector_.NotifyVScroll(now_ + 50);
    // VScroll でリセットされたので軸ロック解除後の小さい入力では発火しない
    detector_.OnHWheel(50, now_ + 50 + SwipeDetector::AXIS_LOCK_MS);
    EXPECT_EQ(detector_.Commit(), NavDirection::None);
}

// ─── 縦スクロールでオーバーレイが消える ───

TEST_F(SwipeDetectorTest, OverlayDisappearsAfterVScroll)
{
    detector_.OnHWheel(SwipeDetector::TRIGGER_THRESHOLD, now_);
    EXPECT_TRUE(detector_.IsOverlayVisible());

    detector_.NotifyVScroll(now_ + 50);
    EXPECT_FALSE(detector_.IsOverlayVisible());
}
