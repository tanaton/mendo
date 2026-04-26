#include <gtest/gtest.h>
#include "theme_service.h"
#include "config_service.h"

class ThemeServiceTest : public ::testing::Test {
protected:
    ConfigService config_;
    ThemeService service_{ config_ };
};

TEST_F(ThemeServiceTest, InitiallyLightMode)
{
    EXPECT_FALSE(service_.IsDarkMode());
}

TEST_F(ThemeServiceTest, ToggleDarkMode)
{
    bool result = service_.ToggleDarkMode();
    EXPECT_TRUE(result);
    EXPECT_TRUE(service_.IsDarkMode());

    result = service_.ToggleDarkMode();
    EXPECT_FALSE(result);
    EXPECT_FALSE(service_.IsDarkMode());
}

TEST_F(ThemeServiceTest, CreateThemeLightMode)
{
    Theme theme = service_.CreateTheme();
    Theme expected = GetLightTheme();
    EXPECT_EQ(theme.bg_color.r, expected.bg_color.r);
    EXPECT_EQ(theme.bg_color.g, expected.bg_color.g);
    EXPECT_EQ(theme.bg_color.b, expected.bg_color.b);
}

TEST_F(ThemeServiceTest, CreateThemeDarkMode)
{
    service_.ToggleDarkMode();
    Theme theme = service_.CreateTheme();
    Theme expected = GetDarkTheme();
    EXPECT_EQ(theme.bg_color.r, expected.bg_color.r);
    EXPECT_EQ(theme.bg_color.g, expected.bg_color.g);
    EXPECT_EQ(theme.bg_color.b, expected.bg_color.b);
}

TEST_F(ThemeServiceTest, CreateThemeDefaultZoom)
{
    Theme theme = service_.CreateTheme(ZOOM_DEFAULT_INDEX);
    EXPECT_FLOAT_EQ(theme.zoom, 1.0f);
}

TEST_F(ThemeServiceTest, CreateThemeWithZoom)
{
    // Use zoom index 12 (2.00x)
    Theme theme = service_.CreateTheme(12);
    EXPECT_FLOAT_EQ(theme.zoom, ZOOM_STEPS[12]);
}

TEST_F(ThemeServiceTest, CreateThemeDarkModeWithZoom)
{
    service_.ToggleDarkMode();
    Theme theme = service_.CreateTheme(12);

    Theme expected = GetDarkTheme();
    expected.ApplyZoom(ZOOM_STEPS[12]);
    EXPECT_FLOAT_EQ(theme.zoom, expected.zoom);
    EXPECT_EQ(theme.bg_color.r, expected.bg_color.r);
}

TEST_F(ThemeServiceTest, LoadZoomIndexDefault)
{
    int idx = service_.LoadZoomIndex();
    EXPECT_GE(idx, 0);
    EXPECT_LT(idx, ZOOM_STEP_COUNT);
}
