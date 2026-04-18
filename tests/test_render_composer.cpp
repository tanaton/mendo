#include <gtest/gtest.h>
#include "render_composer.h"
#include "app_state.h"

// SearchBarController は Init が必要なので BuildSearchBarState はテスト対象外。

TEST(RenderComposer, GestureState_Idle)
{
    AppState state;
    auto gs = render_composer::BuildGestureState(state);
    EXPECT_FALSE(gs.trail_active);
    EXPECT_FALSE(gs.overlay_visible);
    EXPECT_EQ(gs.direction, 0);
    EXPECT_FLOAT_EQ(gs.overlay_alpha, 0.0f);
    ASSERT_NE(gs.trail_points, nullptr);
    EXPECT_TRUE(gs.trail_points->empty());
}

TEST(RenderComposer, GestureState_TrackingLeft)
{
    AppState state;
    state.interaction.gesture.OnRButtonDown(100.0f, 100.0f);
    state.interaction.gesture.OnMouseMove(20.0f, 100.0f); // 左に80px移動

    auto gs = render_composer::BuildGestureState(state);
    EXPECT_TRUE(gs.trail_active);
    EXPECT_TRUE(gs.overlay_visible);
    EXPECT_EQ(gs.direction, -1);
    EXPECT_GT(gs.overlay_alpha, 0.0f);
}

TEST(RenderComposer, GestureState_TrackingRight)
{
    AppState state;
    state.interaction.gesture.OnRButtonDown(100.0f, 100.0f);
    state.interaction.gesture.OnMouseMove(200.0f, 100.0f);

    auto gs = render_composer::BuildGestureState(state);
    EXPECT_EQ(gs.direction, 1);
}

TEST(RenderComposer, GestureState_SwipeFallback_WhenGestureIdle)
{
    AppState state;
    // ジェスチャーは非アクティブのまま、スワイプ検出器のみアクティブにする
    state.interaction.swipe_detector.OnHWheel(500, 1000);

    auto gs = render_composer::BuildGestureState(state);
    EXPECT_FALSE(gs.trail_active);     // gesture 側は非アクティブ
    EXPECT_TRUE(gs.overlay_visible);   // swipe 側のオーバーレイが表示
    EXPECT_EQ(gs.direction, -1);       // 右スワイプ→戻る
    EXPECT_FLOAT_EQ(gs.overlay_alpha, 1.0f);
}

TEST(RenderComposer, GestureState_GestureTakesPrecedenceOverSwipe)
{
    AppState state;
    state.interaction.gesture.OnRButtonDown(100.0f, 100.0f);
    state.interaction.gesture.OnMouseMove(200.0f, 100.0f);
    state.interaction.swipe_detector.OnHWheel(-500, 1000);

    auto gs = render_composer::BuildGestureState(state);
    EXPECT_TRUE(gs.overlay_visible);
    EXPECT_EQ(gs.direction, 1);
}

// ============================================================
// BuildToastState
// ============================================================

TEST(RenderComposer, ToastState_HiddenByDefault)
{
    AppState state;
    auto ts = render_composer::BuildToastState(state);
    EXPECT_FALSE(ts.visible);
    EXPECT_FLOAT_EQ(ts.alpha, 0.0f);
    EXPECT_TRUE(ts.message.empty());
}

TEST(RenderComposer, ToastState_VisibleAfterShow)
{
    AppState state;
    state.interaction.toast.Show(L"Copied");

    auto ts = render_composer::BuildToastState(state);
    EXPECT_TRUE(ts.visible);
    // ホールド中は内部 alpha=2.5 を 1.0 にクランプ
    EXPECT_FLOAT_EQ(ts.alpha, 1.0f);
    EXPECT_EQ(ts.message, L"Copied");
}

TEST(RenderComposer, ToastState_AfterReset)
{
    AppState state;
    state.interaction.toast.Show(L"Hello");
    state.interaction.toast.Reset();

    auto ts = render_composer::BuildToastState(state);
    EXPECT_FALSE(ts.visible);
    EXPECT_TRUE(ts.message.empty());
}

// ============================================================
// BuildTitleBarState
// ============================================================

TEST(RenderComposer, TitleBarState_CopiesFlags)
{
    AppState state;
    state.window.titlebar.UpdateLayout(1024.0f);
    state.cached_title_text = L"example.md";

    auto tb = render_composer::BuildTitleBarState(state, 1024.0f, /*is_dark=*/true, /*is_max=*/false);

    EXPECT_FLOAT_EQ(tb.window_width, 1024.0f);
    EXPECT_TRUE(tb.is_dark_mode);
    EXPECT_FALSE(tb.is_maximized);
    EXPECT_EQ(tb.title_text, std::wstring_view(L"example.md"));
    EXPECT_FLOAT_EQ(tb.height, state.window.titlebar.GetHeight());
}

TEST(RenderComposer, TitleBarState_WindowActiveDefault)
{
    AppState state;
    auto tb = render_composer::BuildTitleBarState(state, 800.0f, false, true);
    EXPECT_TRUE(tb.window_active);
    EXPECT_TRUE(tb.is_maximized);
    EXPECT_FALSE(tb.is_dark_mode);
}

TEST(RenderComposer, TitleBarState_ButtonsComeFromTitleBar)
{
    AppState state;
    state.window.titlebar.UpdateLayout(1024.0f);
    state.window.titlebar.SetHovered(TitleBarHitZone::Close);

    auto tb = render_composer::BuildTitleBarState(state, 1024.0f, false, false);
    EXPECT_TRUE(tb.close.hovered);
    EXPECT_FALSE(tb.minimize.hovered);
    EXPECT_FALSE(tb.maximize.hovered);
}

TEST(RenderComposer, TitleBarState_PaneVisibilityMirrorsState)
{
    AppState state;
    state.view.panes.SetFilePaneVisible(false);
    state.view.panes.SetTocPaneVisible(true);

    auto tb = render_composer::BuildTitleBarState(state, 800.0f, false, false);
    EXPECT_FALSE(tb.file_pane_visible);
    EXPECT_TRUE(tb.toc_pane_visible);
}

// ============================================================
// BuildSidePaneState
// ============================================================

TEST(RenderComposer, SidePaneState_ReferencesDocumentAndPanes)
{
    AppState state;
    state.view.panes.SetFilePaneVisible(true);
    state.view.panes.SetTocPaneVisible(false);
    state.active_toc_index = 3;

    PaneLayout layout{};
    layout.file_rect = { 0.0f, 0.0f, 200.0f, 600.0f };
    layout.toc_rect  = { 200.0f, 0.0f, 200.0f, 600.0f };

    auto sp = render_composer::BuildSidePaneState(state, layout);

    EXPECT_TRUE(sp.show_file_pane);
    EXPECT_FALSE(sp.show_toc_pane);
    EXPECT_EQ(sp.active_toc_index, 3);
    EXPECT_FLOAT_EQ(sp.file_pane_rect.width, 200.0f);
    EXPECT_FLOAT_EQ(sp.toc_pane_rect.x, 200.0f);
    EXPECT_EQ(&sp.file_entries, &state.file_explorer.GetEntries());
    EXPECT_EQ(&sp.nodes, &state.document.doc.GetNodes());
}

TEST(RenderComposer, SidePaneState_HoverAndHeaderFlags)
{
    AppState state;
    state.view.panes.SetHoveredFileIndex(2);
    state.view.panes.SetHoveredTocIndex(5);
    state.view.panes.SetFileCloseHovered(true);
    state.view.panes.SetFileRefreshHovered(true);
    state.view.panes.SetTocCloseHovered(true);

    PaneLayout layout{};
    auto sp = render_composer::BuildSidePaneState(state, layout);
    EXPECT_EQ(sp.hovered_file_index, 2);
    EXPECT_EQ(sp.hovered_toc_index, 5);
    EXPECT_TRUE(sp.file_close_hovered);
    EXPECT_TRUE(sp.file_refresh_hovered);
    EXPECT_TRUE(sp.toc_close_hovered);
}
