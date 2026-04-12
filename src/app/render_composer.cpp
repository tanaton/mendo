#include "render_composer.h"

namespace render_composer {

GestureRenderState BuildGestureState(const AppState& state)
{
    GestureRenderState gs;
    gs.trail_active = state.gesture.IsGestureActive();
    gs.trail_points = &state.gesture.GetTrailPoints();
    gs.overlay_visible = state.gesture.IsOverlayVisible();
    gs.direction = (state.gesture.GetDirection() == GestureDirection::Left) ? -1
        : (state.gesture.GetDirection() == GestureDirection::Right) ? 1 : 0;
    gs.overlay_alpha = state.gesture.GetOverlayAlpha();

    // タッチパッドスワイプのオーバーレイ（マウスジェスチャーが非アクティブの場合のみ）
    if (!gs.overlay_visible && state.swipe_detector.IsOverlayVisible()) {
        gs.overlay_visible = true;
        gs.direction = state.swipe_detector.GetOverlayDirection();
        gs.overlay_alpha = state.swipe_detector.GetOverlayAlpha();
    }
    return gs;
}

SidePaneState BuildSidePaneState(const AppState& state, const PaneLayout& layout)
{
    return { layout.file_rect, layout.toc_rect,
             state.file_explorer.GetEntries(), state.panes.FileScroll(),
             state.doc.GetToc().GetEntries(), state.doc.GetNodes(), state.panes.TocScroll(),
             state.panes.GetHoveredFileIndex(), state.panes.GetHoveredTocIndex(), state.active_toc_index,
             state.panes.IsFilePaneVisible(), state.panes.IsTocPaneVisible(),
             state.panes.IsFileCloseHovered(), state.panes.IsFileRefreshHovered(),
             state.panes.IsTocCloseHovered() };
}

TitleBarRenderState BuildTitleBarState(const AppState& state,
    float window_width, bool is_dark_mode, bool is_maximized)
{
    TitleBarRenderState tb;
    tb.height = state.titlebar.GetHeight();
    tb.window_width = window_width;
    tb.open_file = state.titlebar.GetOpenFileButton();
    tb.help = state.titlebar.GetHelpButton();
    tb.theme_toggle = state.titlebar.GetThemeToggleButton();
    tb.search = state.titlebar.GetSearchButton();
    tb.file_toggle = state.titlebar.GetFileToggleButton();
    tb.toc_toggle = state.titlebar.GetTocToggleButton();
    tb.minimize = state.titlebar.GetMinimizeButton();
    tb.maximize = state.titlebar.GetMaximizeButton();
    tb.close = state.titlebar.GetCloseButton();
    tb.icon_rect = state.titlebar.GetIconRect();
    tb.title_text_rect = state.titlebar.GetTitleTextRect();
    tb.title_text = state.cached_title_text;
    tb.is_dark_mode = is_dark_mode;
    tb.search_active = state.search_state.IsVisible();
    tb.file_pane_visible = state.panes.IsFilePaneVisible();
    tb.toc_pane_visible = state.panes.IsTocPaneVisible();
    tb.is_maximized = is_maximized;
    tb.window_active = state.window_active;
    return tb;
}

ToastRenderState BuildToastState(const AppState& state)
{
    ToastRenderState ts;
    ts.visible = state.toast.IsVisible();
    ts.alpha = state.toast.GetRenderAlpha();
    ts.message = state.toast.GetMessage();
    return ts;
}

SearchBarRenderState BuildSearchBarState(const AppState& state)
{
    return state.search_bar_ctrl.BuildRenderState();
}

} // namespace render_composer
