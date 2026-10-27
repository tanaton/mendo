#pragma once
#include <d2d1.h>
#include <array>
#include <string>
#include <utility>
#include "brush_id.h"
#include "document_types.h"
#include "ui_constants.h"

struct Theme {
    // 固定ブラシ (BrushId) ごとの色。半透明の重ね色や固定色も含め、テーマを作るときに全部決める。
    std::array<D2D1_COLOR_F, BRUSH_COUNT> colors{};
    // テーマを作るときに決める (背景の明るさからは推さない)。
    bool dark = false;

    constexpr const D2D1_COLOR_F& Color(BrushId id) const noexcept
    {
        return colors[std::to_underlying(id)];
    }

    // フォント
    std::wstring font_family;
    // 本文の書式に渡すロケール (i18n::TextLocale)。
    const wchar_t* text_locale = L"ja-jp";
    // 解決前は候補の先頭、ThemeService::Init の後は実在する候補。
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

    // ペインレイアウト
    float pane_item_height;
    float pane_header_height;
    float splitter_width;
    float pane_font_size;

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
    void ApplyZoom(float new_zoom) noexcept;
};

// プロセス内で 1 度だけ初期化される定数テーマへの参照を返す。
// 値変更が必要な場合は呼び出し側でコピーして ApplyZoom 等を行うこと。
const Theme& GetLightTheme();
const Theme& GetDarkTheme();
