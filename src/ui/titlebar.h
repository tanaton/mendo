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

class TitleBar {
public:
    static constexpr float BASE_HEIGHT = 32.0f;
    static constexpr float BUTTON_WIDTH = 32.0f;
    static constexpr float ICON_LEFT_MARGIN = 8.0f;
    static constexpr float ICON_SIZE = 24.0f;
    static constexpr float ICON_RIGHT_GAP = 4.0f;
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
    constexpr const DipRect& GetOpenFileButton() const noexcept
    {
        return open_file_;
    }
    constexpr const DipRect& GetHelpButton() const noexcept
    {
        return help_;
    }
    constexpr const DipRect& GetThemeToggleButton() const noexcept
    {
        return theme_toggle_;
    }
    constexpr const DipRect& GetSearchButton() const noexcept
    {
        return search_;
    }
    constexpr const DipRect& GetFileToggleButton() const noexcept
    {
        return file_toggle_;
    }
    constexpr const DipRect& GetTocToggleButton() const noexcept
    {
        return toc_toggle_;
    }
    constexpr const DipRect& GetMinimizeButton() const noexcept
    {
        return minimize_;
    }
    constexpr const DipRect& GetMaximizeButton() const noexcept
    {
        return maximize_;
    }
    constexpr const DipRect& GetCloseButton() const noexcept
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
    DipRect open_file_;
    DipRect help_;
    DipRect theme_toggle_;
    DipRect search_;
    DipRect file_toggle_;
    DipRect toc_toggle_;
    DipRect minimize_;
    DipRect maximize_;
    DipRect close_;
    DipRect icon_rect_{};
    DipRect title_text_rect_{};
    TitleBarHitZone hovered_ = TitleBarHitZone::None;
};
