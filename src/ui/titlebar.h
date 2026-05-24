#pragma once
#include "ui_types.h"

enum class TitleBarHitZone : uint8_t {
    None,
    Caption,
    Icon,
    OpenFile,
    Help,
    ThemeToggle,
    Search,
    FileToggle,
    TocToggle,
    Minimize,
    Maximize,
    Close,
};

struct TitleBarButton {
    DipRect rect{};
};

class TitleBar {
public:
    static constexpr float BASE_HEIGHT = 32.0f;
    static constexpr float BUTTON_WIDTH = 32.0f;
    static constexpr float ICON_LEFT_MARGIN = 8.0f;
    static constexpr float ICON_SIZE = 24.0f;
    static constexpr float ICON_RIGHT_GAP = 4.0f;
    static constexpr float BUTTON_GAP = 2.0f;
    // キャプションボタン（最小化/最大化/閉じる）はタイトルバーの全高を使う
    static constexpr float CAPTION_BTN_WIDTH = 46.0f;

    constexpr float GetHeight() const noexcept
    {
        return BASE_HEIGHT;
    }

    void UpdateLayout(float window_width_dip) noexcept;

    TitleBarHitZone HitTest(float dip_x, float dip_y) const noexcept;

    // 変化した場合 true。
    bool SetHovered(TitleBarHitZone zone) noexcept;

    constexpr TitleBarHitZone GetHovered() const noexcept
    {
        return hovered_;
    }
    constexpr const TitleBarButton& GetOpenFileButton() const noexcept
    {
        return open_file_;
    }
    constexpr const TitleBarButton& GetHelpButton() const noexcept
    {
        return help_;
    }
    constexpr const TitleBarButton& GetThemeToggleButton() const noexcept
    {
        return theme_toggle_;
    }
    constexpr const TitleBarButton& GetSearchButton() const noexcept
    {
        return search_;
    }
    constexpr const TitleBarButton& GetFileToggleButton() const noexcept
    {
        return file_toggle_;
    }
    constexpr const TitleBarButton& GetTocToggleButton() const noexcept
    {
        return toc_toggle_;
    }
    constexpr const TitleBarButton& GetMinimizeButton() const noexcept
    {
        return minimize_;
    }
    constexpr const TitleBarButton& GetMaximizeButton() const noexcept
    {
        return maximize_;
    }
    constexpr const TitleBarButton& GetCloseButton() const noexcept
    {
        return close_;
    }
    constexpr const DipRect& GetIconRect() const noexcept
    {
        return icon_rect_;
    }
    constexpr const DipRect& GetTitleTextRect() const noexcept
    {
        return title_text_rect_;
    }

private:
    TitleBarButton open_file_;
    TitleBarButton help_;
    TitleBarButton theme_toggle_;
    TitleBarButton search_;
    TitleBarButton file_toggle_;
    TitleBarButton toc_toggle_;
    TitleBarButton minimize_;
    TitleBarButton maximize_;
    TitleBarButton close_;
    DipRect icon_rect_{};
    DipRect title_text_rect_{};
    TitleBarHitZone hovered_ = TitleBarHitZone::None;
};
