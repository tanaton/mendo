#pragma once
// app_mouse_*.cpp 群の共通ヘルパー（内部ヘッダ）。
#include "app_state_queries.h"
#include "i18n.h"
#include "ui_types.h"
#include "pane_layout.h"
#include "tooltip.h"
#include "titlebar.h"
#include "ui_constants.h"
#include <concepts>
#include <string_view>
#include <type_traits>
#include <utility>

namespace mendo::app_mouse {

template <auto Fn>
concept PaneButtonRectFn =
    std::invocable<decltype(Fn), float, float> && std::convertible_to<std::invoke_result_t<decltype(Fn), float, float>, D2D1_RECT_F>;

template <auto ButtonRectFn>
    requires PaneButtonRectFn<ButtonRectFn>
constexpr bool HitPaneHeaderButton(float dip_x, float dip_y, const PaneRect& rect, float header_height)
{
    const float local_x = dip_x - rect.x;
    const float local_y = dip_y - rect.y;
    if (local_y >= header_height) {
        return false;
    }
    return PointInRect(local_x, local_y, ButtonRectFn(rect.width, header_height));
}

// サイドペインヘッダーのボタン構成。Refresh/Reveal はファイルペインのみ。
struct SidePaneHeaderButtons {
    bool has_file_buttons = false;
    // 無効時は描画のみ行い、ヒットしない。
    bool reveal_enabled = false;
};

inline SidePaneHeaderButtons SidePaneHeaderButtonsFor(const AppState& state, PaneTarget target) noexcept
{
    const bool is_file = target == PaneTarget::File;
    return { .has_file_buttons = is_file, .reveal_enabled = is_file && state.document.doc.HasBackingFile() };
}

constexpr PaneHeaderButton HitSidePaneHeaderButton(float dip_x, float dip_y, const PaneRect& rect, float header_h, SidePaneHeaderButtons buttons)
{
    if (HitPaneHeaderButton<PaneCloseButtonRect>(dip_x, dip_y, rect, header_h)) {
        return PaneHeaderButton::Close;
    }
    if (!buttons.has_file_buttons) {
        return PaneHeaderButton::None;
    }
    if (HitPaneHeaderButton<PaneRefreshButtonRect>(dip_x, dip_y, rect, header_h)) {
        return PaneHeaderButton::Refresh;
    }
    if (buttons.reveal_enabled && HitPaneHeaderButton<PaneRevealButtonRect>(dip_x, dip_y, rect, header_h)) {
        return PaneHeaderButton::Reveal;
    }
    return PaneHeaderButton::None;
}

// text が空 (該当ボタンなし) なら空のターゲットを返す。
inline TooltipTarget MakeButtonTooltip(const TooltipTarget& current, TooltipTarget::Zone zone, uint64_t key, std::wstring_view text)
{
    return text.empty() ? TooltipTarget{} : MakeTooltip(current, zone, key, text);
}

inline TooltipTarget BuildTitleBarTooltip(const TooltipTarget& current, TitleBarHitZone zone, bool is_maximized)
{
    const auto& ls = i18n::S();
    const std::wstring_view text = [&]() -> std::wstring_view {
        switch (zone) {
        case TitleBarHitZone::OpenFile:
            return ls.tooltip_open_file;
        case TitleBarHitZone::Help:
            return ls.tooltip_help;
        case TitleBarHitZone::ThemeToggle:
            return ls.tooltip_theme_toggle;
        case TitleBarHitZone::Search:
            return ls.tooltip_search;
        case TitleBarHitZone::FileToggle:
            return ls.tooltip_file_pane;
        case TitleBarHitZone::TocToggle:
            return ls.tooltip_toc_pane;
        case TitleBarHitZone::Minimize:
            return ls.tooltip_minimize;
        case TitleBarHitZone::Maximize:
            return is_maximized ? ls.tooltip_restore : ls.tooltip_maximize;
        case TitleBarHitZone::Close:
            return ls.tooltip_close;
        default:
            return {};
        }
    }();
    // 最大化ボタンは状態で文言が変わるため key を分ける。
    const uint64_t key = std::to_underlying(zone) * 2ull + (zone == TitleBarHitZone::Maximize && is_maximized);
    return MakeButtonTooltip(current, TooltipTarget::Zone::TitleBarButton, key, text);
}

inline TooltipTarget BuildSearchBarTooltip(const TooltipTarget& current, SearchBarHitZone zone)
{
    const auto& ls = i18n::S();
    const std::wstring_view text = [&]() -> std::wstring_view {
        switch (zone) {
        case SearchBarHitZone::Up:
            return ls.tooltip_search_prev;
        case SearchBarHitZone::Down:
            return ls.tooltip_search_next;
        case SearchBarHitZone::CaseSensitive:
            return ls.tooltip_search_case;
        case SearchBarHitZone::Highlight:
            return ls.tooltip_search_highlight;
        case SearchBarHitZone::Close:
            return ls.tooltip_search_close;
        default:
            return {};
        }
    }();
    return MakeButtonTooltip(current, TooltipTarget::Zone::SearchBarButton, std::to_underlying(zone), text);
}

inline TooltipTarget BuildNavButtonTooltip(const TooltipTarget& current, NavButtonHover hit)
{
    const auto& ls = i18n::S();
    const std::wstring_view text = [&]() -> std::wstring_view {
        switch (hit) {
        case NavButtonHover::Back:
            return ls.tooltip_nav_back;
        case NavButtonHover::Forward:
            return ls.tooltip_nav_forward;
        default:
            return {};
        }
    }();
    return MakeButtonTooltip(current, TooltipTarget::Zone::NavButton, std::to_underlying(hit), text);
}

struct PaneHoverResult {
    int hovered_index = -1;
    TooltipTarget tooltip;
    bool button_changed = false;
    bool any_button_hit = false;
};

template <typename SetHoveredFn, typename HitTestFn, typename BuildTooltipFn>
    requires std::predicate<SetHoveredFn&, PaneHeaderButton> && std::invocable<HitTestFn&, float, float> && std::convertible_to<std::invoke_result_t<HitTestFn&, float, float>, int> && std::invocable<BuildTooltipFn&, PaneHeaderButton, int> && std::convertible_to<std::invoke_result_t<BuildTooltipFn&, PaneHeaderButton, int>, TooltipTarget>
PaneHoverResult ProcessSidePaneHover(
    float dip_x, float dip_y,
    const PaneRect& rect, float header_h, float item_height,
    SidePaneHeaderButtons buttons, float scroll_y,
    SetHoveredFn&& set_hovered,
    HitTestFn&& hit_test_fn,
    BuildTooltipFn&& build_tooltip)
{
    PaneHoverResult result;

    const PaneHeaderButton hit = HitSidePaneHeaderButton(dip_x, dip_y, rect, header_h, buttons);
    result.any_button_hit = hit != PaneHeaderButton::None;
    result.button_changed = set_hovered(hit);

    result.hovered_index = hit_test_fn(SidePaneLocalY(dip_y, rect.y + header_h, scroll_y), item_height);

    result.tooltip = build_tooltip(hit, result.hovered_index);

    return result;
}

// ヘッダー領域内のクリックは (ボタン外含め) 全て消費して true を返す。
template <typename OnButtonFn>
    requires std::invocable<OnButtonFn&, PaneHeaderButton>
bool ProcessSidePaneHeaderClick(
    float dip_x, float dip_y,
    const PaneRect& rect, float header_h,
    SidePaneHeaderButtons buttons,
    OnButtonFn&& on_button)
{
    if (dip_y - rect.y >= header_h) {
        return false;
    }
    const PaneHeaderButton hit = HitSidePaneHeaderButton(dip_x, dip_y, rect, header_h, buttons);
    if (hit != PaneHeaderButton::None) {
        on_button(hit);
    }
    return true; // ボタン外でもヘッダー領域内は消費済みとして扱う
}

} // namespace mendo::app_mouse
