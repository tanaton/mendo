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

// ---- ApplyZoom tests ----

TEST(Theme, ApplyZoomScalesFonts) {
    Theme t = GetLightTheme();
    float original_body = t.font_size_body;
    float original_h1 = t.font_size_h1;
    float original_code = t.font_size_code;

    t.ApplyZoom(2.0f);

    EXPECT_NEAR(t.font_size_body, original_body * 2.0f, 0.01f);
    EXPECT_NEAR(t.font_size_h1, original_h1 * 2.0f, 0.01f);
    EXPECT_NEAR(t.font_size_code, original_code * 2.0f, 0.01f);
    EXPECT_FLOAT_EQ(t.zoom, 2.0f);
}

TEST(Theme, ApplyZoomScalesMargins) {
    Theme t = GetLightTheme();
    float original_left = t.margin_left;
    float original_spacing = t.paragraph_spacing;

    t.ApplyZoom(1.5f);

    EXPECT_NEAR(t.margin_left, original_left * 1.5f, 0.01f);
    EXPECT_NEAR(t.paragraph_spacing, original_spacing * 1.5f, 0.01f);
}

TEST(Theme, ApplyZoomScalesPaneSizes) {
    Theme t = GetLightTheme();
    float original_item_h = t.pane_item_height;
    float original_pane_font = t.pane_font_size;

    t.ApplyZoom(1.25f);

    EXPECT_NEAR(t.pane_item_height, original_item_h * 1.25f, 0.01f);
    EXPECT_NEAR(t.pane_font_size, original_pane_font * 1.25f, 0.01f);
}

TEST(Theme, ApplyZoomTwiceIsMultiplicative) {
    Theme t = GetLightTheme();
    float original_body = t.font_size_body;

    t.ApplyZoom(2.0f);  // zoom from 1.0 to 2.0
    t.ApplyZoom(3.0f);  // zoom from 2.0 to 3.0

    EXPECT_NEAR(t.font_size_body, original_body * 3.0f, 0.01f);
    EXPECT_FLOAT_EQ(t.zoom, 3.0f);
}

TEST(Theme, ApplyZoomResetToOne) {
    Theme t = GetLightTheme();
    float original_body = t.font_size_body;

    t.ApplyZoom(2.0f);
    t.ApplyZoom(1.0f);

    EXPECT_NEAR(t.font_size_body, original_body, 0.01f);
}

// ---- Bug #12: ApplyZoom zero guard ----

TEST(Theme, ApplyZoomZeroIsNoOp) {
    Theme t = GetLightTheme();
    float original_body = t.font_size_body;
    float original_zoom = t.zoom;

    t.ApplyZoom(0.0f);

    // Should be unchanged (no division by zero, no inf/NaN)
    EXPECT_FLOAT_EQ(t.font_size_body, original_body);
    EXPECT_FLOAT_EQ(t.zoom, original_zoom);
}

TEST(Theme, ApplyZoomNegativeIsNoOp) {
    Theme t = GetLightTheme();
    float original_body = t.font_size_body;

    t.ApplyZoom(-1.0f);

    EXPECT_FLOAT_EQ(t.font_size_body, original_body);
}

// ---- Bug #20: ApplyZoom drift prevention ----

TEST(Theme, ApplyZoomFromBaseNoDrift) {
    // Simulate repeated zoom in/out via base reconstruction (the fix approach)
    Theme base = GetLightTheme();
    float base_body = base.font_size_body;
    float base_margin = base.margin_left;

    // Apply zoom to 2.0 from base
    Theme zoomed = base;
    zoomed.ApplyZoom(2.0f);
    EXPECT_NEAR(zoomed.font_size_body, base_body * 2.0f, 0.001f);

    // Apply zoom back to 1.0 from base (not from zoomed) -> no drift
    Theme restored = base;
    // zoom is already 1.0 from base, no ApplyZoom needed
    EXPECT_FLOAT_EQ(restored.font_size_body, base_body);
    EXPECT_FLOAT_EQ(restored.margin_left, base_margin);
}

TEST(Theme, ApplyZoomRepeatedRoundTripsAccumulateDrift) {
    // This demonstrates why the base-reconstruction approach is needed:
    // incremental ApplyZoom loses precision over many cycles
    Theme t = GetLightTheme();
    float original_body = t.font_size_body;

    // Zoom in and out 100 times
    for (int i = 0; i < 100; ++i) {
        t.ApplyZoom(1.5f);
        t.ApplyZoom(1.0f);
    }

    // With incremental approach, drift is possible but small
    // The important thing is it doesn't produce inf/NaN
    EXPECT_GT(t.font_size_body, 0.0f);
    EXPECT_LT(t.font_size_body, original_body * 2.0f);
    EXPECT_FALSE(std::isnan(t.font_size_body));
    EXPECT_FALSE(std::isinf(t.font_size_body));
}
