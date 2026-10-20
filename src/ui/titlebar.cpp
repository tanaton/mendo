#include "titlebar.h"
#include <algorithm>
#include <iterator>

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

    Button(TitleBarHitZone::OpenFile) = take_left(BUTTON_WIDTH);
    Button(TitleBarHitZone::Search) = take_left(BUTTON_WIDTH);
    Button(TitleBarHitZone::ThemeToggle) = take_left(BUTTON_WIDTH);
    Button(TitleBarHitZone::Help) = take_left(BUTTON_WIDTH);

    Button(TitleBarHitZone::Close) = take_right(CAPTION_BTN_WIDTH);
    Button(TitleBarHitZone::Maximize) = take_right(CAPTION_BTN_WIDTH);
    Button(TitleBarHitZone::Minimize) = take_right(CAPTION_BTN_WIDTH);
    Button(TitleBarHitZone::TocToggle) = take_right(BUTTON_WIDTH);
    Button(TitleBarHitZone::FileToggle) = take_right(BUTTON_WIDTH);

    const float title_right = GetButton(TitleBarHitZone::FileToggle).left;
    title_text_rect_ = DipRect{ left, 0.0f, std::max(left, title_right), BASE_HEIGHT };
}

TitleBarHitZone TitleBar::HitTest(float dip_x, float dip_y) const noexcept
{
    if (dip_y < 0.0f || dip_y >= BASE_HEIGHT) {
        return TitleBarHitZone::None;
    }
    // 狭幅でボタン同士が重なった場合はキャプションボタンを優先する。
    static constexpr TitleBarHitZone kHitOrder[] = {
        TitleBarHitZone::Close,
        TitleBarHitZone::Maximize,
        TitleBarHitZone::Minimize,
        TitleBarHitZone::OpenFile,
        TitleBarHitZone::Search,
        TitleBarHitZone::ThemeToggle,
        TitleBarHitZone::Help,
        TitleBarHitZone::FileToggle,
        TitleBarHitZone::TocToggle,
    };
    static_assert(std::size(kHitOrder) == kTitleBarButtonCount);
    for (const auto zone : kHitOrder) {
        if (PointInRect(dip_x, dip_y, GetButton(zone))) {
            return zone;
        }
    }
    // アイコン領域（クリックしやすいようアイコン右のギャップまで含む）
    if (dip_x < icon_rect_.right + ICON_RIGHT_GAP) {
        return TitleBarHitZone::Icon;
    }
    return TitleBarHitZone::Caption;
}

