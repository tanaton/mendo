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

// ---- UpdateTooltipAction / ClearTooltipAction テスト ----

TEST_F(ReducerTest, UpdateTooltipAction_EmitsShowTooltipEffectWithSameTarget) {
    TooltipTarget target{ TooltipTarget::Zone::MdLink, L"https://example.com" };
    auto effects = Reduce(state, UpdateTooltipAction{ target, 100, 200 });

    ASSERT_EQ(effects.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<effect::ShowTooltip>(effects[0]));
    const auto& e = std::get<effect::ShowTooltip>(effects[0]);
    EXPECT_EQ(e.target, target);
    EXPECT_EQ(e.px, 100);
    EXPECT_EQ(e.py, 200);
}

TEST_F(ReducerTest, UpdateTooltipAction_SameTargetAsCurrent_NoEffect) {
    // 現在のターゲットと同一なら変更検出でスキップ（初期状態は空 → 空アクションは no-op）
    auto effects = Reduce(state, UpdateTooltipAction{ TooltipTarget{}, 0, 0 });
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, UpdateTooltipAction_ClearsAfterPrevTarget) {
    // 既にターゲットが設定済みなら、空ターゲットへの遷移で ShowTooltip が発行される（Executor でタイマー停止）
    state.interaction.tooltip.Update(
        TooltipTarget{ TooltipTarget::Zone::MdLink, L"x" }, 0, 0);

    auto effects = Reduce(state, UpdateTooltipAction{ TooltipTarget{}, 0, 0 });
    ASSERT_EQ(effects.size(), 1u);
    EXPECT_TRUE(HasEffect<effect::ShowTooltip>(effects));
}

TEST_F(ReducerTest, ClearTooltipAction_EmitsClearTooltipAndResetsState) {
    state.interaction.tooltip.Update(
        TooltipTarget{ TooltipTarget::Zone::MdLink, L"x" }, 0, 0);

    auto effects = Reduce(state, ClearTooltipAction{});

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
    auto effects = Reduce(state, DpiChangedAction{ 192, PixelRect{0, 0, 800, 600} });

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

// ---- MdPaneNavHoverAction テスト ----

TEST_F(ReducerTest, NavHover_NoneToBack_UpdatesStateAndInvalidates) {
    using Hover = NavButtonHover;
    state.interaction.nav_hover = Hover::None;
    state.interaction.hovered_copy_node = 3;
    state.interaction.hovered_save_node = 5;

    auto effects = Reduce(state, MdPaneNavHoverAction{ Hover::Back });

    EXPECT_EQ(state.interaction.nav_hover, Hover::Back);
    // ナビボタンホバー開始でコピー/保存ボタンのホバーはリセット
    EXPECT_EQ(state.interaction.hovered_copy_node, -1);
    EXPECT_EQ(state.interaction.hovered_save_node, -1);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, NavHover_BackToNone_UpdatesStateAndInvalidates) {
    using Hover = NavButtonHover;
    state.interaction.nav_hover = Hover::Back;
    state.interaction.hovered_copy_node = -1;
    state.interaction.hovered_save_node = -1;

    auto effects = Reduce(state, MdPaneNavHoverAction{ Hover::None });

    EXPECT_EQ(state.interaction.nav_hover, Hover::None);
    // ナビ離脱時はコピー/保存の状態は触らない
    EXPECT_EQ(state.interaction.hovered_copy_node, -1);
    EXPECT_EQ(state.interaction.hovered_save_node, -1);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, NavHover_Unchanged_NoEffect) {
    using Hover = NavButtonHover;
    state.interaction.nav_hover = Hover::Forward;
    state.interaction.hovered_copy_node = 7;

    auto effects = Reduce(state, MdPaneNavHoverAction{ Hover::Forward });

    EXPECT_EQ(state.interaction.nav_hover, Hover::Forward);
    // 同値ならコピー/保存ホバーもそのまま
    EXPECT_EQ(state.interaction.hovered_copy_node, 7);
    EXPECT_TRUE(effects.empty());
}

// ---- MdPaneButtonHoverChangedAction テスト ----

TEST_F(ReducerTest, ButtonHoverChanged_CopyUpdated_Invalidates) {
    state.interaction.hovered_copy_node = -1;
    state.interaction.hovered_save_node = -1;

    auto effects = Reduce(state, MdPaneButtonHoverChangedAction{ 3, -1 });

    EXPECT_EQ(state.interaction.hovered_copy_node, 3);
    EXPECT_EQ(state.interaction.hovered_save_node, -1);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, ButtonHoverChanged_SaveUpdated_Invalidates) {
    state.interaction.hovered_copy_node = -1;
    state.interaction.hovered_save_node = -1;

    auto effects = Reduce(state, MdPaneButtonHoverChangedAction{ -1, 5 });

    EXPECT_EQ(state.interaction.hovered_copy_node, -1);
    EXPECT_EQ(state.interaction.hovered_save_node, 5);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, ButtonHoverChanged_Unchanged_NoEffect) {
    state.interaction.hovered_copy_node = 2;
    state.interaction.hovered_save_node = 4;

    auto effects = Reduce(state, MdPaneButtonHoverChangedAction{ 2, 4 });

    EXPECT_EQ(state.interaction.hovered_copy_node, 2);
    EXPECT_EQ(state.interaction.hovered_save_node, 4);
    EXPECT_TRUE(effects.empty());
}

// ---- SplitterDrag* テスト ----

TEST_F(ReducerTest, SplitterDragStarted_Splitter1_SetsTargetAndCaptures) {
    auto effects = Reduce(state, SplitterDragStartedAction{ PaneController::DragTarget::Splitter1 });
    EXPECT_EQ(state.view.panes.GetDragTarget(), PaneController::DragTarget::Splitter1);
    EXPECT_TRUE(HasEffect<effect::SetCapture>(effects));
}

TEST_F(ReducerTest, SplitterDragStarted_Splitter2_SetsTargetAndCaptures) {
    auto effects = Reduce(state, SplitterDragStartedAction{ PaneController::DragTarget::Splitter2 });
    EXPECT_EQ(state.view.panes.GetDragTarget(), PaneController::DragTarget::Splitter2);
    EXPECT_TRUE(HasEffect<effect::SetCapture>(effects));
}

TEST_F(ReducerTest, SplitterDragStarted_InvalidTarget_NoOp) {
    // スプリッター以外のドラッグターゲットは無視される
    auto effects = Reduce(state, SplitterDragStartedAction{ PaneController::DragTarget::MdScrollbar });
    EXPECT_EQ(state.view.panes.GetDragTarget(), PaneController::DragTarget::None);
    EXPECT_FALSE(HasEffect<effect::SetCapture>(effects));
}

TEST_F(ReducerTest, SplitterDragMoved_Splitter1_UpdatesWidthAndInvalidates) {
    state.window.cached_theme.splitter_width = 4.0f;
    state.pane_layout_valid = true;
    const float old_width = state.view.panes.GetFilePaneWidth();

    auto effects = Reduce(state, SplitterDragMovedAction{
        PaneController::DragTarget::Splitter1, 300.0f, 1000.0f });

    EXPECT_NE(state.view.panes.GetFilePaneWidth(), old_width);
    EXPECT_FALSE(state.pane_layout_valid);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, SplitterDragMoved_Splitter2_UpdatesTocWidthAndInvalidates) {
    state.window.cached_theme.splitter_width = 4.0f;
    state.pane_layout_valid = true;

    // Splitter2 は toc_left 基準で toc_width を変更する
    auto effects = Reduce(state, SplitterDragMovedAction{
        PaneController::DragTarget::Splitter2, 600.0f, 1000.0f });

    EXPECT_FALSE(state.pane_layout_valid);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, SplitterDragMoved_InvalidTarget_NoOp) {
    state.pane_layout_valid = true;
    auto effects = Reduce(state, SplitterDragMovedAction{
        PaneController::DragTarget::FileScrollbar, 300.0f, 1000.0f });

    EXPECT_TRUE(state.pane_layout_valid);
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, SplitterDragEnded_ReleasesAndRefreshes) {
    state.view.panes.StartDrag(PaneController::DragTarget::Splitter1);
    state.pane_layout_valid = true;

    auto effects = Reduce(state, SplitterDragEndedAction{});

    EXPECT_EQ(state.view.panes.GetDragTarget(), PaneController::DragTarget::None);
    EXPECT_FALSE(state.pane_layout_valid);
    EXPECT_TRUE(HasEffect<effect::ReleaseCapture>(effects));
    EXPECT_TRUE(HasEffect<effect::PerformResizeEnd>(effects));
}

// ---- SearchInputDrag* テスト ----

TEST_F(ReducerTest, SearchInputDragStarted_BeginsDragAndCaptures) {
    auto effects = Reduce(state, SearchInputDragStartedAction{ 5 });

    EXPECT_TRUE(state.search.search_bar_ctrl.IsDragging());
    EXPECT_EQ(state.search.search_bar_ctrl.GetDragAnchor(), 5);
    EXPECT_TRUE(HasEffect<effect::SetCapture>(effects));
    EXPECT_TRUE(HasEffect<effect::PostMessage>(effects));
}

TEST_F(ReducerTest, SearchInputDragMoved_NotDragging_NoOp) {
    auto effects = Reduce(state, SearchInputDragMovedAction{ 3 });
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, SearchInputDragMoved_DraggingAndChanged_EmitsPostMessage) {
    state.search.search_bar_ctrl.StartDrag(3);
    auto effects = Reduce(state, SearchInputDragMovedAction{ 7 });
    EXPECT_TRUE(HasEffect<effect::PostMessage>(effects));
}

TEST_F(ReducerTest, SearchInputDragMoved_Unchanged_NoEffect) {
    state.search.search_bar_ctrl.StartDrag(5);
    // caret と selection_start を drag_anchor に合わせると変化なしで no-op
    state.search.search_bar_ctrl.SetSelection(5, 5);
    auto effects = Reduce(state, SearchInputDragMovedAction{ 5 });
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, SearchInputDragEnded_EndsDragAndReleases) {
    state.search.search_bar_ctrl.StartDrag(3);

    auto effects = Reduce(state, SearchInputDragEndedAction{});

    EXPECT_FALSE(state.search.search_bar_ctrl.IsDragging());
    EXPECT_TRUE(HasEffect<effect::ReleaseCapture>(effects));
}

// ---- MdScrollbarDrag* テスト ----

TEST_F(ReducerTest, MdScrollbarDragStarted_ThumbHit_StoresOffsetOnly) {
    // fixture で md_rect.height = 500, total_height = 1000 → content_height=500, max_scroll=500
    // thumb_height = max(24, 500 * 500/1000) = 250, scroll_y=0 → thumb_y = 0
    state.view.viewport.ScrollTo(0.0f);
    const float dip_y = 100.0f; // thumb 内（0〜250）
    const float old_scroll = state.view.viewport.GetScrollY();

    auto effects = Reduce(state, MdScrollbarDragStartedAction{ dip_y, 1000.0f });

    EXPECT_EQ(state.view.panes.GetDragTarget(), PaneController::DragTarget::MdScrollbar);
    EXPECT_TRUE(state.view.viewport.IsScrollbarTracking());
    EXPECT_TRUE(HasEffect<effect::SetCapture>(effects));
    EXPECT_FLOAT_EQ(state.view.panes.GetDragScrollOffset(), dip_y);
    // つまみ上クリックではスクロール位置は変化しない
    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), old_scroll);
    EXPECT_FALSE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, MdScrollbarDragStarted_ThumbMiss_JumpsAndEmitsInvalidate) {
    // つまみ外クリック (thumb 領域外の dip_y=400)
    state.view.viewport.ScrollTo(0.0f);
    const float dip_y = 400.0f;

    auto effects = Reduce(state, MdScrollbarDragStartedAction{ dip_y, 1000.0f });

    EXPECT_EQ(state.view.panes.GetDragTarget(), PaneController::DragTarget::MdScrollbar);
    EXPECT_TRUE(HasEffect<effect::SetCapture>(effects));
    EXPECT_GT(state.view.viewport.GetScrollY(), 0.0f);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
    EXPECT_TRUE(HasEffect<effect::BitmapManage>(effects));
}

TEST_F(ReducerTest, MdScrollbarDragMoved_WhileDragging_UpdatesScroll) {
    state.view.panes.StartDrag(PaneController::DragTarget::MdScrollbar);
    state.view.panes.SetDragScrollOffset(0.0f);
    state.view.viewport.ScrollTo(0.0f);

    auto effects = Reduce(state, MdScrollbarDragMovedAction{ 200.0f, 1000.0f });

    EXPECT_GT(state.view.viewport.GetScrollY(), 0.0f);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, MdScrollbarDragMoved_NotDragging_NoOp) {
    // drag_target が MdScrollbar でない → 何もしない
    auto effects = Reduce(state, MdScrollbarDragMovedAction{ 200.0f, 1000.0f });
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, MdScrollbarDragEnded_ReleasesAndSchedulesResize) {
    state.view.panes.StartDrag(PaneController::DragTarget::MdScrollbar);
    state.view.viewport.SetScrollbarTracking(true);

    auto effects = Reduce(state, MdScrollbarDragEndedAction{});

    EXPECT_EQ(state.view.panes.GetDragTarget(), PaneController::DragTarget::None);
    EXPECT_FALSE(state.view.viewport.IsScrollbarTracking());
    EXPECT_TRUE(HasEffect<effect::ReleaseCapture>(effects));
    EXPECT_TRUE(HasEffect<effect::PerformResizeEnd>(effects));
    EXPECT_TRUE(HasEffect<effect::BitmapManage>(effects));
}

TEST_F(ReducerTest, MdScrollbarDragEnded_NotDragging_NoOp) {
    auto effects = Reduce(state, MdScrollbarDragEndedAction{});
    EXPECT_TRUE(effects.empty());
}

// ---- PaneScrollbarDrag* テスト（Toc ペイン経由）----

namespace {

// 目次 30 件を持つドキュメントをセットアップし、Toc ペインが縦にあふれる状態を作る。
void SetupScrollableToc(AppState& state) {
    std::string md;
    for (int i = 0; i < 30; ++i) {
        md += "# Heading ";
        md += std::to_string(i);
        md += "\n\n";
    }
    state.document.doc = Document::FromMarkdown(std::pmr::string(md), L"test.md");
    state.cached_pane_layout.toc_rect = { 0.0f, 0.0f, 200.0f, 300.0f };
    // item_height=28, header_height=32 → content_height=268, total=30*28=840 → scrollable
}

} // namespace

TEST_F(ReducerTest, PaneScrollbarDragStarted_NotScrollable_NoOp) {
    // エントリが少なく total_content <= content_height → スクロール不要で no-op
    state.cached_pane_layout.toc_rect = { 0.0f, 0.0f, 200.0f, 300.0f };
    // 既定の Toc (エントリ 0) で総コンテンツ 0 → スクロール不要
    auto effects = Reduce(state, PaneScrollbarDragStartedAction{ PaneTarget::Toc, 100.0f });
    EXPECT_EQ(state.view.panes.GetDragTarget(), PaneController::DragTarget::None);
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, PaneScrollbarDragStarted_TocScrollable_ThumbHit_StoresOffset) {
    SetupScrollableToc(state);
    // header_height=32 → content_top=32。scroll=0 の時 thumb_y = content_top = 32
    const float dip_y = 40.0f; // thumb 内を想定
    auto& scroll = state.view.panes.TocScroll();
    scroll.scroll_y = 0.0f;

    auto effects = Reduce(state, PaneScrollbarDragStartedAction{ PaneTarget::Toc, dip_y });

    EXPECT_EQ(state.view.panes.GetDragTarget(), PaneController::DragTarget::TocScrollbar);
    EXPECT_TRUE(HasEffect<effect::SetCapture>(effects));
    // thumb 上クリックではスクロール位置は不変
    EXPECT_FLOAT_EQ(state.view.panes.TocScroll().scroll_y, 0.0f);
    EXPECT_FALSE(HasEffect<effect::InvalidatePaneCache>(effects));
}

TEST_F(ReducerTest, PaneScrollbarDragStarted_TocScrollable_ThumbMiss_Jumps) {
    SetupScrollableToc(state);
    const float dip_y = 250.0f; // thumb 外（下側）
    state.view.panes.TocScroll().scroll_y = 0.0f;

    auto effects = Reduce(state, PaneScrollbarDragStartedAction{ PaneTarget::Toc, dip_y });

    EXPECT_EQ(state.view.panes.GetDragTarget(), PaneController::DragTarget::TocScrollbar);
    EXPECT_TRUE(HasEffect<effect::SetCapture>(effects));
    EXPECT_GT(state.view.panes.TocScroll().scroll_y, 0.0f);
    EXPECT_TRUE(HasEffect<effect::InvalidatePaneCache>(effects));
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, PaneScrollbarDragMoved_WhileDragging_UpdatesScroll) {
    SetupScrollableToc(state);
    state.view.panes.StartDrag(PaneController::DragTarget::TocScrollbar);
    state.view.panes.SetDragScrollOffset(0.0f);
    state.view.panes.TocScroll().scroll_y = 0.0f;

    auto effects = Reduce(state, PaneScrollbarDragMovedAction{ PaneTarget::Toc, 200.0f });

    EXPECT_GT(state.view.panes.TocScroll().scroll_y, 0.0f);
    EXPECT_TRUE(HasEffect<effect::InvalidatePaneCache>(effects));
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, PaneScrollbarDragMoved_WrongTarget_NoOp) {
    // drag_target が File の時に Toc への Move が来た → no-op
    state.view.panes.StartDrag(PaneController::DragTarget::FileScrollbar);
    auto effects = Reduce(state, PaneScrollbarDragMovedAction{ PaneTarget::Toc, 200.0f });
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, PaneScrollbarDragEnded_TocOrFile_Releases) {
    state.view.panes.StartDrag(PaneController::DragTarget::TocScrollbar);

    auto effects = Reduce(state, PaneScrollbarDragEndedAction{});

    EXPECT_EQ(state.view.panes.GetDragTarget(), PaneController::DragTarget::None);
    EXPECT_TRUE(HasEffect<effect::ReleaseCapture>(effects));
}

TEST_F(ReducerTest, PaneScrollbarDragEnded_NotDragging_NoOp) {
    auto effects = Reduce(state, PaneScrollbarDragEndedAction{});
    EXPECT_TRUE(effects.empty());
}

// ---- TextSelection* テスト ----

TEST_F(ReducerTest, TextSelectionStarted_WithHit_SetsAnchorAndCaptures) {
    state.document.doc = Document::FromMarkdown(std::pmr::string("Hello world"), L"test.md");
    // 既存の選択を残しておき、クリア動作を検証
    state.view.viewport.SetSelection(TextSelection::MakeOrdered(0, 0, 0, 5));

    auto effects = Reduce(state, TextSelectionStartedAction{ 0, 3u, 10, 20 });

    EXPECT_EQ(state.view.viewport.GetClickStartX(), 10);
    EXPECT_EQ(state.view.viewport.GetClickStartY(), 20);
    EXPECT_EQ(state.view.viewport.GetAnchorNode(), 0);
    EXPECT_EQ(state.view.viewport.GetAnchorPos(), 3u);
    EXPECT_TRUE(state.view.viewport.IsDragging());
    EXPECT_FALSE(state.view.viewport.GetSelection().active);
    EXPECT_TRUE(HasEffect<effect::SetCapture>(effects));
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, TextSelectionStarted_NoHit_StoresClickStartOnly) {
    // ヒットなし（空白領域のクリック）: ClickStart は記録するが SetCapture も anchor/dragging も変えない。
    // SetCapture をスキップすることで、対応する ReleaseCapture を省略できる。
    auto effects = Reduce(state, TextSelectionStartedAction{ -1, 0u, 5, 7 });

    EXPECT_EQ(state.view.viewport.GetClickStartX(), 5);
    EXPECT_EQ(state.view.viewport.GetClickStartY(), 7);
    EXPECT_FALSE(state.view.viewport.IsDragging());
    EXPECT_FALSE(HasEffect<effect::SetCapture>(effects));
    EXPECT_FALSE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, TextSelectionMoved_NotDragging_NoOp) {
    state.document.doc = Document::FromMarkdown(std::pmr::string("Hello"), L"test.md");
    auto effects = Reduce(state, TextSelectionMovedAction{ 0, 3u });
    EXPECT_FALSE(state.view.viewport.GetSelection().active);
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, TextSelectionMoved_DraggingWithHit_UpdatesSelection) {
    state.document.doc = Document::FromMarkdown(std::pmr::string("Hello world"), L"test.md");
    state.view.viewport.SetAnchor(0, 2u);
    state.view.viewport.SetDragging(true);

    auto effects = Reduce(state, TextSelectionMovedAction{ 0, 7u });

    const auto& sel = state.view.viewport.GetSelection();
    EXPECT_TRUE(sel.active);
    EXPECT_EQ(sel.start_node, 0);
    EXPECT_EQ(sel.end_node, 0);
    EXPECT_EQ(sel.start_pos, 2u);
    EXPECT_EQ(sel.end_pos, 7u);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, TextSelectionMoved_NoHit_NoUpdate) {
    state.view.viewport.SetAnchor(0, 2u);
    state.view.viewport.SetDragging(true);

    auto effects = Reduce(state, TextSelectionMovedAction{ -1, 0u });

    EXPECT_FALSE(state.view.viewport.GetSelection().active);
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, TextSelectionEnded_NotDragging_NoOp) {
    auto effects = Reduce(state, TextSelectionEndedAction{ 0, 3u });
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, TextSelectionEnded_WithHit_FinalizesSelection) {
    state.document.doc = Document::FromMarkdown(std::pmr::string("Hello world"), L"test.md");
    state.view.viewport.SetAnchor(0, 2u);
    state.view.viewport.SetDragging(true);

    auto effects = Reduce(state, TextSelectionEndedAction{ 0, 8u });

    const auto& sel = state.view.viewport.GetSelection();
    EXPECT_TRUE(sel.active);
    EXPECT_EQ(sel.start_pos, 2u);
    EXPECT_EQ(sel.end_pos, 8u);
    EXPECT_FALSE(state.view.viewport.IsDragging());
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, TextSelectionEnded_NoHit_ClearsDraggingOnly) {
    state.view.viewport.SetAnchor(0, 2u);
    state.view.viewport.SetDragging(true);
    // 事前選択なし
    auto effects = Reduce(state, TextSelectionEndedAction{ -1, 0u });

    EXPECT_FALSE(state.view.viewport.IsDragging());
    EXPECT_FALSE(state.view.viewport.GetSelection().active);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

// ---- RightClickGesture* テスト ----

TEST_F(ReducerTest, RightClickGestureStarted_EntersPressedAndCaptures) {
    auto effects = Reduce(state, RightClickGestureStartedAction{ 100.0f, 200.0f });

    EXPECT_EQ(state.interaction.gesture.GetPhase(), GesturePhase::Pressed);
    EXPECT_TRUE(HasEffect<effect::SetCapture>(effects));
}

TEST_F(ReducerTest, RightClickGestureMoved_BelowThreshold_StaysPressed) {
    state.interaction.gesture.OnRButtonDown(100.0f, 200.0f);

    auto effects = Reduce(state, RightClickGestureMovedAction{ 105.0f, 205.0f });

    EXPECT_EQ(state.interaction.gesture.GetPhase(), GesturePhase::Pressed);
    // Tracking でないので InvalidateWindow は出ない
    EXPECT_FALSE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, RightClickGestureMoved_AboveThreshold_EntersTracking) {
    state.interaction.gesture.OnRButtonDown(100.0f, 200.0f);

    // 閾値 (30.0f) を超える水平方向移動
    auto effects = Reduce(state, RightClickGestureMovedAction{ 200.0f, 200.0f });

    EXPECT_EQ(state.interaction.gesture.GetPhase(), GesturePhase::Tracking);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, RightClickGestureCompleted_Idle_NoOp) {
    auto effects = Reduce(state, RightClickGestureCompletedAction{ 0, 0 });
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, RightClickGestureCompleted_Pressed_ShowsContextMenu) {
    state.interaction.gesture.OnRButtonDown(100.0f, 200.0f);

    auto effects = Reduce(state, RightClickGestureCompletedAction{ 400, 500 });

    EXPECT_EQ(state.interaction.gesture.GetPhase(), GesturePhase::Idle);
    EXPECT_TRUE(HasEffect<effect::ReleaseCapture>(effects));
    EXPECT_TRUE(HasEffect<effect::ShowContextMenu>(effects));
    bool found = false;
    for (const auto& e : effects) {
        if (auto* p = std::get_if<effect::ShowContextMenu>(&e)) {
            found = true;
            EXPECT_EQ(p->screen_x, 400);
            EXPECT_EQ(p->screen_y, 500);
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(ReducerTest, RightClickGestureCompleted_TrackingLeft_NavigatesBack) {
    // 戻り先履歴を用意
    state.document.doc = Document::FromMarkdown(std::pmr::string("test"), L"C:\\current.md");
    state.view.nav_history.Push(NavEntry{ L"C:\\prev.md", 0.0f });

    state.interaction.gesture.OnRButtonDown(200.0f, 200.0f);
    state.interaction.gesture.OnMouseMove(100.0f, 200.0f); // 左に 100px = 閾値越え

    auto effects = Reduce(state, RightClickGestureCompletedAction{ 0, 0 });

    EXPECT_EQ(state.interaction.gesture.GetPhase(), GesturePhase::Idle);
    EXPECT_TRUE(HasEffect<effect::ReleaseCapture>(effects));
    // ReduceNavigateBack が走るので LoadFile が出る
    EXPECT_TRUE(HasEffect<effect::LoadFile>(effects));
}

TEST_F(ReducerTest, RightClickGestureCompleted_TrackingRight_NavigatesForward) {
    // 前進履歴を用意: page2 からスタート → Push(page1) → GoBack → 前進スタックに page2
    state.view.nav_history.Push(NavEntry{ L"C:\\page1.md", 0.0f });
    NavEntry tmp;
    state.view.nav_history.GoBack(NavEntry{ L"C:\\page2.md", 0.0f }, tmp);
    // 現在ドキュメントを page1（後進結果）に合わせる
    state.document.doc = Document::FromMarkdown(std::pmr::string("test"), L"C:\\page1.md");
    ASSERT_TRUE(state.view.nav_history.CanGoForward());

    state.interaction.gesture.OnRButtonDown(100.0f, 200.0f);
    state.interaction.gesture.OnMouseMove(200.0f, 200.0f); // 右に 100px

    auto effects = Reduce(state, RightClickGestureCompletedAction{ 0, 0 });

    EXPECT_EQ(state.interaction.gesture.GetPhase(), GesturePhase::Idle);
    EXPECT_TRUE(HasEffect<effect::ReleaseCapture>(effects));
    // GoForward で page2.md に進むので LoadFile 発火
    EXPECT_TRUE(HasEffect<effect::LoadFile>(effects));
}

// ---- FilePane* テスト ----

TEST_F(ReducerTest, FilePaneDirectoryClicked_UpdatesDirAndInvalidates) {
    // スクロール位置を設定してリセットされることを確認
    state.view.panes.FileScroll().scroll_y = 100.0f;

    auto effects = Reduce(state, FilePaneDirectoryClickedAction{
        std::pmr::wstring(L"C:\\nonexistent_dir_for_test") });

    EXPECT_EQ(state.view.panes.FileScroll().scroll_y, 0.0f);
    EXPECT_TRUE(HasEffect<effect::InvalidatePaneCache>(effects));
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, FilePaneFileClicked_PushesHistoryAndLoads) {
    state.document.doc = Document::FromMarkdown(std::pmr::string("test"), L"C:\\current.md");

    auto effects = Reduce(state, FilePaneFileClickedAction{
        std::pmr::wstring(L"C:\\clicked.md") });

    EXPECT_TRUE(state.view.nav_history.CanGoBack());
    EXPECT_TRUE(HasEffect<effect::LoadFile>(effects));
}

// ---- TocItemClicked テスト ----

TEST_F(ReducerTest, TocItemClicked_UnknownAnchor_PushesHistoryOnly) {
    state.document.doc = Document::FromMarkdown(std::pmr::string("test"), L"C:\\file.md");

    auto effects = Reduce(state, TocItemClickedAction{
        std::pmr::wstring(L"nonexistent") });

    EXPECT_TRUE(state.view.nav_history.CanGoBack());
    EXPECT_FALSE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, TocItemClicked_ValidAnchor_ScrollsAndInvalidates) {
    state.document.doc = Document::FromMarkdown(std::pmr::string("# First\n\n# Second\n\n# Third"), L"C:\\file.md");

    // layout_cache を同期して y_position を設定
    auto& cache = state.document.layout_cache;
    const auto& nodes = state.document.doc.GetNodes();
    cache.Resize(nodes.size());
    // 2番目の見出しに仮の y 座標を割り当て
    cache[1].y_position = 100.0f;

    const auto anchor = nodes[1].anchor_id();
    if (anchor.empty()) {
        GTEST_SKIP() << "anchor_id が空のため検証できない";
    }

    auto effects = Reduce(state, TocItemClickedAction{ std::pmr::wstring(anchor) });

    EXPECT_TRUE(state.view.nav_history.CanGoBack());
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
    EXPECT_TRUE(HasEffect<effect::BitmapManage>(effects));
}
