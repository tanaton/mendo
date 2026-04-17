#pragma once
// app_mouse_*.cpp 群の共通ヘルパー（内部ヘッダ）。
#include "i18n.h"
#include "pane_layout.h"
#include "tooltip.h"
#include "titlebar.h"
#include "ui_constants.h"

namespace mendo::app_mouse {

// ペインヘッダー内のボタンがクリックされたか判定する。
template <auto ButtonRectFn>
bool HitPaneHeaderButton(float dip_x, float dip_y, const PaneRect& rect, float header_height)
{
    const float local_x = dip_x - rect.x;
    const float local_y = dip_y - rect.y;
    if (local_y >= header_height) {
        return false;
    }
    return PointInRect(local_x, local_y, ButtonRectFn(rect.width, header_height));
}

// タイトルバーボタンに対応するツールチップを返す。
inline TooltipTarget BuildTitleBarTooltip(TitleBarHitZone zone, bool is_maximized) noexcept
{
    const auto& ls = i18n::S();
    switch (zone) {
    case TitleBarHitZone::OpenFile:    return { TooltipTarget::Zone::TitleBarButton, ls.tooltip_open_file };
    case TitleBarHitZone::Help:        return { TooltipTarget::Zone::TitleBarButton, ls.tooltip_help };
    case TitleBarHitZone::ThemeToggle: return { TooltipTarget::Zone::TitleBarButton, ls.tooltip_theme_toggle };
    case TitleBarHitZone::Search:      return { TooltipTarget::Zone::TitleBarButton, ls.tooltip_search };
    case TitleBarHitZone::FileToggle:  return { TooltipTarget::Zone::TitleBarButton, ls.tooltip_file_pane };
    case TitleBarHitZone::TocToggle:   return { TooltipTarget::Zone::TitleBarButton, ls.tooltip_toc_pane };
    case TitleBarHitZone::Minimize:    return { TooltipTarget::Zone::TitleBarButton, ls.tooltip_minimize };
    case TitleBarHitZone::Maximize:    return { TooltipTarget::Zone::TitleBarButton, is_maximized ? ls.tooltip_restore : ls.tooltip_maximize };
    case TitleBarHitZone::Close:       return { TooltipTarget::Zone::TitleBarButton, ls.tooltip_close };
    default: return {};
    }
}

// サイドペインのヘッダーボタンホバー処理結果。
struct PaneHoverResult {
    int hovered_index = -1;
    TooltipTarget tooltip;
    bool button_changed = false;
    bool any_button_hit = false;
};

// サイドペインの共通ホバー処理。
// ヘッダーボタンのホバー状態を更新し、コンテンツ領域のアイテムヒットテストを行う。
template<typename SetCloseHoveredFn, typename SetRefreshHoveredFn,
    typename HitTestFn, typename BuildTooltipFn>
PaneHoverResult ProcessSidePaneHover(
    float dip_x, float dip_y,
    const PaneRect& rect, float header_h, float item_height,
    bool has_refresh_btn, float scroll_y,
    SetCloseHoveredFn&& set_close_hovered,
    SetRefreshHoveredFn&& set_refresh_hovered,
    HitTestFn&& hit_test_fn,
    BuildTooltipFn&& build_tooltip)
{
    PaneHoverResult result;

    const bool close_hit = HitPaneHeaderButton<PaneCloseButtonRect>(dip_x, dip_y, rect, header_h);
    bool refresh_hit = false;
    if (has_refresh_btn) {
        refresh_hit = HitPaneHeaderButton<PaneRefreshButtonRect>(dip_x, dip_y, rect, header_h);
    }
    result.any_button_hit = close_hit || refresh_hit;

    bool changed = set_close_hovered(close_hit);
    if (has_refresh_btn) {
        changed |= set_refresh_hovered(refresh_hit);
    }
    result.button_changed = changed;

    const float content_top = rect.y + header_h;
    const float local_y = dip_y - content_top + scroll_y;
    result.hovered_index = hit_test_fn(local_y, item_height);

    result.tooltip = build_tooltip(close_hit, refresh_hit, result.hovered_index);

    return result;
}

// サイドペインのヘッダー領域のクリック処理を共通化。
// ヘッダー領域内のクリックを消費した場合 true を返す（ボタン外も含む）。
template<typename ToggleFn, typename RefreshFn>
bool ProcessSidePaneHeaderClick(
    float dip_x, float dip_y,
    const PaneRect& rect, float header_h,
    bool has_refresh_btn,
    ToggleFn&& toggle_fn,
    RefreshFn&& refresh_fn)
{
    if (dip_y - rect.y >= header_h) {
        return false;
    }
    if (HitPaneHeaderButton<PaneCloseButtonRect>(dip_x, dip_y, rect, header_h)) {
        toggle_fn();
        return true;
    }
    if (has_refresh_btn && HitPaneHeaderButton<PaneRefreshButtonRect>(dip_x, dip_y, rect, header_h)) {
        refresh_fn();
        return true;
    }
    return true; // ヘッダー領域内だがボタン外 — 消費済みとして扱う
}

} // namespace mendo::app_mouse
