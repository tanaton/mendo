#include <gtest/gtest.h>
#include "toast_notifier.h"

class ToastNotifierTest : public ::testing::Test {
protected:
    ToastNotifier toast_;
};

// ─── 初期状態 ───

TEST_F(ToastNotifierTest, InitiallyHidden) {
    EXPECT_FALSE(toast_.IsVisible());
    EXPECT_FLOAT_EQ(toast_.GetAlpha(), 0.0f);
    EXPECT_FLOAT_EQ(toast_.GetRenderAlpha(), 0.0f);
    EXPECT_TRUE(toast_.GetMessage().empty());
}

// ─── Show ───

TEST_F(ToastNotifierTest, ShowMakesVisible) {
    toast_.Show(L"テスト");
    EXPECT_TRUE(toast_.IsVisible());
    EXPECT_FLOAT_EQ(toast_.GetAlpha(), ToastNotifier::INITIAL_ALPHA);
    EXPECT_EQ(toast_.GetMessage(), L"テスト");
}

TEST_F(ToastNotifierTest, RenderAlphaClampedDuringHold) {
    toast_.Show(L"test");
    // INITIAL_ALPHA > 1.0 なのでレンダー用アルファは 1.0 にクランプ
    EXPECT_FLOAT_EQ(toast_.GetRenderAlpha(), 1.0f);
}

TEST_F(ToastNotifierTest, ShowOverwritesPrevious) {
    toast_.Show(L"first");
    toast_.Show(L"second");
    EXPECT_EQ(toast_.GetMessage(), L"second");
    EXPECT_FLOAT_EQ(toast_.GetAlpha(), ToastNotifier::INITIAL_ALPHA);
}

// ─── Tick ───

TEST_F(ToastNotifierTest, TickDecrementsAlpha) {
    toast_.Show(L"test");
    float before = toast_.GetAlpha();
    toast_.Tick();
    EXPECT_FLOAT_EQ(toast_.GetAlpha(), before - ToastNotifier::FADE_SPEED);
}

TEST_F(ToastNotifierTest, TickReturnsTrueWhileVisible) {
    toast_.Show(L"test");
    EXPECT_TRUE(toast_.Tick());
}

TEST_F(ToastNotifierTest, TickReturnsFalseWhenDone) {
    toast_.Show(L"test");
    // アルファが 0 以下になるまでティック
    while (toast_.Tick()) {}
    EXPECT_FALSE(toast_.IsVisible());
    EXPECT_FLOAT_EQ(toast_.GetAlpha(), 0.0f);
}

TEST_F(ToastNotifierTest, TickOnIdleReturnsFalse) {
    EXPECT_FALSE(toast_.Tick());
}

TEST_F(ToastNotifierTest, MessageClearedWhenFadeComplete) {
    toast_.Show(L"hello");
    while (toast_.Tick()) {}
    EXPECT_TRUE(toast_.GetMessage().empty());
}

// ─── フェードのタイミング ───

TEST_F(ToastNotifierTest, HoldPhaseKeepsRenderAlphaAtOne) {
    toast_.Show(L"test");
    // ホールド期間中（alpha > 1.0）はレンダーアルファが 1.0 のまま
    int ticks = 0;
    while (toast_.GetAlpha() > 1.0f) {
        EXPECT_FLOAT_EQ(toast_.GetRenderAlpha(), 1.0f);
        toast_.Tick();
        ++ticks;
    }
    // ホールド期間が存在することを確認
    EXPECT_GT(ticks, 0);
}

TEST_F(ToastNotifierTest, FadePhaseDecreaseRenderAlpha) {
    toast_.Show(L"test");
    // ホールド期間をスキップ
    while (toast_.GetAlpha() > 1.0f) {
        toast_.Tick();
    }
    // フェード期間に入ったらレンダーアルファが減少
    float prev = toast_.GetRenderAlpha();
    toast_.Tick();
    EXPECT_LT(toast_.GetRenderAlpha(), prev);
}

TEST_F(ToastNotifierTest, TotalTickCount) {
    toast_.Show(L"test");
    int count = 0;
    while (toast_.Tick()) {
        ++count;
    }
    // INITIAL_ALPHA / FADE_SPEED ≈ 83 ティック
    int expected = static_cast<int>(std::ceil(ToastNotifier::INITIAL_ALPHA / ToastNotifier::FADE_SPEED));
    EXPECT_GE(count, expected - 1);
    EXPECT_LE(count, expected);
}

// ─── Reset ───

TEST_F(ToastNotifierTest, ResetClearsAll) {
    toast_.Show(L"test");
    toast_.Reset();
    EXPECT_FALSE(toast_.IsVisible());
    EXPECT_FLOAT_EQ(toast_.GetAlpha(), 0.0f);
    EXPECT_TRUE(toast_.GetMessage().empty());
}

TEST_F(ToastNotifierTest, ResetDuringFade) {
    toast_.Show(L"test");
    for (int i = 0; i < 10; ++i) { toast_.Tick(); }
    toast_.Reset();
    EXPECT_FALSE(toast_.IsVisible());
    EXPECT_TRUE(toast_.GetMessage().empty());
}

// ─── Alphaはゼロ未満にならない ───

TEST_F(ToastNotifierTest, AlphaNeverNegative) {
    toast_.Show(L"test");
    for (int i = 0; i < 200; ++i) {
        toast_.Tick();
    }
    EXPECT_FLOAT_EQ(toast_.GetAlpha(), 0.0f);
    EXPECT_FLOAT_EQ(toast_.GetRenderAlpha(), 0.0f);
}

// ─── Show中に再Showでリセット ───

TEST_F(ToastNotifierTest, ShowDuringFadeRestartsTimer) {
    toast_.Show(L"first");
    for (int i = 0; i < 30; ++i) { toast_.Tick(); }
    float mid_alpha = toast_.GetAlpha();
    EXPECT_LT(mid_alpha, ToastNotifier::INITIAL_ALPHA);

    toast_.Show(L"second");
    EXPECT_FLOAT_EQ(toast_.GetAlpha(), ToastNotifier::INITIAL_ALPHA);
    EXPECT_EQ(toast_.GetMessage(), L"second");
}
