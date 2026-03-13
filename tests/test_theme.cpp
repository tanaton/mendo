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
