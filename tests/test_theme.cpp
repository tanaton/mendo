#include <gtest/gtest.h>
#include "theme.h"
#include <span>
#include <cmath>

namespace {

struct ThemeCase {
    const char* name;
    const Theme& (*get)();
};

constexpr ThemeCase kBothThemes[] = {
    { "light", GetLightTheme },
    { "dark", GetDarkTheme },
};

// ライト/ダークで共通の値を持つ寸法。font_size_h は配列なので別に扱う。
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
constexpr std::span<const LayoutField> kAllLayoutFields[] = { kScalableFields, kPaneFields };

bool SameRgb(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

} // namespace

TEST(Theme, LayoutValuesArePositive)
{
    for (const auto& [name, get] : kBothThemes) {
        SCOPED_TRACE(name);
        const Theme& t = get();
        for (const auto fields : kAllLayoutFields) {
            for (const auto& f : fields) {
                EXPECT_GT(t.*f.member, 0.0f) << f.name;
            }
        }
        for (int i = 0; i < 6; ++i) {
            EXPECT_GT(t.font_size_h[i], 0.0f) << "h" << i + 1;
        }
    }
}

TEST(Theme, HeadingSizesDecrease)
{
    for (const auto& [name, get] : kBothThemes) {
        SCOPED_TRACE(name);
        const Theme& t = get();
        EXPECT_GT(t.font_size_h[0], t.font_size_h[1]);
        EXPECT_GT(t.font_size_h[1], t.font_size_h[2]);
        EXPECT_GE(t.font_size_h[2], t.font_size_h[3]);
        EXPECT_GE(t.font_size_h[3], t.font_size_h[4]);
        EXPECT_GE(t.font_size_h[4], t.font_size_h[5]);
    }
}

TEST(Theme, GetHeadingSize)
{
    const Theme& t = GetLightTheme();
    for (int level = 1; level <= 6; ++level) {
        EXPECT_EQ(t.GetHeadingSize(level), t.font_size_h[level - 1]) << "level " << level;
    }
    for (const int level : { 0, 7, -1, 100, -100 }) {
        EXPECT_EQ(t.GetHeadingSize(level), t.font_size_body) << "level " << level;
    }
}

TEST(Theme, FontFamilyNotEmpty)
{
    for (const auto& [name, get] : kBothThemes) {
        SCOPED_TRACE(name);
        const Theme& t = get();
        EXPECT_FALSE(t.font_family.empty());
        EXPECT_FALSE(t.monospace_font.empty());
        EXPECT_FALSE(t.icon_font.empty());
    }
}

TEST(Theme, LightThemeIsDarkTextOnWhite)
{
    const Theme& t = GetLightTheme();
    const auto bg = t.Color(BrushId::Background);
    const auto text = t.Color(BrushId::Text);
    EXPECT_FLOAT_EQ(bg.r, 1.0f);
    EXPECT_FLOAT_EQ(bg.g, 1.0f);
    EXPECT_FLOAT_EQ(bg.b, 1.0f);
    EXPECT_LT(text.r, 0.3f);
    EXPECT_LT(text.g, 0.3f);
    EXPECT_LT(text.b, 0.3f);
    EXPECT_FALSE(t.dark);
}

TEST(Theme, DarkThemeIsLightTextOnDark)
{
    const Theme& t = GetDarkTheme();
    const auto bg = t.Color(BrushId::Background);
    const auto text = t.Color(BrushId::Text);
    EXPECT_LT(bg.r, 0.3f);
    EXPECT_LT(bg.g, 0.3f);
    EXPECT_LT(bg.b, 0.3f);
    EXPECT_GT(text.r, 0.7f);
    EXPECT_GT(text.g, 0.7f);
    EXPECT_GT(text.b, 0.7f);
    EXPECT_TRUE(t.dark);
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

// ApplyCommonLayout を両テーマで共有している契約。
TEST(Theme, DarkAndLightShareLayout)
{
    const Theme& light = GetLightTheme();
    const Theme& dark = GetDarkTheme();
    for (const auto fields : kAllLayoutFields) {
        for (const auto& f : fields) {
            EXPECT_FLOAT_EQ(light.*f.member, dark.*f.member) << f.name;
        }
    }
    for (int i = 0; i < 6; ++i) {
        EXPECT_FLOAT_EQ(light.font_size_h[i], dark.font_size_h[i]) << "h" << i + 1;
    }
    EXPECT_EQ(light.font_family, dark.font_family);
    EXPECT_EQ(light.monospace_font, dark.monospace_font);
    EXPECT_EQ(light.icon_font, dark.icon_font);
}

TEST(Theme, DarkAndLightDifferentColors)
{
    const Theme& light = GetLightTheme();
    const Theme& dark = GetDarkTheme();
    EXPECT_NE(light.Color(BrushId::Background).r, dark.Color(BrushId::Background).r);
    EXPECT_NE(light.Color(BrushId::Text).r, dark.Color(BrushId::Text).r);
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

// 白黒の重ね色はテーマが決める。ライトは黒、ダークは白 (ジェスチャーの背景は灰色)。
TEST(Theme, OverlayColorsFollowDarkMode)
{
    const Theme& light = GetLightTheme();
    const Theme& dark = GetDarkTheme();
    EXPECT_FLOAT_EQ(light.Color(BrushId::Overlay).r, 0.0f);
    EXPECT_FLOAT_EQ(dark.Color(BrushId::Overlay).r, 1.0f);
    EXPECT_FLOAT_EQ(light.Color(BrushId::OverlayGestureBg).r, 0.0f);
    EXPECT_FLOAT_EQ(dark.Color(BrushId::OverlayGestureBg).r, 0.2f);
}

// ---- GitHub Alerts ----

TEST(Theme, AlertColorsAreOpaqueAndNonBlack)
{
    for (const auto& [name, get] : kBothThemes) {
        SCOPED_TRACE(name);
        const Theme& t = get();
        for (size_t i = 0; i < ALERT_TYPE_COUNT; ++i) {
            const auto c = t.Color(AlertBrushIdFromIndex(i));
            EXPECT_GT(c.r + c.g + c.b, 0.0f) << "alert " << i;
            EXPECT_FLOAT_EQ(c.a, 1.0f) << "alert " << i;
        }
    }
}

TEST(Theme, AlertBgColorsAreTranslucent)
{
    for (const auto& [name, get] : kBothThemes) {
        SCOPED_TRACE(name);
        const Theme& t = get();
        for (size_t i = 0; i < ALERT_TYPE_COUNT; ++i) {
            const auto c = t.Color(AlertBgBrushIdFromIndex(i));
            EXPECT_GT(c.a, 0.0f) << "alert bg " << i;
            EXPECT_LT(c.a, 1.0f) << "alert bg " << i;
        }
    }
}

TEST(Theme, LightAndDarkAlertColorsDiffer)
{
    const Theme& light = GetLightTheme();
    const Theme& dark = GetDarkTheme();
    for (size_t i = 0; i < ALERT_TYPE_COUNT; ++i) {
        EXPECT_FALSE(SameRgb(light.Color(AlertBrushIdFromIndex(i)), dark.Color(AlertBrushIdFromIndex(i)))) << "alert " << i;
    }
}

TEST(Theme, AlertColorsAreDistinct)
{
    const Theme& t = GetLightTheme();
    for (size_t i = 0; i < ALERT_TYPE_COUNT; ++i) {
        for (size_t j = i + 1; j < ALERT_TYPE_COUNT; ++j) {
            EXPECT_FALSE(SameRgb(t.Color(AlertBrushIdFromIndex(i)), t.Color(AlertBrushIdFromIndex(j)))) << "alert " << i << " と " << j;
        }
    }
}
