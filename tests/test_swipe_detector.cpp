#include <gtest/gtest.h>
#include "swipe_detector.h"

class SwipeDetectorTest : public ::testing::Test {
protected:
    SwipeDetector detector_;
    uint64_t now_ = 1000;  // 任意の開始時刻
};

// ─── 初期状態 ───

TEST_F(SwipeDetectorTest, InitiallyZero) {
    EXPECT_EQ(detector_.GetAccumulatedDelta(), 0);
}

// ─── 閾値未満は None ───

TEST_F(SwipeDetectorTest, BelowThresholdReturnsNone) {
    auto result = detector_.OnHWheel(120, now_);
    EXPECT_EQ(result, SwipeResult::None);
    EXPECT_EQ(detector_.GetAccumulatedDelta(), 120);
}

// ─── 右スワイプ（正のdelta蓄積）→ Back ───

TEST_F(SwipeDetectorTest, RightSwipeTriggersBack) {
    detector_.OnHWheel(200, now_);
    auto result = detector_.OnHWheel(200, now_ + 10);
    EXPECT_EQ(result, SwipeResult::Back);
    // 発動後にリセットされる
    EXPECT_EQ(detector_.GetAccumulatedDelta(), 0);
}

// ─── 左スワイプ（負のdelta蓄積）→ Forward ───

TEST_F(SwipeDetectorTest, LeftSwipeTriggersForward) {
    detector_.OnHWheel(-200, now_);
    auto result = detector_.OnHWheel(-200, now_ + 10);
    EXPECT_EQ(result, SwipeResult::Forward);
    EXPECT_EQ(detector_.GetAccumulatedDelta(), 0);
}

// ─── ちょうど閾値で発動 ───

TEST_F(SwipeDetectorTest, ExactThresholdTriggersBack) {
    auto result = detector_.OnHWheel(SwipeDetector::TRIGGER_THRESHOLD, now_);
    EXPECT_EQ(result, SwipeResult::Back);
}

TEST_F(SwipeDetectorTest, ExactNegativeThresholdTriggersForward) {
    auto result = detector_.OnHWheel(-SwipeDetector::TRIGGER_THRESHOLD, now_);
    EXPECT_EQ(result, SwipeResult::Forward);
}

// ─── 複数回の小さいイベントで蓄積 ───

TEST_F(SwipeDetectorTest, SmallDeltasAccumulate) {
    // TRIGGER_THRESHOLD / 50 = 8 ずつ、50回
    int small = SwipeDetector::TRIGGER_THRESHOLD / 50;
    SwipeResult result = SwipeResult::None;
    for (int i = 0; i < 50; ++i) {
        result = detector_.OnHWheel(small, now_ + i * 10);
        if (result != SwipeResult::None) break;
    }
    EXPECT_EQ(result, SwipeResult::Back);
}

// ─── 軸ロック: 縦スクロール直後は水平入力を無視 ───

TEST_F(SwipeDetectorTest, AxisLockIgnoresHWheelAfterVScroll) {
    detector_.NotifyVScroll(now_);

    // 軸ロック期間内
    auto result = detector_.OnHWheel(SwipeDetector::TRIGGER_THRESHOLD, now_ + 100);
    EXPECT_EQ(result, SwipeResult::None);
    EXPECT_EQ(detector_.GetAccumulatedDelta(), 0);  // NotifyVScrollでリセットされている
}

TEST_F(SwipeDetectorTest, AxisLockExpiresAfterTimeout) {
    detector_.NotifyVScroll(now_);

    // 軸ロック期間を過ぎた後は通常通り動作
    uint64_t after_lock = now_ + SwipeDetector::AXIS_LOCK_MS;
    auto result = detector_.OnHWheel(SwipeDetector::TRIGGER_THRESHOLD, after_lock);
    EXPECT_EQ(result, SwipeResult::Back);
}

// ─── タイムアウト: 長時間の無活動で蓄積リセット ───

TEST_F(SwipeDetectorTest, ResetAfterInactivity) {
    detector_.OnHWheel(300, now_);
    EXPECT_EQ(detector_.GetAccumulatedDelta(), 300);

    // タイムアウト後に新しいイベント → 蓄積リセットされてから加算
    uint64_t after_timeout = now_ + SwipeDetector::RESET_TIMEOUT_MS + 1;
    auto result = detector_.OnHWheel(50, after_timeout);
    EXPECT_EQ(result, SwipeResult::None);
    EXPECT_EQ(detector_.GetAccumulatedDelta(), 50);  // 300はリセットされた
}

TEST_F(SwipeDetectorTest, NoResetWithinTimeout) {
    detector_.OnHWheel(300, now_);

    // タイムアウト内なら蓄積は継続
    auto result = detector_.OnHWheel(100, now_ + SwipeDetector::RESET_TIMEOUT_MS);
    EXPECT_EQ(result, SwipeResult::Back);  // 300 + 100 = 400 >= threshold
}

// ─── 方向の打ち消し ───

TEST_F(SwipeDetectorTest, OppositeDirectionsCancelOut) {
    detector_.OnHWheel(300, now_);
    detector_.OnHWheel(-300, now_ + 10);
    EXPECT_EQ(detector_.GetAccumulatedDelta(), 0);
}

// ─── Reset() で全状態クリア ───

TEST_F(SwipeDetectorTest, ResetClearsAllState) {
    detector_.OnHWheel(200, now_);
    detector_.NotifyVScroll(now_ + 10);
    detector_.Reset();

    EXPECT_EQ(detector_.GetAccumulatedDelta(), 0);
    // リセット後は軸ロックも解除されている
    auto result = detector_.OnHWheel(SwipeDetector::TRIGGER_THRESHOLD, now_ + 20);
    EXPECT_EQ(result, SwipeResult::Back);
}

// ─── 連続ナビゲーション: 発動後すぐに次のスワイプが可能 ───

TEST_F(SwipeDetectorTest, ConsecutiveSwipes) {
    auto r1 = detector_.OnHWheel(SwipeDetector::TRIGGER_THRESHOLD, now_);
    EXPECT_EQ(r1, SwipeResult::Back);

    auto r2 = detector_.OnHWheel(-SwipeDetector::TRIGGER_THRESHOLD, now_ + 100);
    EXPECT_EQ(r2, SwipeResult::Forward);
}

// ─── NotifyVScrollが蓄積をリセットする ───

TEST_F(SwipeDetectorTest, VScrollResetsDelta) {
    detector_.OnHWheel(300, now_);
    EXPECT_EQ(detector_.GetAccumulatedDelta(), 300);

    detector_.NotifyVScroll(now_ + 50);
    EXPECT_EQ(detector_.GetAccumulatedDelta(), 0);
}

// ─── オーバーレイ表示 ───

TEST_F(SwipeDetectorTest, OverlayNotVisibleInitially) {
    EXPECT_FALSE(detector_.IsOverlayVisible());
    EXPECT_EQ(detector_.GetOverlayDirection(), 0);
    EXPECT_FLOAT_EQ(detector_.GetOverlayAlpha(), 0.0f);
}

TEST_F(SwipeDetectorTest, OverlayNotVisibleBelowMinDelta) {
    detector_.OnHWheel(SwipeDetector::OVERLAY_MIN_DELTA - 1, now_);
    EXPECT_FALSE(detector_.IsOverlayVisible());
    EXPECT_EQ(detector_.GetOverlayDirection(), 0);
    EXPECT_FLOAT_EQ(detector_.GetOverlayAlpha(), 0.0f);
}

TEST_F(SwipeDetectorTest, OverlayVisibleAtMinDelta) {
    detector_.OnHWheel(SwipeDetector::OVERLAY_MIN_DELTA, now_);
    EXPECT_TRUE(detector_.IsOverlayVisible());
}

TEST_F(SwipeDetectorTest, OverlayDirectionBackOnPositiveDelta) {
    detector_.OnHWheel(100, now_);
    EXPECT_EQ(detector_.GetOverlayDirection(), -1);  // 戻る
}

TEST_F(SwipeDetectorTest, OverlayDirectionForwardOnNegativeDelta) {
    detector_.OnHWheel(-100, now_);
    EXPECT_EQ(detector_.GetOverlayDirection(), 1);  // 進む
}

TEST_F(SwipeDetectorTest, OverlayAlphaProportionalToProgress) {
    // 閾値の半分
    int half = SwipeDetector::TRIGGER_THRESHOLD / 2;
    detector_.OnHWheel(half, now_);
    float alpha = detector_.GetOverlayAlpha();
    EXPECT_NEAR(alpha, 0.5f, 0.01f);
}

TEST_F(SwipeDetectorTest, OverlayAlphaClampedToOne) {
    // 閾値をわずかに下回る値（発動はしない）
    detector_.OnHWheel(SwipeDetector::TRIGGER_THRESHOLD - 1, now_);
    float alpha = detector_.GetOverlayAlpha();
    EXPECT_LE(alpha, 1.0f);
    EXPECT_GT(alpha, 0.9f);
}

TEST_F(SwipeDetectorTest, OverlayDisappearsAfterTrigger) {
    detector_.OnHWheel(SwipeDetector::TRIGGER_THRESHOLD, now_);
    // 発動後はデルタがリセットされる
    EXPECT_FALSE(detector_.IsOverlayVisible());
    EXPECT_EQ(detector_.GetOverlayDirection(), 0);
    EXPECT_FLOAT_EQ(detector_.GetOverlayAlpha(), 0.0f);
}

TEST_F(SwipeDetectorTest, OverlayDisappearsAfterVScroll) {
    detector_.OnHWheel(200, now_);
    EXPECT_TRUE(detector_.IsOverlayVisible());

    detector_.NotifyVScroll(now_ + 50);
    EXPECT_FALSE(detector_.IsOverlayVisible());
}

TEST_F(SwipeDetectorTest, OverlayDisappearsAfterReset) {
    detector_.OnHWheel(200, now_);
    EXPECT_TRUE(detector_.IsOverlayVisible());

    detector_.Reset();
    EXPECT_FALSE(detector_.IsOverlayVisible());
}
