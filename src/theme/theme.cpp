#include "theme.h"
#include "theme_palette.h"
#include "i18n.h"
#include <cassert>
#include <span>

namespace {
// Theme 内の「ズームでスケールする float メンバ」を一元管理する。
// 新規のスケーラブルフィールドを追加する際はここに追記すれば
// ApplyZoom へのスケール漏れを防げる。font_size_h は配列なので別ループで扱う。
// サイドペイン (ファイル一覧・目次) の寸法は含めない。ズームは本文の可読性を上げる
// ためのもので、一覧まで拡大すると同時に見える項目数が減って操作しにくくなる。
constexpr float Theme::* kScalableThemeFields[]{
    &Theme::font_size_body,
    &Theme::font_size_code,
    &Theme::margin_left,
    &Theme::margin_right,
    &Theme::margin_top,
    &Theme::margin_bottom,
    &Theme::paragraph_spacing,
    &Theme::list_item_spacing,
    &Theme::heading_spacing_above,
    &Theme::heading_spacing_below,
    &Theme::heading_spacing_below_h1h2,
    &Theme::code_block_spacing_above,
    &Theme::code_block_padding,
    &Theme::indent_width,
    &Theme::blockquote_bar_width,
    &Theme::list_bullet_offset,
    &Theme::list_bullet_radius,
    &Theme::hr_thickness,
    &Theme::h2_underline_thickness,
};
} // namespace

float Theme::GetHeadingSize(int level) const noexcept
{
    if (level < 1 || level > 6) {
        return font_size_body;
    }
    return font_size_h[level - 1];
}

void Theme::ApplyZoom(float new_zoom) noexcept
{
    assert(zoom == 1.0f && new_zoom > 0.0f);
    zoom = new_zoom;
    for (float Theme::* field : kScalableThemeFields) {
        this->*field *= new_zoom;
    }
    for (auto& f : font_size_h) {
        f *= new_zoom;
    }
}

namespace {
constexpr D2D1_COLOR_F SELECTION_COLOR = { 0.26f, 0.56f, 0.84f, 0.3f };

void SetColor(Theme& t, BrushId id, D2D1_COLOR_F color) noexcept
{
    t.colors[std::to_underlying(id)] = color;
}

// selection_html (HTML コピー) と共有するコード/罫線/シンタックス色。
void ApplySharedColors(Theme& t, const theme_palette::SharedColors& p)
{
    SetColor(t, BrushId::CodeBg, D2D1::ColorF(p.code_bg));
    SetColor(t, BrushId::CodeText, D2D1::ColorF(p.code_text));
    SetColor(t, BrushId::Hr, D2D1::ColorF(p.rule));
    SetColor(t, BrushId::SyntaxKeyword, D2D1::ColorF(p.syntax_keyword));
    SetColor(t, BrushId::SyntaxType, D2D1::ColorF(p.syntax_type));
    SetColor(t, BrushId::SyntaxString, D2D1::ColorF(p.syntax_string));
    SetColor(t, BrushId::SyntaxNumber, D2D1::ColorF(p.syntax_number));
    SetColor(t, BrushId::SyntaxComment, D2D1::ColorF(p.syntax_comment));
    SetColor(t, BrushId::SyntaxPreprocessor, D2D1::ColorF(p.syntax_preprocessor));
    SetColor(t, BrushId::SyntaxFunction, D2D1::ColorF(p.syntax_function));
    SetColor(t, BrushId::SyntaxInserted, D2D1::ColorF(p.syntax_inserted));
    SetColor(t, BrushId::SyntaxDeleted, D2D1::ColorF(p.syntax_deleted));
}

// 白 (ダーク) か黒 (ライト) を alpha で重ねる色。
D2D1_COLOR_F MonochromeOverlay(bool dark, float alpha) noexcept
{
    return dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha) : D2D1::ColorF(0.0f, 0.0f, 0.0f, alpha);
}

// 両テーマで同じ色と、t.dark から決まる重ね色。
void ApplyCommonColors(Theme& t)
{
    SetColor(t, BrushId::Selection, SELECTION_COLOR);
    SetColor(t, BrushId::TableStripe, MonochromeOverlay(t.dark, t.dark ? TABLE_STRIPE_ALPHA_DARK : TABLE_STRIPE_ALPHA_LIGHT));
    SetColor(t, BrushId::ScrollbarThumb, MonochromeOverlay(t.dark, t.dark ? 0.4f : 0.25f));
    SetColor(t, BrushId::TitleBarCloseRed, D2D1::ColorF(0xE81123));
    SetColor(t, BrushId::OverlayWhite, D2D1::ColorF(D2D1::ColorF::White));
    SetColor(t, BrushId::Overlay, MonochromeOverlay(t.dark, 1.0f));
    SetColor(t, BrushId::GestureTrail, D2D1::ColorF(0.9f, 0.2f, 0.2f, 1.0f));
}

// ライトテーマとダークテーマで共有するフォントとレイアウト定数。
void ApplyCommonLayout(Theme& t)
{
    // 本文は GitHub (Windows) と同じく英数字を Segoe UI で組み、Segoe UI に無い漢字・かな・全角記号は
    // DirectWrite のシステムフォールバックに任せる。フォールバック先は書式のロケール (i18n::TextLocale) で決まり、
    // ja-jp なら Yu Gothic UI、zh-cn なら Microsoft YaHei UI、zh-tw なら Microsoft JhengHei UI、ko-kr なら Malgun Gothic になる。
    t.font_family = L"Segoe UI";
    t.text_locale = i18n::TextLocale();
    t.monospace_font = theme_palette::kMonospaceFontCandidates[0];
    t.icon_font = theme_palette::kIconFontCandidates[0];

    t.font_size_body = 16.0f;
    t.font_size_h[0] = 32.0f;
    t.font_size_h[1] = 26.0f;
    t.font_size_h[2] = 22.0f;
    t.font_size_h[3] = 18.0f;
    t.font_size_h[4] = 16.0f;
    t.font_size_h[5] = 14.0f;
    t.font_size_code = 14.0f;

    t.margin_left = 40.0f;
    t.margin_right = 40.0f;
    t.margin_top = 8.0f;
    t.margin_bottom = 72.0f;
    t.paragraph_spacing = 12.0f;
    t.list_item_spacing = 6.0f;
    t.heading_spacing_above = 12.0f;
    t.heading_spacing_below = 10.0f;
    t.heading_spacing_below_h1h2 = 16.0f;
    t.code_block_spacing_above = 8.0f;
    t.code_block_padding = 12.0f;
    t.indent_width = 24.0f;
    t.blockquote_bar_width = 4.0f;
    t.list_bullet_offset = 20.0f;
    t.list_bullet_radius = 3.0f;
    t.hr_thickness = 1.5f;
    t.h2_underline_thickness = 1.0f;

    t.pane_item_height = 28.0f;
    t.pane_header_height = 32.0f;
    t.splitter_width = 4.0f;
    t.pane_font_size = 13.0f;
}

// テーマごとに違う色。ライトとダークを同じ行に並べて見比べられるようにする。
struct ThemeColor {
    BrushId id;
    D2D1_COLOR_F light;
    D2D1_COLOR_F dark;
};

// D2D1::ColorF は constexpr でないので、初めて使うときに作る。
std::span<const ThemeColor> ThemeColors()
{
    using D2D1::ColorF;
    static const ThemeColor kColors[]{
        { BrushId::Background, ColorF(0xFFFFFF), ColorF(0x1e1e1e) },
        // GitHub のインラインコード背景 (ライトは rgba(129,139,152,0.12)、ダークは rgba(101,108,118,0.2)) を背景色に重ねた色。
        { BrushId::InlineCodeBg, ColorF(0xf0f1f2), ColorF(0x2c2e30) },
        { BrushId::Text, ColorF(0x24292e), ColorF(0xd4d4d4) },
        { BrushId::Heading, ColorF(0x1a1a1a), ColorF(0xe0e0e0) },
        { BrushId::Link, ColorF(0x0366d6), ColorF(0x569cd6) },
        { BrushId::BlockquoteBar, ColorF(0xdfe2e5), ColorF(0x505050) },
        { BrushId::BlockquoteText, ColorF(0x6a737d), ColorF(0x9e9e9e) },

        // GitHub Alerts
        { BrushId::AlertNote, ColorF(0x0969da), ColorF(0x4493f8) },
        { BrushId::AlertTip, ColorF(0x1a7f37), ColorF(0x3fb950) },
        { BrushId::AlertImportant, ColorF(0x8250df), ColorF(0xa371f7) },
        { BrushId::AlertWarning, ColorF(0x9a6700), ColorF(0xd29922) },
        { BrushId::AlertCaution, ColorF(0xcf222e), ColorF(0xf85149) },
        { BrushId::AlertNoteBg, ColorF(0xddf4ff, 0.4f), ColorF(0x0d1d31, 0.5f) },
        { BrushId::AlertTipBg, ColorF(0xdafbe1, 0.4f), ColorF(0x0b2212, 0.5f) },
        { BrushId::AlertImportantBg, ColorF(0xfbefff, 0.4f), ColorF(0x1e0f35, 0.5f) },
        { BrushId::AlertWarningBg, ColorF(0xfff8c5, 0.4f), ColorF(0x2a1e02, 0.5f) },
        { BrushId::AlertCautionBg, ColorF(0xffebe9, 0.4f), ColorF(0x2e0b0d, 0.5f) },

        // タイトルバー
        { BrushId::TitleBarBg, ColorF(0xf0f0f0), ColorF(0x272727) },
        { BrushId::TitleBarText, ColorF(0x333333), ColorF(0xcccccc) },
        { BrushId::TitleBarButtonHover, ColorF(0xe0e0e0), ColorF(0x383838) },
        { BrushId::TitleBarButtonActive, ColorF(0xd0d0d0), ColorF(0x444444) },

        // ペインレイアウト
        { BrushId::PaneBg, ColorF(0xf5f5f5), ColorF(0x252526) },
        { BrushId::Splitter, ColorF(0xe0e0e0), ColorF(0x3c3c3c) },
        { BrushId::PaneItemHover, ColorF(0xe8e8e8), ColorF(0x2a2d2e) },
        { BrushId::PaneItemActive, ColorF(0xd0e0f0), ColorF(0x094771) },

        // 検索。全マッチは黄色系、現在のマッチはオレンジ系の半透明。
        { BrushId::SearchBarBg, ColorF(0xf0f0f0), ColorF(0x2d2d2d) },
        { BrushId::SearchBarBorder, ColorF(0xcccccc), ColorF(0x444444) },
        { BrushId::SearchInputBg, ColorF(0xffffff), ColorF(0x3c3c3c) },
        { BrushId::SearchInputText, ColorF(0x1a1a1a), ColorF(0xd4d4d4) },
        { BrushId::SearchHighlight, ColorF(1.0f, 0.92f, 0.0f, 0.4f), ColorF(0.8f, 0.7f, 0.0f, 0.35f) },
        { BrushId::SearchHighlightCurrent, ColorF(1.0f, 0.55f, 0.0f, 0.6f), ColorF(1.0f, 0.5f, 0.0f, 0.55f) },
        { BrushId::SearchNoMatchBg, ColorF(1.0f, 0.8f, 0.8f, 1.0f), ColorF(0.5f, 0.15f, 0.15f, 1.0f) },

        // ジェスチャー/トーストの背景パネル。ダークは灰色、ライトは黒を alpha で塗る。
        { BrushId::OverlayGestureBg, ColorF(ColorF::Black), ColorF(0.2f, 0.2f, 0.2f) },
    };
    return kColors;
}

Theme BuildTheme(bool dark)
{
    Theme t{};
    t.dark = dark;
    for (const auto& c : ThemeColors()) {
        SetColor(t, c.id, dark ? c.dark : c.light);
    }
    ApplySharedColors(t, dark ? theme_palette::kDark : theme_palette::kLight);
    ApplyCommonColors(t);
    ApplyCommonLayout(t);
    return t;
}
} // namespace

// 初回呼び出し時に 1 回だけ構築する。Magic statics の保護コストは
// 呼び出しごとの atomic ロードのみで、ホットパスでもほぼ無視できる。
const Theme& GetLightTheme()
{
    static const Theme kLight = BuildTheme(false);
    return kLight;
}

const Theme& GetDarkTheme()
{
    static const Theme kDark = BuildTheme(true);
    return kDark;
}
