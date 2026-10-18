#include <gtest/gtest.h>
#include "theme.h"
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

// ライト/ダークで共通の値を持ち、ズームでスケールする寸法。font_size_h は配列なので別に扱う。
struct LayoutField {
    const char* name;
    float Theme::* member;
};
constexpr LayoutField kLayoutFields[] = {
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
    { "pane_item_height", &Theme::pane_item_height },
    { "pane_header_height", &Theme::pane_header_height },
    { "splitter_width", &Theme::splitter_width },
    { "pane_font_size", &Theme::pane_font_size },
};

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
        for (const auto& f : kLayoutFields) {
            EXPECT_GT(t.*f.member, 0.0f) << f.name;
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
    EXPECT_FLOAT_EQ(t.bg_color.r, 1.0f);
    EXPECT_FLOAT_EQ(t.bg_color.g, 1.0f);
    EXPECT_FLOAT_EQ(t.bg_color.b, 1.0f);
    EXPECT_LT(t.text_color.r, 0.3f);
    EXPECT_LT(t.text_color.g, 0.3f);
    EXPECT_LT(t.text_color.b, 0.3f);
    EXPECT_FALSE(t.IsDark());
}

TEST(Theme, DarkThemeIsLightTextOnDark)
{
    const Theme& t = GetDarkTheme();
    EXPECT_LT(t.bg_color.r, 0.3f);
    EXPECT_LT(t.bg_color.g, 0.3f);
    EXPECT_LT(t.bg_color.b, 0.3f);
    EXPECT_GT(t.text_color.r, 0.7f);
    EXPECT_GT(t.text_color.g, 0.7f);
    EXPECT_GT(t.text_color.b, 0.7f);
    EXPECT_TRUE(t.IsDark());
}

// ApplyCommonLayout を両テーマで共有している契約。
TEST(Theme, DarkAndLightShareLayout)
{
    const Theme& light = GetLightTheme();
    const Theme& dark = GetDarkTheme();
    for (const auto& f : kLayoutFields) {
        EXPECT_FLOAT_EQ(light.*f.member, dark.*f.member) << f.name;
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
    EXPECT_NE(light.bg_color.r, dark.bg_color.r);
    EXPECT_NE(light.text_color.r, dark.text_color.r);
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
        for (const auto& f : kLayoutFields) {
            EXPECT_NEAR(t.*f.member, base.*f.member * zoom, 0.01f) << f.name;
        }
        for (int i = 0; i < 6; ++i) {
            EXPECT_NEAR(t.font_size_h[i], base.font_size_h[i] * zoom, 0.01f) << "h" << i + 1;
        }
    }
}

TEST(Theme, ApplyZoomTwiceIsMultiplicative)
{
    Theme t = GetLightTheme();
    const float original_body = t.font_size_body;

    t.ApplyZoom(2.0f);
    t.ApplyZoom(3.0f);

    EXPECT_NEAR(t.font_size_body, original_body * 3.0f, 0.01f);
    EXPECT_FLOAT_EQ(t.zoom, 3.0f);
}

TEST(Theme, ApplyZoomResetToOne)
{
    Theme t = GetLightTheme();
    const float original_body = t.font_size_body;

    t.ApplyZoom(2.0f);
    t.ApplyZoom(1.0f);

    EXPECT_NEAR(t.font_size_body, original_body, 0.01f);
}

// バグ #12: ゼロ除算と inf/NaN の伝染を防ぐ。
TEST(Theme, ApplyZoomNonPositiveIsNoOp)
{
    for (const float zoom : { 0.0f, -1.0f }) {
        SCOPED_TRACE(zoom);
        Theme t = GetLightTheme();
        const float original_body = t.font_size_body;
        const float original_zoom = t.zoom;

        t.ApplyZoom(zoom);

        EXPECT_FLOAT_EQ(t.font_size_body, original_body);
        EXPECT_FLOAT_EQ(t.zoom, original_zoom);
    }
}

// バグ #20: 増分のズームを繰り返すと誤差は出るが、inf/NaN にはならない。
TEST(Theme, ApplyZoomRepeatedRoundTripsStayFinite)
{
    Theme t = GetLightTheme();
    const float original_body = t.font_size_body;

    for (int i = 0; i < 100; ++i) {
        t.ApplyZoom(1.5f);
        t.ApplyZoom(1.0f);
    }

    EXPECT_GT(t.font_size_body, 0.0f);
    EXPECT_LT(t.font_size_body, original_body * 2.0f);
    EXPECT_FALSE(std::isnan(t.font_size_body));
    EXPECT_FALSE(std::isinf(t.font_size_body));
}

// ---- GitHub Alerts ----

TEST(Theme, AlertColorsAreOpaqueAndNonBlack)
{
    for (const auto& [name, get] : kBothThemes) {
        SCOPED_TRACE(name);
        const Theme& t = get();
        for (size_t i = 0; i < ALERT_TYPE_COUNT; ++i) {
            const float sum = t.alert_color[i].r + t.alert_color[i].g + t.alert_color[i].b;
            EXPECT_GT(sum, 0.0f) << "alert_color[" << i << "]";
            EXPECT_FLOAT_EQ(t.alert_color[i].a, 1.0f) << "alert_color[" << i << "]";
        }
    }
}

TEST(Theme, AlertBgColorsAreTranslucent)
{
    for (const auto& [name, get] : kBothThemes) {
        SCOPED_TRACE(name);
        const Theme& t = get();
        for (size_t i = 0; i < ALERT_TYPE_COUNT; ++i) {
            EXPECT_GT(t.alert_bg_color[i].a, 0.0f) << "alert_bg_color[" << i << "]";
            EXPECT_LT(t.alert_bg_color[i].a, 1.0f) << "alert_bg_color[" << i << "]";
        }
    }
}

TEST(Theme, LightAndDarkAlertColorsDiffer)
{
    const Theme& light = GetLightTheme();
    const Theme& dark = GetDarkTheme();
    for (size_t i = 0; i < ALERT_TYPE_COUNT; ++i) {
        EXPECT_FALSE(SameRgb(light.alert_color[i], dark.alert_color[i])) << "alert_color[" << i << "]";
    }
}

TEST(Theme, AlertColorsAreDistinct)
{
    const Theme& t = GetLightTheme();
    for (size_t i = 0; i < ALERT_TYPE_COUNT; ++i) {
        for (size_t j = i + 1; j < ALERT_TYPE_COUNT; ++j) {
            EXPECT_FALSE(SameRgb(t.alert_color[i], t.alert_color[j])) << "alert_color[" << i << "] と [" << j << "]";
        }
    }
}
