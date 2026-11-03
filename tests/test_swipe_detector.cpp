#include <gtest/gtest.h>
#include "swipe_detector.h"
#include <vector>

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
