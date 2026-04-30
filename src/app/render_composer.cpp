#include "render_composer.h"

namespace render_composer {

GestureRenderState BuildGestureState(const AppState& state)
{
    GestureRenderState gs;
    gs.trail_active = state.interaction.gesture.IsGestureActive();
    gs.trail_points = &state.interaction.gesture.GetTrailPoints();
    gs.overlay_visible = state.interaction.gesture.IsOverlayVisible();
    gs.direction = (state.interaction.gesture.GetDirection() == GestureDirection::Left)    ? -1
                   : (state.interaction.gesture.GetDirection() == GestureDirection::Right) ? 1
                                                                                           : 0;
    gs.overlay_alpha = state.interaction.gesture.GetOverlayAlpha();

    // タッチパッドスワイプのオーバーレイ（マウスジェスチャーが非アクティブの場合のみ）
    if (!gs.overlay_visible && state.interaction.swipe_detector.IsOverlayVisible()) {
        gs.overlay_visible = true;
        gs.direction = state.interaction.swipe_detector.GetOverlayDirection();
        gs.overlay_alpha = state.interaction.swipe_detector.GetOverlayAlpha();
    }
    return gs;
}

SidePaneState BuildSidePaneState(const AppState& state, const PaneLayout& layout)
{
    return { layout.file_rect, layout.toc_rect,
             state.file_explorer.GetEntries(), state.view.panes.FileScroll(),
             state.document.doc.GetToc().GetEntries(), state.document.doc.GetNodes(), state.view.panes.TocScroll(),
             state.view.panes.GetHoveredFileIndex(), state.view.panes.GetHoveredTocIndex(), state.active_toc_index,
             state.view.panes.IsFilePaneVisible(), state.view.panes.IsTocPaneVisible(),
             state.view.panes.IsFileCloseHovered(), state.view.panes.IsFileRefreshHovered(),
             state.view.panes.IsTocCloseHovered() };
}

TitleBarRenderState BuildTitleBarState(const AppState& state,
                                       float window_width, bool is_dark_mode, bool is_maximized)
{
    TitleBarRenderState tb;
    tb.height = state.window.titlebar.GetHeight();
    tb.window_width = window_width;
    tb.open_file = state.window.titlebar.GetOpenFileButton();
    tb.help = state.window.titlebar.GetHelpButton();
    tb.theme_toggle = state.window.titlebar.GetThemeToggleButton();
    tb.search = state.window.titlebar.GetSearchButton();
    tb.file_toggle = state.window.titlebar.GetFileToggleButton();
    tb.toc_toggle = state.window.titlebar.GetTocToggleButton();
    tb.minimize = state.window.titlebar.GetMinimizeButton();
    tb.maximize = state.window.titlebar.GetMaximizeButton();
    tb.close = state.window.titlebar.GetCloseButton();
    tb.icon_rect = state.window.titlebar.GetIconRect();
    tb.title_text_rect = state.window.titlebar.GetTitleTextRect();
    tb.title_text = state.cached_title_text;
    tb.hovered_zone = state.window.titlebar.GetHovered();
    tb.is_dark_mode = is_dark_mode;
    tb.search_active = state.search.search_state.IsVisible();
    tb.file_pane_visible = state.view.panes.IsFilePaneVisible();
    tb.toc_pane_visible = state.view.panes.IsTocPaneVisible();
    tb.is_maximized = is_maximized;
    tb.window_active = state.window.window_active;
    return tb;
}

ToastRenderState BuildToastState(const AppState& state)
{
    ToastRenderState ts;
    ts.visible = state.interaction.toast.IsVisible();
    ts.alpha = state.interaction.toast.GetRenderAlpha();
    ts.message = state.interaction.toast.GetMessage();
    return ts;
}

SearchBarRenderState BuildSearchBarState(const AppState& state)
{
    return state.search.search_bar_ctrl.BuildRenderState();
}

} // namespace render_composer
