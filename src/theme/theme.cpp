#include "theme.h"
#include "theme_palette.h"
#include "i18n.h"
#include <algorithm>
#include <array>
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
// ライトテーマとダークテーマで共有するフォントとレイアウト定数。
void ApplyCommonLayout(Theme& t)
{
    // 本文は GitHub (Windows) と同じく英数字を Segoe UI で組み、Segoe UI に無い文字のフォントは
    // DirectWrite のシステムフォールバックが決める。フォールバック先は書式のロケール (i18n::TextLocale) で変わり、
    // 漢字は ja-jp なら Yu Gothic UI、zh-cn なら Microsoft YaHei UI、zh-tw なら Microsoft JhengHei UI、ko-kr なら Malgun Gothic になる。
    // 計測を速くするために名前を明示する文字も、名前はシステムに尋ねて決める (FallbackFontMap)。
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

// テーマごとの色。ライトとダークを同じ行に並べて見比べられるようにする。
struct ThemeColor {
    BrushId id;
    D2D1_COLOR_F light;
    D2D1_COLOR_F dark;
};

// D2D1::ColorF(rgb, a) と同じ値。ColorF は constexpr でなく、表を static_assert で確かめられないので自前で作る。
constexpr D2D1_COLOR_F Rgb(uint32_t rgb, float a = 1.0f) noexcept
{
    return { static_cast<float>((rgb >> 16) & 0xFF) / 255.f, static_cast<float>((rgb >> 8) & 0xFF) / 255.f, static_cast<float>(rgb & 0xFF) / 255.f, a };
}

// selection_html (HTML コピー) と共有するコード/罫線/シンタックス色。
constexpr ThemeColor Shared(BrushId id, uint32_t theme_palette::SharedColors::* color) noexcept
{
    return { id, Rgb(theme_palette::kLight.*color), Rgb(theme_palette::kDark.*color) };
}

constexpr ThemeColor Same(BrushId id, D2D1_COLOR_F color) noexcept
{
    return { id, color, color };
}

// 黒 (ライト) か白 (ダーク) を alpha で重ねる色。
constexpr ThemeColor Monochrome(BrushId id, float light_alpha, float dark_alpha) noexcept
{
    return {
        id, { 0.0f, 0.0f, 0.0f, light_alpha },
         { 1.0f, 1.0f, 1.0f, dark_alpha  }
    };
}

using theme_palette::SharedColors;
constexpr ThemeColor kThemeColors[]{
    { BrushId::Background, Rgb(0xFFFFFF), Rgb(0x1e1e1e) },
    // GitHub のインラインコード背景 (ライトは rgba(129,139,152,0.12)、ダークは rgba(101,108,118,0.2)) を背景色に重ねた色。
    { BrushId::InlineCodeBg, Rgb(0xf0f1f2), Rgb(0x2c2e30) },
    { BrushId::Text, Rgb(0x24292e), Rgb(0xd4d4d4) },
    { BrushId::Heading, Rgb(0x1a1a1a), Rgb(0xe0e0e0) },
    { BrushId::Link, Rgb(0x0366d6), Rgb(0x569cd6) },
    { BrushId::BlockquoteBar, Rgb(0xdfe2e5), Rgb(0x505050) },
    // 引用と h6 の薄い文字色。ライトは GitHub の fgColor-muted。ダークは背景が GitHub (#0d1117) と違うので、
    // GitHub の #9198a1 ではなく本文と同じ無彩色の灰色にしている。
    { BrushId::MutedText, Rgb(0x59636e), Rgb(0x9e9e9e) },

    Shared(BrushId::CodeBg, &SharedColors::code_bg),
    Shared(BrushId::CodeText, &SharedColors::code_text),
    Shared(BrushId::Hr, &SharedColors::rule),
    Shared(BrushId::SyntaxKeyword, &SharedColors::syntax_keyword),
    Shared(BrushId::SyntaxType, &SharedColors::syntax_type),
    Shared(BrushId::SyntaxString, &SharedColors::syntax_string),
    Shared(BrushId::SyntaxNumber, &SharedColors::syntax_number),
    Shared(BrushId::SyntaxComment, &SharedColors::syntax_comment),
    Shared(BrushId::SyntaxPreprocessor, &SharedColors::syntax_preprocessor),
    Shared(BrushId::SyntaxFunction, &SharedColors::syntax_function),
    Shared(BrushId::SyntaxInserted, &SharedColors::syntax_inserted),
    Shared(BrushId::SyntaxDeleted, &SharedColors::syntax_deleted),

    // GitHub Alerts
    { BrushId::AlertNote, Rgb(0x0969da), Rgb(0x4493f8) },
    { BrushId::AlertTip, Rgb(0x1a7f37), Rgb(0x3fb950) },
    { BrushId::AlertImportant, Rgb(0x8250df), Rgb(0xa371f7) },
    { BrushId::AlertWarning, Rgb(0x9a6700), Rgb(0xd29922) },
    { BrushId::AlertCaution, Rgb(0xcf222e), Rgb(0xf85149) },
    { BrushId::AlertNoteBg, Rgb(0xddf4ff, 0.4f), Rgb(0x0d1d31, 0.5f) },
    { BrushId::AlertTipBg, Rgb(0xdafbe1, 0.4f), Rgb(0x0b2212, 0.5f) },
    { BrushId::AlertImportantBg, Rgb(0xfbefff, 0.4f), Rgb(0x1e0f35, 0.5f) },
    { BrushId::AlertWarningBg, Rgb(0xfff8c5, 0.4f), Rgb(0x2a1e02, 0.5f) },
    { BrushId::AlertCautionBg, Rgb(0xffebe9, 0.4f), Rgb(0x2e0b0d, 0.5f) },

    // タイトルバー
    { BrushId::TitleBarBg, Rgb(0xf0f0f0), Rgb(0x272727) },
    { BrushId::TitleBarText, Rgb(0x333333), Rgb(0xcccccc) },
    { BrushId::TitleBarButtonHover, Rgb(0xe0e0e0), Rgb(0x383838) },
    { BrushId::TitleBarButtonActive, Rgb(0xd0d0d0), Rgb(0x444444) },
    Same(BrushId::TitleBarCloseRed, Rgb(0xE81123)),

    // ペインレイアウト
    { BrushId::PaneBg, Rgb(0xf5f5f5), Rgb(0x252526) },
    { BrushId::Splitter, Rgb(0xe0e0e0), Rgb(0x3c3c3c) },
    { BrushId::PaneItemHover, Rgb(0xe8e8e8), Rgb(0x2a2d2e) },
    { BrushId::PaneItemActive, Rgb(0xd0e0f0), Rgb(0x094771) },

    // 検索。全マッチは黄色系、現在のマッチはオレンジ系の半透明。
    { BrushId::SearchBarBg, Rgb(0xf0f0f0), Rgb(0x2d2d2d) },
    { BrushId::SearchBarBorder, Rgb(0xcccccc), Rgb(0x444444) },
    { BrushId::SearchInputBg, Rgb(0xffffff), Rgb(0x3c3c3c) },
    { BrushId::SearchInputText, Rgb(0x1a1a1a), Rgb(0xd4d4d4) },
    { BrushId::SearchHighlight, { 1.0f, 0.92f, 0.0f, 0.4f }, { 0.8f, 0.7f, 0.0f, 0.35f } },
    { BrushId::SearchHighlightCurrent, { 1.0f, 0.55f, 0.0f, 0.6f }, { 1.0f, 0.5f, 0.0f, 0.55f } },
    { BrushId::SearchNoMatchBg, { 1.0f, 0.8f, 0.8f, 1.0f }, { 0.5f, 0.15f, 0.15f, 1.0f } },

    // ジェスチャー/トーストの背景パネル。ダークは灰色、ライトは黒を alpha で塗る。
    { BrushId::OverlayGestureBg, Rgb(0x000000), { 0.2f, 0.2f, 0.2f, 1.0f } },
    Same(BrushId::OverlayWhite, Rgb(0xFFFFFF)),
    Monochrome(BrushId::Overlay, 1.0f, 1.0f),
    Same(
        BrushId::GestureTrail, { 0.9f, 0.2f, 0.2f, 1.0f }
    ),

    Same(
        BrushId::Selection, { 0.26f, 0.56f, 0.84f, 0.3f }
    ),
    Monochrome(BrushId::TableStripe, 0.02f, 0.05f),
    Monochrome(BrushId::ScrollbarThumb, 0.25f, 0.4f),
};

constexpr bool CoversEveryBrushOnce(std::span<const ThemeColor> colors) noexcept
{
    std::array<int, BRUSH_COUNT> count{};
    for (const auto& c : colors) {
        ++count.at(std::to_underlying(c.id));
    }
    return std::ranges::all_of(count, [](int n) static noexcept { return n == 1; });
}
static_assert(CoversEveryBrushOnce(kThemeColors), "kThemeColors must set every BrushId exactly once");

Theme BuildTheme(bool dark)
{
    Theme t{};
    t.dark = dark;
    for (const auto& c : kThemeColors) {
        t.colors[std::to_underlying(c.id)] = dark ? c.dark : c.light;
    }
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
