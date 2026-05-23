#include "titlebar.h"

void TitleBar::UpdateLayout(float window_width_dip) noexcept
{
    const float icon_top = (BASE_HEIGHT - ICON_SIZE) / 2.0f;
    icon_rect_ = DipRect{ ICON_LEFT_MARGIN, icon_top, ICON_LEFT_MARGIN + ICON_SIZE, icon_top + ICON_SIZE };

    float left = ICON_LEFT_MARGIN + ICON_SIZE + ICON_RIGHT_GAP;
    open_file_.rect = DipRect{ left, 0.0f, left + BUTTON_WIDTH, BASE_HEIGHT };
    left += BUTTON_WIDTH;
    search_.rect = DipRect{ left, 0.0f, left + BUTTON_WIDTH, BASE_HEIGHT };
    left += BUTTON_WIDTH;
    theme_toggle_.rect = DipRect{ left, 0.0f, left + BUTTON_WIDTH, BASE_HEIGHT };
    left += BUTTON_WIDTH;
    help_.rect = DipRect{ left, 0.0f, left + BUTTON_WIDTH, BASE_HEIGHT };
    left += BUTTON_WIDTH;

    float right = window_width_dip;
    close_.rect = DipRect{ right - CAPTION_BTN_WIDTH, 0.0f, right, BASE_HEIGHT };
    right -= CAPTION_BTN_WIDTH;
    maximize_.rect = DipRect{ right - CAPTION_BTN_WIDTH, 0.0f, right, BASE_HEIGHT };
    right -= CAPTION_BTN_WIDTH;
    minimize_.rect = DipRect{ right - CAPTION_BTN_WIDTH, 0.0f, right, BASE_HEIGHT };
    right -= CAPTION_BTN_WIDTH;

    toc_toggle_.rect = DipRect{ right - BUTTON_WIDTH, 0.0f, right, BASE_HEIGHT };
    right -= BUTTON_WIDTH;
    file_toggle_.rect = DipRect{ right - BUTTON_WIDTH, 0.0f, right, BASE_HEIGHT };

    const float title_right = file_toggle_.rect.left;
    title_text_rect_ = DipRect{ left, 0.0f, (title_right > left) ? title_right : left, BASE_HEIGHT };
}

TitleBarHitZone TitleBar::HitTest(float dip_x, float dip_y) const noexcept
{
    if (dip_y < 0.0f || dip_y >= BASE_HEIGHT) {
        return TitleBarHitZone::None;
    }
    if (PointInRect(dip_x, dip_y, close_.rect)) {
        return TitleBarHitZone::Close;
    }
    if (PointInRect(dip_x, dip_y, maximize_.rect)) {
        return TitleBarHitZone::Maximize;
    }
    if (PointInRect(dip_x, dip_y, minimize_.rect)) {
        return TitleBarHitZone::Minimize;
    }
    if (PointInRect(dip_x, dip_y, open_file_.rect)) {
        return TitleBarHitZone::OpenFile;
    }
    if (PointInRect(dip_x, dip_y, search_.rect)) {
        return TitleBarHitZone::Search;
    }
    if (PointInRect(dip_x, dip_y, theme_toggle_.rect)) {
        return TitleBarHitZone::ThemeToggle;
    }
    if (PointInRect(dip_x, dip_y, help_.rect)) {
        return TitleBarHitZone::Help;
    }
    if (PointInRect(dip_x, dip_y, file_toggle_.rect)) {
        return TitleBarHitZone::FileToggle;
    }
    if (PointInRect(dip_x, dip_y, toc_toggle_.rect)) {
        return TitleBarHitZone::TocToggle;
    }
    // アイコン領域（クリックしやすいようアイコン右のギャップまで含む）
    if (dip_x < icon_rect_.right + ICON_RIGHT_GAP) {
        return TitleBarHitZone::Icon;
    }
    return TitleBarHitZone::Caption;
}

bool TitleBar::SetHovered(TitleBarHitZone zone) noexcept
{
    if (hovered_ == zone) {
        return false;
    }
    hovered_ = zone;
    return true;
}
