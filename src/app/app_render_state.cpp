#include "app.h"

GestureRenderState App::BuildGestureRenderState() const
{
    GestureRenderState gs;
    gs.trail_active = gesture_.IsGestureActive();
    gs.trail_points = &gesture_.GetTrailPoints();
    gs.overlay_visible = gesture_.IsOverlayVisible();
    gs.direction = (gesture_.GetDirection() == GestureDirection::Left) ? -1
        : (gesture_.GetDirection() == GestureDirection::Right) ? 1 : 0;
    gs.overlay_alpha = gesture_.GetOverlayAlpha();

    // タッチパッドスワイプのオーバーレイ（マウスジェスチャーが非アクティブの場合のみ）
    if (!gs.overlay_visible && swipe_detector_.IsOverlayVisible()) {
        gs.overlay_visible = true;
        gs.direction = swipe_detector_.GetOverlayDirection();
        gs.overlay_alpha = swipe_detector_.GetOverlayAlpha();
    }
    return gs;
}

SidePaneState App::BuildSidePaneState(const ::PaneLayout& layout) const
{
    return { layout.file_rect, layout.toc_rect,
             file_explorer_.GetEntries(), panes_.FileScroll(),
             doc_.GetToc().GetEntries(), doc_.GetNodes(), panes_.TocScroll(),
             panes_.GetHoveredFileIndex(), panes_.GetHoveredTocIndex(), active_toc_index_,
             panes_.IsFilePaneVisible(), panes_.IsTocPaneVisible(),
             panes_.IsFileCloseHovered(), panes_.IsFileRefreshHovered(),
             panes_.IsTocCloseHovered() };
}

TitleBarRenderState App::BuildTitleBarRenderState(float window_width) const
{
    TitleBarRenderState tb;
    tb.height = titlebar_.GetHeight();
    tb.window_width = window_width;
    tb.open_file = titlebar_.GetOpenFileButton();
    tb.help = titlebar_.GetHelpButton();
    tb.theme_toggle = titlebar_.GetThemeToggleButton();
    tb.search = titlebar_.GetSearchButton();
    tb.file_toggle = titlebar_.GetFileToggleButton();
    tb.toc_toggle = titlebar_.GetTocToggleButton();
    tb.minimize = titlebar_.GetMinimizeButton();
    tb.maximize = titlebar_.GetMaximizeButton();
    tb.close = titlebar_.GetCloseButton();
    tb.icon_rect = titlebar_.GetIconRect();
    tb.title_text_rect = titlebar_.GetTitleTextRect();
    tb.title_text = cached_title_text_;
    tb.is_dark_mode = theme_service_.IsDarkMode();
    tb.search_active = search_state_.IsVisible();
    tb.file_pane_visible = panes_.IsFilePaneVisible();
    tb.toc_pane_visible = panes_.IsTocPaneVisible();
    tb.is_maximized = IsZoomed(hwnd_) != FALSE;
    tb.window_active = window_active_;
    return tb;
}

ToastRenderState App::BuildToastRenderState() const
{
    ToastRenderState ts;
    ts.visible = toast_.IsVisible();
    ts.alpha = toast_.GetRenderAlpha();
    ts.message = toast_.GetMessage();
    return ts;
}

SearchBarRenderState App::BuildSearchBarRenderState() const
{
    return search_bar_ctrl_.BuildRenderState();
}
