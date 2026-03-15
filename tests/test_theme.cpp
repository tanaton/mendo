#include <gtest/gtest.h>
#include "theme.h"

TEST(Theme, LightThemeHasPositiveFontSizes) {
    Theme t = GetLightTheme();
    EXPECT_GT(t.font_size_body, 0.0f);
    EXPECT_GT(t.font_size_h1, 0.0f);
    EXPECT_GT(t.font_size_h2, 0.0f);
    EXPECT_GT(t.font_size_h3, 0.0f);
    EXPECT_GT(t.font_size_h4, 0.0f);
    EXPECT_GT(t.font_size_h5, 0.0f);
    EXPECT_GT(t.font_size_h6, 0.0f);
    EXPECT_GT(t.font_size_code, 0.0f);
}

TEST(Theme, HeadingSizesDecrease) {
    Theme t = GetLightTheme();
    EXPECT_GT(t.font_size_h1, t.font_size_h2);
    EXPECT_GT(t.font_size_h2, t.font_size_h3);
    EXPECT_GE(t.font_size_h3, t.font_size_h4);
    EXPECT_GE(t.font_size_h4, t.font_size_h5);
    EXPECT_GE(t.font_size_h5, t.font_size_h6);
}

TEST(Theme, GetHeadingSizeReturnsCorrectLevel) {
    Theme t = GetLightTheme();
    EXPECT_EQ(t.GetHeadingSize(1), t.font_size_h1);
    EXPECT_EQ(t.GetHeadingSize(2), t.font_size_h2);
    EXPECT_EQ(t.GetHeadingSize(3), t.font_size_h3);
    EXPECT_EQ(t.GetHeadingSize(4), t.font_size_h4);
    EXPECT_EQ(t.GetHeadingSize(5), t.font_size_h5);
    EXPECT_EQ(t.GetHeadingSize(6), t.font_size_h6);
}

TEST(Theme, GetHeadingSizeInvalidLevelReturnsBody) {
    Theme t = GetLightTheme();
    EXPECT_EQ(t.GetHeadingSize(0), t.font_size_body);
    EXPECT_EQ(t.GetHeadingSize(7), t.font_size_body);
    EXPECT_EQ(t.GetHeadingSize(-1), t.font_size_body);
}

TEST(Theme, PositiveMargins) {
    Theme t = GetLightTheme();
    EXPECT_GT(t.margin_left, 0.0f);
    EXPECT_GT(t.margin_right, 0.0f);
    EXPECT_GT(t.margin_top, 0.0f);
    EXPECT_GT(t.paragraph_spacing, 0.0f);
    EXPECT_GT(t.indent_width, 0.0f);
}

TEST(Theme, FontFamilyNotEmpty) {
    Theme t = GetLightTheme();
    EXPECT_GT(wcslen(t.font_family), 0u);
    EXPECT_GT(wcslen(t.monospace_font), 0u);
}

TEST(Theme, BackgroundColorIsWhite) {
    Theme t = GetLightTheme();
    EXPECT_FLOAT_EQ(t.bg_color.r, 1.0f);
    EXPECT_FLOAT_EQ(t.bg_color.g, 1.0f);
    EXPECT_FLOAT_EQ(t.bg_color.b, 1.0f);
}

// ---- Dark theme tests ----

TEST(Theme, DarkThemeHasPositiveFontSizes) {
    Theme t = GetDarkTheme();
    EXPECT_GT(t.font_size_body, 0.0f);
    EXPECT_GT(t.font_size_h1, 0.0f);
    EXPECT_GT(t.font_size_h2, 0.0f);
    EXPECT_GT(t.font_size_h3, 0.0f);
    EXPECT_GT(t.font_size_h4, 0.0f);
    EXPECT_GT(t.font_size_h5, 0.0f);
    EXPECT_GT(t.font_size_h6, 0.0f);
    EXPECT_GT(t.font_size_code, 0.0f);
}

TEST(Theme, DarkThemeHeadingSizesDecrease) {
    Theme t = GetDarkTheme();
    EXPECT_GT(t.font_size_h1, t.font_size_h2);
    EXPECT_GT(t.font_size_h2, t.font_size_h3);
    EXPECT_GE(t.font_size_h3, t.font_size_h4);
    EXPECT_GE(t.font_size_h4, t.font_size_h5);
    EXPECT_GE(t.font_size_h5, t.font_size_h6);
}

TEST(Theme, DarkThemeBackgroundIsDark) {
    Theme t = GetDarkTheme();
    // Dark theme should have a dark background (low RGB values)
    EXPECT_LT(t.bg_color.r, 0.3f);
    EXPECT_LT(t.bg_color.g, 0.3f);
    EXPECT_LT(t.bg_color.b, 0.3f);
}

TEST(Theme, DarkThemeTextIsLight) {
    Theme t = GetDarkTheme();
    // Dark theme text should be light (high RGB values)
    EXPECT_GT(t.text_color.r, 0.7f);
    EXPECT_GT(t.text_color.g, 0.7f);
    EXPECT_GT(t.text_color.b, 0.7f);
}

TEST(Theme, DarkThemePositiveMargins) {
    Theme t = GetDarkTheme();
    EXPECT_GT(t.margin_left, 0.0f);
    EXPECT_GT(t.margin_right, 0.0f);
    EXPECT_GT(t.margin_top, 0.0f);
    EXPECT_GT(t.paragraph_spacing, 0.0f);
    EXPECT_GT(t.indent_width, 0.0f);
}

TEST(Theme, DarkThemeFontFamilyNotEmpty) {
    Theme t = GetDarkTheme();
    EXPECT_GT(wcslen(t.font_family), 0u);
    EXPECT_GT(wcslen(t.monospace_font), 0u);
}

TEST(Theme, DarkAndLightHaveSameFontSizes) {
    Theme light = GetLightTheme();
    Theme dark = GetDarkTheme();
    EXPECT_FLOAT_EQ(light.font_size_body, dark.font_size_body);
    EXPECT_FLOAT_EQ(light.font_size_h1, dark.font_size_h1);
    EXPECT_FLOAT_EQ(light.font_size_code, dark.font_size_code);
}

TEST(Theme, DarkAndLightHaveSameSpacing) {
    Theme light = GetLightTheme();
    Theme dark = GetDarkTheme();
    EXPECT_FLOAT_EQ(light.margin_left, dark.margin_left);
    EXPECT_FLOAT_EQ(light.margin_top, dark.margin_top);
    EXPECT_FLOAT_EQ(light.paragraph_spacing, dark.paragraph_spacing);
    EXPECT_FLOAT_EQ(light.heading_spacing_above, dark.heading_spacing_above);
    EXPECT_FLOAT_EQ(light.heading_spacing_below, dark.heading_spacing_below);
}

// ---- ApplyCommonLayout consistency tests ----

TEST(Theme, DarkAndLightHaveSameIndentation) {
    Theme light = GetLightTheme();
    Theme dark = GetDarkTheme();
    EXPECT_FLOAT_EQ(light.indent_width, dark.indent_width);
    EXPECT_FLOAT_EQ(light.margin_right, dark.margin_right);
    EXPECT_FLOAT_EQ(light.code_block_padding, dark.code_block_padding);
}

TEST(Theme, DarkAndLightHaveSamePaneLayout) {
    Theme light = GetLightTheme();
    Theme dark = GetDarkTheme();
    EXPECT_FLOAT_EQ(light.splitter_width, dark.splitter_width);
    EXPECT_FLOAT_EQ(light.pane_item_height, dark.pane_item_height);
    EXPECT_FLOAT_EQ(light.pane_header_height, dark.pane_header_height);
}

TEST(Theme, DarkAndLightHaveSameAllHeadingSizes) {
    Theme light = GetLightTheme();
    Theme dark = GetDarkTheme();
    EXPECT_FLOAT_EQ(light.font_size_h2, dark.font_size_h2);
    EXPECT_FLOAT_EQ(light.font_size_h3, dark.font_size_h3);
    EXPECT_FLOAT_EQ(light.font_size_h4, dark.font_size_h4);
    EXPECT_FLOAT_EQ(light.font_size_h5, dark.font_size_h5);
    EXPECT_FLOAT_EQ(light.font_size_h6, dark.font_size_h6);
}

TEST(Theme, GetHeadingSizeLargeLevel) {
    Theme t = GetLightTheme();
    EXPECT_EQ(t.GetHeadingSize(100), t.font_size_body);
    EXPECT_EQ(t.GetHeadingSize(-100), t.font_size_body);
}

TEST(Theme, DarkAndLightDifferentColors) {
    Theme light = GetLightTheme();
    Theme dark = GetDarkTheme();
    // Background colors should be different
    EXPECT_NE(light.bg_color.r, dark.bg_color.r);
    // Text colors should be different
    EXPECT_NE(light.text_color.r, dark.text_color.r);
}

TEST(Theme, LightThemeTextIsDark) {
    Theme t = GetLightTheme();
    EXPECT_LT(t.text_color.r, 0.3f);
    EXPECT_LT(t.text_color.g, 0.3f);
    EXPECT_LT(t.text_color.b, 0.3f);
}
