#include <gtest/gtest.h>
#include "theme.h"

namespace {

struct ThemeCase {
    const char* name;
    const Theme& (*get)();
};

constexpr ThemeCase kBothThemes[] = {
    { "light", GetLightTheme },
    { "dark", GetDarkTheme },
};

// font_size_h は配列なので別に扱う。
struct LayoutField {
    const char* name;
    float Theme::* member;
};
// 本文のズームでスケールする寸法。
constexpr LayoutField kScalableFields[] = {
    { "font_size_body", &Theme::font_size_body },
    { "font_size_code", &Theme::font_size_code },
    { "margin_left", &Theme::margin_left },
    { "margin_right", &Theme::margin_right },
    { "margin_top", &Theme::margin_top },
    { "margin_bottom", &Theme::margin_bottom },
    { "paragraph_spacing", &Theme::paragraph_spacing },
    { "list_item_spacing", &Theme::list_item_spacing },
    { "heading_spacing_above", &Theme::heading_spacing_above },
    { "heading_spacing_below", &Theme::heading_spacing_below },
    { "heading_spacing_below_h1h2", &Theme::heading_spacing_below_h1h2 },
    { "code_block_spacing_above", &Theme::code_block_spacing_above },
    { "code_block_padding", &Theme::code_block_padding },
    { "indent_width", &Theme::indent_width },
    { "blockquote_bar_width", &Theme::blockquote_bar_width },
    { "list_bullet_offset", &Theme::list_bullet_offset },
    { "list_bullet_radius", &Theme::list_bullet_radius },
    { "hr_thickness", &Theme::hr_thickness },
    { "h2_underline_thickness", &Theme::h2_underline_thickness },
};
// サイドペインの寸法。本文のズームでは変えない。
constexpr LayoutField kPaneFields[] = {
    { "pane_item_height", &Theme::pane_item_height },
    { "pane_header_height", &Theme::pane_header_height },
    { "splitter_width", &Theme::splitter_width },
    { "pane_font_size", &Theme::pane_font_size },
};

} // namespace

TEST(Theme, GetHeadingSize)
{
    const Theme& t = GetLightTheme();
    for (int level = 1; level <= 6; ++level) {
        EXPECT_EQ(t.GetHeadingSize(level), t.font_size_h[level - 1]) << "level " << level;
    }
}

// テーマを作るときに全ブラシの色を決める (未設定の色は透明の黒のまま残る)。
TEST(Theme, EveryBrushHasColor)
{
    for (const auto& [name, get] : kBothThemes) {
        SCOPED_TRACE(name);
        const Theme& t = get();
        for (size_t i = 0; i < BRUSH_COUNT; ++i) {
            EXPECT_GT(t.colors[i].a, 0.0f) << "BrushId " << i;
        }
    }
}

// ---- ApplyZoom ----

TEST(Theme, ApplyZoomScalesLayoutValues)
{
    const Theme& base = GetLightTheme();
    for (const float zoom : { 1.25f, 1.5f, 2.0f }) {
        SCOPED_TRACE(zoom);
        Theme t = base;
        t.ApplyZoom(zoom);
        EXPECT_FLOAT_EQ(t.zoom, zoom);
        for (const auto& f : kScalableFields) {
            EXPECT_NEAR(t.*f.member, base.*f.member * zoom, 0.01f) << f.name;
        }
        for (int i = 0; i < 6; ++i) {
            EXPECT_NEAR(t.font_size_h[i], base.font_size_h[i] * zoom, 0.01f) << "h" << i + 1;
        }
    }
}

// ズームは本文だけに掛ける。サイドペインの寸法を一緒に拡大すると、
// ファイル一覧や目次で同時に見える項目数が変わってしまう。
TEST(Theme, ApplyZoomKeepsSidePaneMetrics)
{
    const Theme& base = GetLightTheme();
    for (const float zoom : { 0.5f, 0.8f, 1.25f, 2.0f }) {
        SCOPED_TRACE(zoom);
        Theme t = base;
        t.ApplyZoom(zoom);
        for (const auto& f : kPaneFields) {
            EXPECT_FLOAT_EQ(t.*f.member, base.*f.member) << f.name;
        }
        // 本文側はスケールしていること (取り違え防止)。
        EXPECT_NEAR(t.font_size_body, base.font_size_body * zoom, 0.01f);
    }
}
