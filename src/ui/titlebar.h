#pragma once
#include "ui_types.h"
#include <array>
#include <cassert>
#include <utility>

// OpenFile..Close はボタン。連続した値であることを TitleBarButtonIndex が前提にする。
enum class TitleBarHitZone : uint8_t {
    None,
    Caption,
    Icon,
    OpenFile,
    Settings,
    Help,
    ThemeToggle,
    Search,
    FileToggle,
    TocToggle,
    Minimize,
    Maximize,
    Close,
};

constexpr bool IsTitleBarButton(TitleBarHitZone zone) noexcept
{
    return zone >= TitleBarHitZone::OpenFile && zone <= TitleBarHitZone::Close;
}

inline constexpr size_t kTitleBarButtonCount =
    std::to_underlying(TitleBarHitZone::Close) - std::to_underlying(TitleBarHitZone::OpenFile) + 1;

constexpr size_t TitleBarButtonIndex(TitleBarHitZone zone) noexcept
{
    assert(IsTitleBarButton(zone));
    return std::to_underlying(zone) - std::to_underlying(TitleBarHitZone::OpenFile);
}

// TitleBarButtonIndex で引くボタン矩形。
using TitleBarButtons = std::array<DipRect, kTitleBarButtonCount>;

constexpr auto& TitleBarButtonRect(auto& buttons, TitleBarHitZone zone) noexcept
{
    return buttons[TitleBarButtonIndex(zone)];
}

class TitleBar {
public:
    static constexpr float BASE_HEIGHT = 32.0f;
    static constexpr float BUTTON_WIDTH = 32.0f;
    static constexpr float ICON_LEFT_MARGIN = 8.0f;
    static constexpr float ICON_SIZE = 24.0f;
    static constexpr float ICON_RIGHT_GAP = 4.0f;
    // キャプションボタン（最小化/最大化/閉じる）はタイトルバーの全高を使う
    static constexpr float CAPTION_BTN_WIDTH = 46.0f;
    // アイコンの位置は幅に依存しない。
    static constexpr DipRect ICON_RECT{ ICON_LEFT_MARGIN, (BASE_HEIGHT - ICON_SIZE) / 2.0f, ICON_LEFT_MARGIN + ICON_SIZE,
                                        (BASE_HEIGHT + ICON_SIZE) / 2.0f };

    void UpdateLayout(float window_width_dip) noexcept;

    TitleBarHitZone HitTest(float dip_x, float dip_y) const noexcept;

    constexpr const DipRect& GetButton(TitleBarHitZone zone) const noexcept
    {
        return TitleBarButtonRect(buttons_, zone);
    }
    constexpr const DipRect& GetTitleTextRect() const noexcept
    {
        return title_text_rect_;
    }

private:
    constexpr DipRect& Button(TitleBarHitZone zone) noexcept
    {
        return TitleBarButtonRect(buttons_, zone);
    }

    TitleBarButtons buttons_{};
    DipRect title_text_rect_{};
};
