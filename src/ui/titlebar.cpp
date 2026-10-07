#include "titlebar.h"
#include <utility>

void TitleBar::UpdateLayout(float window_width_dip) noexcept
{
    const float icon_top = (BASE_HEIGHT - ICON_SIZE) / 2.0f;
    icon_rect_ = DipRect{ ICON_LEFT_MARGIN, icon_top, ICON_LEFT_MARGIN + ICON_SIZE, icon_top + ICON_SIZE };

    float left = ICON_LEFT_MARGIN + ICON_SIZE + ICON_RIGHT_GAP;
    const auto take_left = [&left](float width) noexcept {
        const DipRect r{ left, 0.0f, left + width, BASE_HEIGHT };
        left += width;
        return r;
    };
    float right = window_width_dip;
    const auto take_right = [&right](float width) noexcept {
        const DipRect r{ right - width, 0.0f, right, BASE_HEIGHT };
        right -= width;
        return r;
    };

    open_file_ = take_left(BUTTON_WIDTH);
    search_ = take_left(BUTTON_WIDTH);
    theme_toggle_ = take_left(BUTTON_WIDTH);
    help_ = take_left(BUTTON_WIDTH);

    close_ = take_right(CAPTION_BTN_WIDTH);
    maximize_ = take_right(CAPTION_BTN_WIDTH);
    minimize_ = take_right(CAPTION_BTN_WIDTH);
    toc_toggle_ = take_right(BUTTON_WIDTH);
    file_toggle_ = take_right(BUTTON_WIDTH);

    const float title_right = file_toggle_.left;
    title_text_rect_ = DipRect{ left, 0.0f, (title_right > left) ? title_right : left, BASE_HEIGHT };
}

TitleBarHitZone TitleBar::HitTest(float dip_x, float dip_y) const noexcept
{
    if (dip_y < 0.0f || dip_y >= BASE_HEIGHT) {
        return TitleBarHitZone::None;
    }
    // 狭幅でボタン同士が重なった場合はキャプションボタンを優先する。
    const struct {
        const DipRect& rect;
        TitleBarHitZone zone;
    } buttons[] = {
        { close_, TitleBarHitZone::Close },
        { maximize_, TitleBarHitZone::Maximize },
        { minimize_, TitleBarHitZone::Minimize },
        { open_file_, TitleBarHitZone::OpenFile },
        { search_, TitleBarHitZone::Search },
        { theme_toggle_, TitleBarHitZone::ThemeToggle },
        { help_, TitleBarHitZone::Help },
        { file_toggle_, TitleBarHitZone::FileToggle },
        { toc_toggle_, TitleBarHitZone::TocToggle },
    };
    for (const auto& b : buttons) {
        if (PointInRect(dip_x, dip_y, b.rect)) {
            return b.zone;
        }
    }
    // アイコン領域（クリックしやすいようアイコン右のギャップまで含む）
    if (dip_x < icon_rect_.right + ICON_RIGHT_GAP) {
        return TitleBarHitZone::Icon;
    }
    return TitleBarHitZone::Caption;
}

bool TitleBar::SetHovered(TitleBarHitZone zone) noexcept
{
    return std::exchange(hovered_, zone) != zone;
}
