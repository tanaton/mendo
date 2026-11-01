#pragma once
#include <d2d1.h>
#include <array>
#include <cassert>
#include <string>
#include <utility>
#include "brush_id.h"
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
    float font_size_body = 0.0f;
    float font_size_h[6]{};
    float font_size_code = 0.0f;
    // 表の見出し行の段。計測の書式と数式のキー (InlineMathKey) の大きさはこれで揃える。
    static constexpr int kTableHeaderLevel = 4;

    // マージンとパディング（DIP単位）
    float margin_left = 0.0f;
    float margin_right = 0.0f;
    float margin_top = 0.0f;
    float margin_bottom = 0.0f;
    float paragraph_spacing = 0.0f;
    float list_item_spacing = 0.0f;
    float heading_spacing_above = 0.0f;
    float heading_spacing_below = 0.0f;      // h3〜h6（下線なし）の下マージン
    float heading_spacing_below_h1h2 = 0.0f; // h1/h2（下線あり）の下マージン — 下線と次行の余白を確保するため大きめ
    float code_block_spacing_above = 0.0f;
    float code_block_padding = 0.0f;
    float indent_width = 0.0f;
    float blockquote_bar_width = 0.0f;
    float list_bullet_offset = 0.0f;
    float list_bullet_radius = 0.0f;
    float hr_thickness = 0.0f;
    float h2_underline_thickness = 0.0f;

    // ペインレイアウト
    float pane_item_height = 0.0f;
    float pane_header_height = 0.0f;
    float splitter_width = 0.0f;
    float pane_font_size = 0.0f;

    // ズーム（1.0 = 100%）
    float zoom = 1.0f;

    // 余白はズームで広がるが本文ペインの最小幅は広がらないので、引くだけだと負になる。
    constexpr float ContentWidth(float viewport_width) const noexcept
    {
        return ShrinkKeepingMinContent(viewport_width, margin_left + margin_right);
    }
    // level は 1〜6。
    constexpr float GetHeadingSize(int level) const noexcept
    {
        assert(level >= 1 && level <= 6);
        return font_size_h[level - 1];
    }
    constexpr float GetHeadingUnderlineThickness(int level) const noexcept
    {
        return (level == 2) ? h2_underline_thickness : hr_thickness;
    }
    // 倍率 1 のテーマ (BaseTheme や GetLightTheme の写し) に倍率を掛ける。誤差を溜めないよう、倍率を変えるたびに
    // ベーステーマから作り直す (ThemeService::ApplyZoom)。
    void ApplyZoom(float new_zoom) noexcept;
};

// プロセス内で 1 度だけ初期化される定数テーマへの参照を返す。
// 値変更が必要な場合は呼び出し側でコピーして ApplyZoom 等を行うこと。
const Theme& GetLightTheme();
const Theme& GetDarkTheme();
