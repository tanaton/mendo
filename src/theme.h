#pragma once
#include <d2d1.h>
#include <cstdint>
#include <string>

struct Theme {
    // 色
    D2D1_COLOR_F bg_color;
    D2D1_COLOR_F text_color;
    D2D1_COLOR_F heading_color;
    D2D1_COLOR_F code_bg_color;
    D2D1_COLOR_F code_text_color;
    D2D1_COLOR_F link_color;
    D2D1_COLOR_F hr_color;
    D2D1_COLOR_F blockquote_bar_color;
    D2D1_COLOR_F blockquote_text_color;

    // GitHub Alerts: Note, Tip, Important, Warning, Caution（AlertType-1でインデックス）
    D2D1_COLOR_F alert_color[5];     // バー・ラベル色
    D2D1_COLOR_F alert_bg_color[5];  // 背景色

    // シンタックスハイライトの色
    D2D1_COLOR_F syntax_keyword;
    D2D1_COLOR_F syntax_type;
    D2D1_COLOR_F syntax_string;
    D2D1_COLOR_F syntax_number;
    D2D1_COLOR_F syntax_comment;
    D2D1_COLOR_F syntax_preprocessor;
    D2D1_COLOR_F syntax_function;

    // フォント
    std::wstring font_family;
    std::wstring monospace_font;

    // フォントサイズ（DIP単位）
    float font_size_body;
    float font_size_h[6];
    float font_size_code;

    // マージンとパディング（DIP単位）
    float margin_left;
    float margin_right;
    float margin_top;
    float paragraph_spacing;
    float heading_spacing_above;
    float heading_spacing_below;
    float code_block_padding;
    float indent_width;
    float blockquote_bar_width;
    float list_bullet_offset;
    float hr_thickness;

    // ペインレイアウト
    D2D1_COLOR_F pane_bg_color;
    D2D1_COLOR_F splitter_color;
    D2D1_COLOR_F pane_item_hover_color;
    D2D1_COLOR_F pane_item_active_color;
    float pane_item_height;
    float pane_header_height;
    float splitter_width;
    float pane_font_size;

    // ズーム（1.0 = 100%）
    float zoom = 1.0f;

    float GetHeadingSize(int level) const noexcept;
    constexpr bool IsDark() const noexcept { return (bg_color.r + bg_color.g + bg_color.b) < 1.5f; }

    // すべてのスケーラブルなサイズ（フォントサイズ、マージン、スペーシング）にズーム倍率を適用する。
    // `zoom` を変更した後に呼び出して派生値を更新する。
    void ApplyZoom(float new_zoom) noexcept;
};

// Chrome風の段階的ズームステップ
inline constexpr float ZOOM_STEPS[] = {
    0.25f, 0.33f, 0.50f, 0.67f, 0.75f, 0.80f, 0.90f,
    1.00f,
    1.10f, 1.25f, 1.50f, 1.75f, 2.00f, 2.50f, 3.00f, 4.00f, 5.00f
};
inline constexpr int ZOOM_STEP_COUNT = sizeof(ZOOM_STEPS) / sizeof(ZOOM_STEPS[0]);
inline constexpr int ZOOM_DEFAULT_INDEX = 7; // 1.00f

[[nodiscard]] Theme GetLightTheme();
[[nodiscard]] Theme GetDarkTheme();
