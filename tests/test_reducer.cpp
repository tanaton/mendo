#include <gtest/gtest.h>
#include "reducer.h"
#include "document.h"

class ReducerTest : public ::testing::Test {
protected:
    AppState state;

    void SetUp() override {
        // スクロール範囲を設定（最大1000.0fまでスクロール可能）
        state.view.viewport.SyncMaxScroll(1000.0f, 500.0f);
        // PaneLayout のモック（ページサイズ用）
        state.cached_pane_layout.md_rect.height = 500.0f;
        state.pane_layout_valid = true;
    }
};

// ---- KeyScrollAction テスト ----

TEST_F(ReducerTest, KeyScrollLineDown) {
    const float old_scroll = state.view.viewport.GetScrollY();
    auto effects = Reduce(state, KeyScrollAction{ScrollType::LineDown});

    EXPECT_GT(state.view.viewport.GetScrollY(), old_scroll);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
    EXPECT_TRUE(HasEffect<effect::BitmapManage>(effects));
}

TEST_F(ReducerTest, KeyScrollLineUp_AtTop_NoEffect) {
    // スクロール位置0で上にスクロール → 変化なし → 副作用なし
    state.view.viewport.ScrollTo(0.0f);
    auto effects = Reduce(state, KeyScrollAction{ScrollType::LineUp});

    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), 0.0f);
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, KeyScrollPageDown) {
    const float old_scroll = state.view.viewport.GetScrollY();
    auto effects = Reduce(state, KeyScrollAction{ScrollType::PageDown});

    // ページスクロールはラインスクロールより大きい
    EXPECT_GT(state.view.viewport.GetScrollY() - old_scroll, 40.0f);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, KeyScrollHome) {
    state.view.viewport.ScrollTo(500.0f);
    auto effects = Reduce(state, KeyScrollAction{ScrollType::Home});

    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), 0.0f);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, KeyScrollEnd) {
    auto effects = Reduce(state, KeyScrollAction{ScrollType::End});

    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), state.view.viewport.GetMaxScroll());
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

// ---- DirectScrollByAction テスト ----

TEST_F(ReducerTest, DirectScrollBy_Positive) {
    auto effects = Reduce(state, DirectScrollByAction{100.0f});

    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), 100.0f);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
    EXPECT_TRUE(HasEffect<effect::BitmapManage>(effects));
}

TEST_F(ReducerTest, DirectScrollBy_ClampedAtMax) {
    auto effects = Reduce(state, DirectScrollByAction{99999.0f});

    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), state.view.viewport.GetMaxScroll());
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

// ---- SelectAllAction テスト ----

TEST_F(ReducerTest, SelectAll_WithNodes) {
    state.document.doc = Document::FromMarkdown(std::pmr::string("Hello world\n\nSecond paragraph"), L"test.md");

    auto effects = Reduce(state, SelectAllAction{});

    EXPECT_TRUE(state.view.viewport.GetSelection().active);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

// ---- ClearSelectionAction テスト ----

TEST_F(ReducerTest, ClearSelection_WhenNotVisible) {
    // 検索バーが非表示の場合、選択をクリアする
    auto effects = Reduce(state, ClearSelectionAction{});

    EXPECT_FALSE(state.view.viewport.GetSelection().active);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, ClearSelection_ClosesSearchBar) {
    state.search.search_state.Show();

    auto effects = Reduce(state, ClearSelectionAction{});

    // 検索バーが閉じられる
    EXPECT_FALSE(state.search.search_state.IsVisible());
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

// ---- NoOpAction テスト ----

TEST_F(ReducerTest, NoOp_NoStateChange) {
    const float scroll = state.view.viewport.GetScrollY();
    auto effects = Reduce(state, NoOpAction{});

    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), scroll);
    EXPECT_TRUE(effects.empty());
}

// ---- ActivateAction テスト ----

TEST_F(ReducerTest, Activate_ChangesWindowActive) {
    state.window.window_active = true;
    auto effects = Reduce(state, ActivateAction{false});

    EXPECT_FALSE(state.window.window_active);
    EXPECT_TRUE(HasEffect<effect::InvalidateTitleBar>(effects));
    EXPECT_TRUE(HasEffect<effect::ClearTooltip>(effects));
}

TEST_F(ReducerTest, Activate_NoChangeWhenSame) {
    state.window.window_active = true;
    auto effects = Reduce(state, ActivateAction{true});

    // 状態変化なし → InvalidateTitleBar なし
    EXPECT_FALSE(HasEffect<effect::InvalidateTitleBar>(effects));
}

// ---- EnterSizeMoveAction テスト ----

TEST_F(ReducerTest, EnterSizeMove_SetsFlag) {
    state.window.is_sizing = false;
    Reduce(state, EnterSizeMoveAction{});

    EXPECT_TRUE(state.window.is_sizing);
}

// ---- MouseLeaveAction テスト ----

TEST_F(ReducerTest, MouseLeave_ClearsTooltip) {
    auto effects = Reduce(state, MouseLeaveAction{});
    EXPECT_TRUE(HasEffect<effect::ClearTooltip>(effects));
}

// ---- コマンド系アクションテスト ----

TEST_F(ReducerTest, ReloadFileAction_EmitsReloadEffect) {
    auto effects = Reduce(state, ReloadFileAction{});
    EXPECT_EQ(effects.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<effect::ReloadFile>(effects[0]));
}

TEST_F(ReducerTest, OpenFileAction_EmitsOpenFileDialog) {
    auto effects = Reduce(state, OpenFileAction{});
    EXPECT_EQ(effects.size(), 1u);
    EXPECT_TRUE(HasEffect<effect::OpenFileDialog>(effects));
}

TEST_F(ReducerTest, FileWatchAction_EmitsCheckFileChanges) {
    auto effects = Reduce(state, FileWatchAction{});
    EXPECT_EQ(effects.size(), 1u);
    EXPECT_TRUE(HasEffect<effect::CheckFileChanges>(effects));
}

TEST_F(ReducerTest, ImageLoadedAction_EmitsNotifyImageLoaded) {
    auto effects = Reduce(state, ImageLoadedAction{});
    EXPECT_EQ(effects.size(), 1u);
    EXPECT_TRUE(HasEffect<effect::NotifyImageLoaded>(effects));
}

// ---- ナビゲーションテスト ----

TEST_F(ReducerTest, NavigateBack_NoHistory_NoEffects) {
    auto effects = Reduce(state, NavigateBackAction{});
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, NavigateBack_DifferentFile_EmitsLoadFile) {
    state.document.doc = Document::FromMarkdown(std::pmr::string("test"), L"C:\\current.md");
    state.view.nav_history.Push(NavEntry{ L"C:\\prev.md", 50.0f });

    auto effects = Reduce(state, NavigateBackAction{});
    EXPECT_TRUE(HasEffect<effect::LoadFile>(effects));
}

TEST_F(ReducerTest, NavigateBack_SameFile_ScrollsAndInvalidates) {
    state.document.doc = Document::FromMarkdown(std::pmr::string("test"), L"C:\\file.md");
    state.view.nav_history.Push(NavEntry{ L"C:\\file.md", 50.0f });
    state.view.viewport.ScrollTo(200.0f);

    auto effects = Reduce(state, NavigateBackAction{});
    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), 50.0f);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
    EXPECT_TRUE(HasEffect<effect::BitmapManage>(effects));
}

TEST_F(ReducerTest, NavigateForward_NoHistory_NoEffects) {
    auto effects = Reduce(state, NavigateForwardAction{});
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, XButtonBack_DelegatesToNavigateBack) {
    state.document.doc = Document::FromMarkdown(std::pmr::string("test"), L"C:\\current.md");
    state.view.nav_history.Push(NavEntry{ L"C:\\prev.md", 0.0f });

    auto effects = Reduce(state, XButtonBackAction{});
    EXPECT_TRUE(HasEffect<effect::LoadFile>(effects));
}

// ---- DropFiles / ShowHelp テスト ----

TEST_F(ReducerTest, DropFiles_PushesHistoryAndLoads) {
    state.document.doc = Document::FromMarkdown(std::pmr::string("test"), L"C:\\current.md");

    auto effects = Reduce(state, DropFilesAction{ std::pmr::wstring(L"C:\\dropped.md") });
    EXPECT_TRUE(HasEffect<effect::LoadFile>(effects));
    EXPECT_TRUE(state.view.nav_history.CanGoBack());
}

TEST_F(ReducerTest, DropFiles_EmptyDoc_NoPush) {
    // ドキュメントが空ならナビゲーション履歴にプッシュしない
    auto effects = Reduce(state, DropFilesAction{ std::pmr::wstring(L"C:\\dropped.md") });
    EXPECT_TRUE(HasEffect<effect::LoadFile>(effects));
    EXPECT_FALSE(state.view.nav_history.CanGoBack());
}

TEST_F(ReducerTest, ShowHelp_EmitsLoadFile) {
    state.document.doc = Document::FromMarkdown(std::pmr::string("test"), L"C:\\file.md");

    auto effects = Reduce(state, ShowHelpAction{});
    EXPECT_TRUE(HasEffect<effect::LoadFile>(effects));
    EXPECT_TRUE(state.view.nav_history.CanGoBack());
}

// ---- リサイズ / DPI テスト ----

TEST_F(ReducerTest, Resize_ZeroSize_NoEffects) {
    auto effects = Reduce(state, ResizeAction{ 0, 0 });
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, Resize_Normal_EmitsResizeEnd) {
    state.window.cached_dpi_scale = 1.0f;
    auto effects = Reduce(state, ResizeAction{ 800, 600 });

    EXPECT_TRUE(HasEffect<effect::RendererResize>(effects));
    EXPECT_TRUE(HasEffect<effect::PerformResizeEnd>(effects));
    EXPECT_FALSE(state.pane_layout_valid);
}

TEST_F(ReducerTest, Resize_Sizing_EmitsSizingUpdate) {
    state.window.is_sizing = true;
    state.window.cached_dpi_scale = 1.0f;
    auto effects = Reduce(state, ResizeAction{ 800, 600 });

    EXPECT_TRUE(HasEffect<effect::RendererResize>(effects));
    EXPECT_TRUE(HasEffect<effect::PerformSizingUpdate>(effects));
    EXPECT_FALSE(HasEffect<effect::PerformResizeEnd>(effects));
}

TEST_F(ReducerTest, DpiChanged_UpdatesScale) {
    auto effects = Reduce(state, DpiChangedAction{ 192, RECT{0, 0, 800, 600} });

    EXPECT_FLOAT_EQ(state.window.cached_dpi_scale, 2.0f);
    EXPECT_TRUE(HasEffect<effect::RendererSetDpi>(effects));
    EXPECT_TRUE(HasEffect<effect::ClearFileCache>(effects));
    EXPECT_TRUE(HasEffect<effect::SetWindowPosition>(effects));
}

// ---- テーマ / ズームテスト ----

TEST_F(ReducerTest, ToggleDarkMode_EmitsApplyThemeChange) {
    auto effects = Reduce(state, ToggleDarkModeAction{});

    EXPECT_TRUE(HasEffect<effect::ApplyThemeChange>(effects));
    EXPECT_FALSE(state.pane_layout_valid);
}

TEST_F(ReducerTest, ZoomIn_EmitsApplyThemeChange) {
    state.window.cached_theme.zoom = 1.0f;
    auto effects = Reduce(state, ZoomAction{ ZoomDirection::In });

    EXPECT_TRUE(HasEffect<effect::ApplyThemeChange>(effects));
    EXPECT_FALSE(state.pane_layout_valid);
}

// ---- HWheel テスト ----

TEST_F(ReducerTest, HWheel_EmitsSetTimer) {
    auto effects = Reduce(state, HWheelAction{ 120, 1000 });
    EXPECT_TRUE(HasEffect<effect::SetTimer>(effects));
}

// ---- CaptureChanged テスト ----

TEST_F(ReducerTest, CaptureChanged_ResetsGesture) {
    // ジェスチャーがアクティブでない場合、InvalidateWindow は出ない
    auto effects = Reduce(state, CaptureChangedAction{});
    EXPECT_FALSE(HasEffect<effect::InvalidateWindow>(effects));
}

// ---- タイマーテスト（代表ケース） ----

TEST_F(ReducerTest, Timer_Toast_EmitsInvalidate) {
    state.interaction.toast.Show(L"test");
    auto effects = Reduce(state, TimerAction{ 6 }); // app_timer::TOAST = 6
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, Timer_DeferredLayout_EmitsProcessDeferredLayout) {
    auto effects = Reduce(state, TimerAction{ 3 }); // app_timer::DEFERRED_LAYOUT = 3
    EXPECT_TRUE(HasEffect<effect::ProcessDeferredLayout>(effects));
}

// ---- Destroy / ParseComplete テスト ----

TEST_F(ReducerTest, Destroy_EmitsDestroyEffect) {
    auto effects = Reduce(state, DestroyAction{});
    EXPECT_EQ(effects.size(), 1u);
    EXPECT_TRUE(HasEffect<effect::Destroy>(effects));
}

TEST_F(ReducerTest, ParseComplete_EmitsHandleParseComplete) {
    auto effects = Reduce(state, ParseCompleteAction{});
    EXPECT_EQ(effects.size(), 1u);
    EXPECT_TRUE(HasEffect<effect::HandleParseComplete>(effects));
}
