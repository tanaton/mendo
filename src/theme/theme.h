#pragma once
#include <d2d1.h>
#include <iterator>
#include <string>
#include "document_types.h"
#include "ui_constants.h"

struct Theme {
    // 色
    D2D1_COLOR_F bg_color;
    D2D1_COLOR_F text_color;
    D2D1_COLOR_F heading_color;
    D2D1_COLOR_F code_bg_color;
    D2D1_COLOR_F inline_code_bg_color;
    D2D1_COLOR_F code_text_color;
    D2D1_COLOR_F link_color;
    D2D1_COLOR_F hr_color;
    D2D1_COLOR_F blockquote_bar_color;
    D2D1_COLOR_F blockquote_text_color;

    // GitHub Alerts: AlertColorIndex() でインデックス
    D2D1_COLOR_F alert_color[ALERT_TYPE_COUNT];    // バー・ラベル色
    D2D1_COLOR_F alert_bg_color[ALERT_TYPE_COUNT];

    // シンタックスハイライトの色
    D2D1_COLOR_F syntax_keyword;
    D2D1_COLOR_F syntax_type;
    D2D1_COLOR_F syntax_string;
    D2D1_COLOR_F syntax_number;
    D2D1_COLOR_F syntax_comment;
    D2D1_COLOR_F syntax_preprocessor;
    D2D1_COLOR_F syntax_function;
    D2D1_COLOR_F syntax_inserted;
    D2D1_COLOR_F syntax_deleted;

    // フォント
    std::wstring font_family;
    // 本文の書式に渡すロケール (i18n::TextLocale)。
    const wchar_t* text_locale = L"ja-jp";
    std::wstring monospace_font;
    std::wstring icon_font;

    // フォントサイズ（DIP単位）
    float font_size_body;
    float font_size_h[6];
    float font_size_code;

    // マージンとパディング（DIP単位）
    float margin_left;
    float margin_right;
    float margin_top;
    float margin_bottom;
    float paragraph_spacing;
    float list_item_spacing;
    float heading_spacing_above;
    float heading_spacing_below;      // h3〜h6（下線なし）の下マージン
    float heading_spacing_below_h1h2; // h1/h2（下線あり）の下マージン — 下線と次行の余白を確保するため大きめ
    float code_block_spacing_above;
    float code_block_padding;
    float indent_width;
    float blockquote_bar_width;
    float list_bullet_offset;
    float list_bullet_radius;
    float hr_thickness;
    float h2_underline_thickness;

    // タイトルバー
    D2D1_COLOR_F titlebar_bg_color;
    D2D1_COLOR_F titlebar_text_color;
    D2D1_COLOR_F titlebar_button_hover_color;
    D2D1_COLOR_F titlebar_button_active_color;

    // ペインレイアウト
    D2D1_COLOR_F pane_bg_color;
    D2D1_COLOR_F splitter_color;
    D2D1_COLOR_F pane_item_hover_color;
    D2D1_COLOR_F pane_item_active_color;
    float pane_item_height;
    float pane_header_height;
    float splitter_width;
    float pane_font_size;

    // 検索
    D2D1_COLOR_F search_bar_bg_color;
    D2D1_COLOR_F search_bar_border_color;
    D2D1_COLOR_F search_input_bg_color;
    D2D1_COLOR_F search_input_text_color;
    D2D1_COLOR_F search_highlight_color;         // 全マッチ（黄色系半透明）
    D2D1_COLOR_F search_highlight_current_color; // 現在マッチ（オレンジ系）
    D2D1_COLOR_F search_no_match_bg_color;       // マッチなし時の入力背景

    // ズーム（1.0 = 100%）
    float zoom = 1.0f;

    // 余白はズームで広がるが本文ペインの最小幅は広がらないので、引くだけだと負になる。
    constexpr float ContentWidth(float viewport_width) const noexcept
    {
        return ShrinkKeepingMinContent(viewport_width, margin_left + margin_right);
    }
    float GetHeadingSize(int level) const noexcept;
    constexpr float GetHeadingUnderlineThickness(int level) const noexcept
    {
        return (level == 2) ? h2_underline_thickness : hr_thickness;
    }
    constexpr bool IsDark() const noexcept
    {
        return (bg_color.r + bg_color.g + bg_color.b) < 1.5f;
    }

    void ApplyZoom(float new_zoom) noexcept;
};

// プロセス内で 1 度だけ初期化される定数テーマへの参照を返す。
// 値変更が必要な場合は呼び出し側でコピーして ApplyZoom 等を行うこと。
const Theme& GetLightTheme();
const Theme& GetDarkTheme();
