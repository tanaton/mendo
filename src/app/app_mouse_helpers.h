#pragma once
// app_mouse_*.cpp 群の共通ヘルパー（内部ヘッダ）。
#include "app_state_queries.h"
#include "ui_types.h"
#include "pane_layout.h"
#include "ui_constants.h"

namespace mendo::app_mouse {

// button はペイン左上を原点とする矩形。
constexpr bool HitPaneHeaderButton(float dip_x, float dip_y, const PaneRect& rect, float header_height, const D2D1_RECT_F& button)
{
    const float local_x = dip_x - rect.x;
    const float local_y = dip_y - rect.y;
    if (local_y >= header_height) {
        return false;
    }
    return PointInRect(local_x, local_y, button);
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
    if (HitPaneHeaderButton(dip_x, dip_y, rect, header_h, PaneCloseButtonRect(rect.width, header_h))) {
        return PaneHeaderButton::Close;
    }
    if (!buttons.has_file_buttons) {
        return PaneHeaderButton::None;
    }
    if (HitPaneHeaderButton(dip_x, dip_y, rect, header_h, PaneRefreshButtonRect(rect.width, header_h))) {
        return PaneHeaderButton::Refresh;
    }
    if (buttons.reveal_enabled && HitPaneHeaderButton(dip_x, dip_y, rect, header_h, PaneRevealButtonRect(rect.width, header_h))) {
        return PaneHeaderButton::Reveal;
    }
    return PaneHeaderButton::None;
}

} // namespace mendo::app_mouse
