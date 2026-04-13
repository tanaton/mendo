#include <gtest/gtest.h>
#include "reducer.h"
#include "document.h"

class ReducerTest : public ::testing::Test {
protected:
    AppState state;

    void SetUp() override {
        // スクロール範囲を設定（最大1000.0fまでスクロール可能）
        state.viewport.SyncMaxScroll(1000.0f, 500.0f);
        // PaneLayout のモック（ページサイズ用）
        state.cached_pane_layout.md_rect.height = 500.0f;
        state.pane_layout_valid = true;
    }
};

// ---- KeyScrollAction テスト ----

TEST_F(ReducerTest, KeyScrollLineDown) {
    const float old_scroll = state.viewport.GetScrollY();
    auto effects = Reduce(state, KeyScrollAction{ScrollType::LineDown});

    EXPECT_GT(state.viewport.GetScrollY(), old_scroll);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
    EXPECT_TRUE(HasEffect<effect::BitmapManage>(effects));
}

TEST_F(ReducerTest, KeyScrollLineUp_AtTop_NoEffect) {
    // スクロール位置0で上にスクロール → 変化なし → 副作用なし
    state.viewport.ScrollTo(0.0f);
    auto effects = Reduce(state, KeyScrollAction{ScrollType::LineUp});

    EXPECT_FLOAT_EQ(state.viewport.GetScrollY(), 0.0f);
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, KeyScrollPageDown) {
    const float old_scroll = state.viewport.GetScrollY();
    auto effects = Reduce(state, KeyScrollAction{ScrollType::PageDown});

    // ページスクロールはラインスクロールより大きい
    EXPECT_GT(state.viewport.GetScrollY() - old_scroll, 40.0f);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, KeyScrollHome) {
    state.viewport.ScrollTo(500.0f);
    auto effects = Reduce(state, KeyScrollAction{ScrollType::Home});

    EXPECT_FLOAT_EQ(state.viewport.GetScrollY(), 0.0f);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, KeyScrollEnd) {
    auto effects = Reduce(state, KeyScrollAction{ScrollType::End});

    EXPECT_FLOAT_EQ(state.viewport.GetScrollY(), state.viewport.GetMaxScroll());
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

// ---- DirectScrollByAction テスト ----

TEST_F(ReducerTest, DirectScrollBy_Positive) {
    auto effects = Reduce(state, DirectScrollByAction{100.0f});

    EXPECT_FLOAT_EQ(state.viewport.GetScrollY(), 100.0f);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
    EXPECT_TRUE(HasEffect<effect::BitmapManage>(effects));
}

TEST_F(ReducerTest, DirectScrollBy_ClampedAtMax) {
    auto effects = Reduce(state, DirectScrollByAction{99999.0f});

    EXPECT_FLOAT_EQ(state.viewport.GetScrollY(), state.viewport.GetMaxScroll());
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

// ---- SelectAllAction テスト ----

TEST_F(ReducerTest, SelectAll_WithNodes) {
    state.doc = Document::FromMarkdown(std::pmr::string("Hello world\n\nSecond paragraph"), L"test.md");

    auto effects = Reduce(state, SelectAllAction{});

    EXPECT_TRUE(state.viewport.GetSelection().active);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

// ---- ClearSelectionAction テスト ----

TEST_F(ReducerTest, ClearSelection_WhenNotVisible) {
    // 検索バーが非表示の場合、選択をクリアする
    auto effects = Reduce(state, ClearSelectionAction{});

    EXPECT_FALSE(state.viewport.GetSelection().active);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, ClearSelection_ClosesSearchBar) {
    state.search_state.Show();

    auto effects = Reduce(state, ClearSelectionAction{});

    // 検索バーが閉じられる
    EXPECT_FALSE(state.search_state.IsVisible());
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

// ---- NoOpAction テスト ----

TEST_F(ReducerTest, NoOp_NoStateChange) {
    const float scroll = state.viewport.GetScrollY();
    auto effects = Reduce(state, NoOpAction{});

    EXPECT_FLOAT_EQ(state.viewport.GetScrollY(), scroll);
    EXPECT_TRUE(effects.empty());
}

// ---- ActivateAction テスト ----

TEST_F(ReducerTest, Activate_ChangesWindowActive) {
    state.window_active = true;
    auto effects = Reduce(state, ActivateAction{false});

    EXPECT_FALSE(state.window_active);
    EXPECT_TRUE(HasEffect<effect::InvalidateTitleBar>(effects));
    EXPECT_TRUE(HasEffect<effect::ClearTooltip>(effects));
}

TEST_F(ReducerTest, Activate_NoChangeWhenSame) {
    state.window_active = true;
    auto effects = Reduce(state, ActivateAction{true});

    // 状態変化なし → InvalidateTitleBar なし
    EXPECT_FALSE(HasEffect<effect::InvalidateTitleBar>(effects));
}

// ---- EnterSizeMoveAction テスト ----

TEST_F(ReducerTest, EnterSizeMove_SetsFlag) {
    state.is_sizing = false;
    Reduce(state, EnterSizeMoveAction{});

    EXPECT_TRUE(state.is_sizing);
}

// ---- MouseLeaveAction テスト ----

TEST_F(ReducerTest, MouseLeave_ClearsTooltip) {
    auto effects = Reduce(state, MouseLeaveAction{});
    EXPECT_TRUE(HasEffect<effect::ClearTooltip>(effects));
}

// ---- 未処理アクションテスト ----

TEST_F(ReducerTest, ReloadFileAction_EmitsReloadEffect) {
    auto effects = Reduce(state, ReloadFileAction{});
    EXPECT_EQ(effects.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<effect::ReloadFile>(effects[0]));
}

TEST_F(ReducerTest, NoOpAction_EmptyEffects) {
    // NoOpAction は空の副作用リストを返す
    auto effects = Reduce(state, NoOpAction{});
    EXPECT_TRUE(effects.empty());
}
