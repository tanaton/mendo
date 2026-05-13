#include <gtest/gtest.h>
#include "swipe_detector.h"

class SwipeDetectorTest : public ::testing::Test {
protected:
    SwipeDetector detector_;
    uint64_t now_ = 1000;  // 任意の開始時刻
};

// ─── 初期状態 ───

TEST_F(SwipeDetectorTest, InitiallyNoSwipe)
{
    EXPECT_EQ(detector_.Commit(), SwipeResult::None);
    EXPECT_FALSE(detector_.IsOverlayVisible());
}

// ─── 閾値未満は None ───

TEST_F(SwipeDetectorTest, BelowThresholdReturnsNone)
{
    detector_.OnHWheel(120, now_);
    EXPECT_FALSE(detector_.IsOverlayVisible());
    EXPECT_EQ(detector_.Commit(), SwipeResult::None);
}

// ─── 右スワイプ（正のdelta蓄積）→ Commit で Back ───

TEST_F(SwipeDetectorTest, RightSwipeTriggersBack)
{
    detector_.OnHWheel(200, now_);
    detector_.OnHWheel(200, now_ + 10);
    EXPECT_EQ(detector_.Commit(), SwipeResult::Back);
    // Commit後にリセットされる
    EXPECT_EQ(detector_.Commit(), SwipeResult::None);
}

// ─── 左スワイプ（負のdelta蓄積）→ Commit で Forward ───

TEST_F(SwipeDetectorTest, LeftSwipeTriggersForward)
{
    detector_.OnHWheel(-200, now_);
    detector_.OnHWheel(-200, now_ + 10);
    EXPECT_EQ(detector_.Commit(), SwipeResult::Forward);
    EXPECT_EQ(detector_.Commit(), SwipeResult::None);
}

// ─── ちょうど閾値で発動 ───

TEST_F(SwipeDetectorTest, ExactThresholdTriggersBack)
{
    detector_.OnHWheel(SwipeDetector::TRIGGER_THRESHOLD, now_);
    EXPECT_EQ(detector_.Commit(), SwipeResult::Back);
}

TEST_F(SwipeDetectorTest, ExactNegativeThresholdTriggersForward)
{
    detector_.OnHWheel(-SwipeDetector::TRIGGER_THRESHOLD, now_);
    EXPECT_EQ(detector_.Commit(), SwipeResult::Forward);
}

// ─── 複数回の小さいイベントで蓄積 ───

TEST_F(SwipeDetectorTest, SmallDeltasAccumulate)
{
    // TRIGGER_THRESHOLD / 50 = 8 ずつ、50回
    int small = SwipeDetector::TRIGGER_THRESHOLD / 50;
    for (int i = 0; i < 50; ++i) {
        detector_.OnHWheel(small, now_ + i * 10);
    }
    EXPECT_EQ(detector_.Commit(), SwipeResult::Back);
}

// ─── OnHWheel は即時発火しない ───

TEST_F(SwipeDetectorTest, OnHWheelDoesNotFireImmediately)
{
    // 閾値を大幅に超えてもOnHWheel自体は発火しない
    detector_.OnHWheel(SwipeDetector::TRIGGER_THRESHOLD * 2, now_);
    // オーバーレイは表示されるが Commit() を呼ばない限りナビゲーションは発火しない
    EXPECT_TRUE(detector_.IsOverlayVisible());
    EXPECT_EQ(detector_.Commit(), SwipeResult::Back);
}

// ─── 軸ロック: 縦スクロール直後は水平入力を無視 ───

TEST_F(SwipeDetectorTest, AxisLockIgnoresHWheelAfterVScroll)
{
    detector_.NotifyVScroll(now_);

    // 軸ロック期間内
    detector_.OnHWheel(SwipeDetector::TRIGGER_THRESHOLD, now_ + 100);
    EXPECT_EQ(detector_.Commit(), SwipeResult::None);
}

TEST_F(SwipeDetectorTest, AxisLockExpiresAfterTimeout)
{
    detector_.NotifyVScroll(now_);

    // 軸ロック期間を過ぎた後は通常通り動作
    uint64_t after_lock = now_ + SwipeDetector::AXIS_LOCK_MS;
    detector_.OnHWheel(SwipeDetector::TRIGGER_THRESHOLD, after_lock);
    EXPECT_EQ(detector_.Commit(), SwipeResult::Back);
}

// ─── タイムアウト: 長時間の無活動で蓄積リセット ───

TEST_F(SwipeDetectorTest, ResetAfterInactivity)
{
    detector_.OnHWheel(300, now_);

    // タイムアウト後に新しいイベント → 蓄積リセットされてから加算
    uint64_t after_timeout = now_ + SwipeDetector::RESET_TIMEOUT_MS + 1;
    detector_.OnHWheel(50, after_timeout);
    // 50 のみで TRIGGER_THRESHOLD に満たないので None
    EXPECT_EQ(detector_.Commit(), SwipeResult::None);
}

TEST_F(SwipeDetectorTest, NoResetWithinTimeout)
{
    detector_.OnHWheel(300, now_);

    // タイムアウト内なら蓄積は継続
    detector_.OnHWheel(100, now_ + SwipeDetector::RESET_TIMEOUT_MS);
    EXPECT_EQ(detector_.Commit(), SwipeResult::Back);  // 300 + 100 = 400 >= threshold
}

// ─── 方向の打ち消し ───

TEST_F(SwipeDetectorTest, OppositeDirectionsCancelOut)
{
    detector_.OnHWheel(300, now_);
    detector_.OnHWheel(-300, now_ + 10);
    EXPECT_EQ(detector_.Commit(), SwipeResult::None);
}

// ─── Reset() で全状態クリア ───

TEST_F(SwipeDetectorTest, ResetClearsAllState)
{
    detector_.OnHWheel(200, now_);
    detector_.NotifyVScroll(now_ + 10);
    detector_.Reset();

    // リセット後は軸ロックも解除されている
    detector_.OnHWheel(SwipeDetector::TRIGGER_THRESHOLD, now_ + 20);
    EXPECT_EQ(detector_.Commit(), SwipeResult::Back);
}

// ─── Commit はリセット後に再度 None を返す ───

TEST_F(SwipeDetectorTest, CommitResetsState)
{
    detector_.OnHWheel(SwipeDetector::TRIGGER_THRESHOLD, now_);
    EXPECT_EQ(detector_.Commit(), SwipeResult::Back);
    // 2回目のCommitはNone
    EXPECT_EQ(detector_.Commit(), SwipeResult::None);
}

// ─── 連続ナビゲーション: Commit後すぐに次のスワイプが可能 ───

TEST_F(SwipeDetectorTest, ConsecutiveSwipes)
{
    detector_.OnHWheel(SwipeDetector::TRIGGER_THRESHOLD, now_);
    EXPECT_EQ(detector_.Commit(), SwipeResult::Back);

    detector_.OnHWheel(-SwipeDetector::TRIGGER_THRESHOLD, now_ + 100);
    EXPECT_EQ(detector_.Commit(), SwipeResult::Forward);
}

// ─── NotifyVScrollが蓄積をリセットする ───

TEST_F(SwipeDetectorTest, VScrollResetsDelta)
{
    detector_.OnHWheel(300, now_);
    detector_.NotifyVScroll(now_ + 50);
    // VScroll でリセットされたので軸ロック解除後の小さい入力では発火しない
    detector_.OnHWheel(50, now_ + 50 + SwipeDetector::AXIS_LOCK_MS);
    EXPECT_EQ(detector_.Commit(), SwipeResult::None);
}

// ─── オーバーレイ表示（閾値到達で表示、Commitで消滅） ───

TEST_F(SwipeDetectorTest, OverlayNotVisibleInitially)
{
    EXPECT_FALSE(detector_.IsOverlayVisible());
    EXPECT_EQ(detector_.GetOverlayDirection(), 0);
    EXPECT_FLOAT_EQ(detector_.GetOverlayAlpha(), 0.0f);
}

TEST_F(SwipeDetectorTest, OverlayNotVisibleBelowThreshold)
{
    detector_.OnHWheel(SwipeDetector::TRIGGER_THRESHOLD - 1, now_);
    EXPECT_FALSE(detector_.IsOverlayVisible());
    EXPECT_EQ(detector_.GetOverlayDirection(), 0);
    EXPECT_FLOAT_EQ(detector_.GetOverlayAlpha(), 0.0f);
}

TEST_F(SwipeDetectorTest, OverlayVisibleAtThreshold)
{
    detector_.OnHWheel(SwipeDetector::TRIGGER_THRESHOLD, now_);
    EXPECT_TRUE(detector_.IsOverlayVisible());
    EXPECT_FLOAT_EQ(detector_.GetOverlayAlpha(), 1.0f);
}

TEST_F(SwipeDetectorTest, OverlayDirectionBackOnPositiveDelta)
{
    detector_.OnHWheel(SwipeDetector::TRIGGER_THRESHOLD, now_);
    EXPECT_EQ(detector_.GetOverlayDirection(), -1);  // 戻る
}

TEST_F(SwipeDetectorTest, OverlayDirectionForwardOnNegativeDelta)
{
    detector_.OnHWheel(-SwipeDetector::TRIGGER_THRESHOLD, now_);
    EXPECT_EQ(detector_.GetOverlayDirection(), 1);  // 進む
}

TEST_F(SwipeDetectorTest, OverlayDisappearsAfterCommit)
{
    detector_.OnHWheel(SwipeDetector::TRIGGER_THRESHOLD, now_);
    EXPECT_TRUE(detector_.IsOverlayVisible());
    // Commit後はデルタがリセットされオーバーレイが消える
    detector_.Commit();
    EXPECT_FALSE(detector_.IsOverlayVisible());
    EXPECT_EQ(detector_.GetOverlayDirection(), 0);
    EXPECT_FLOAT_EQ(detector_.GetOverlayAlpha(), 0.0f);
}

TEST_F(SwipeDetectorTest, OverlayDisappearsAfterVScroll)
{
    detector_.OnHWheel(SwipeDetector::TRIGGER_THRESHOLD, now_);
    EXPECT_TRUE(detector_.IsOverlayVisible());

    detector_.NotifyVScroll(now_ + 50);
    EXPECT_FALSE(detector_.IsOverlayVisible());
}

TEST_F(SwipeDetectorTest, OverlayDisappearsAfterReset)
{
    detector_.OnHWheel(SwipeDetector::TRIGGER_THRESHOLD, now_);
    EXPECT_TRUE(detector_.IsOverlayVisible());

    detector_.Reset();
    EXPECT_FALSE(detector_.IsOverlayVisible());
}
