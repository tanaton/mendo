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

// 毎回コピーせず、プロセス共有の定数テーマをそのまま返す。
TEST_F(ThemeServiceTest, BaseThemeFollowsDarkMode)
{
    EXPECT_EQ(&service_.BaseTheme(), &GetLightTheme());
    service_.ToggleDarkMode();
    EXPECT_EQ(&service_.BaseTheme(), &GetDarkTheme());
}

namespace {
ZoomLevel ZoomAt(int index)
{
    ZoomLevel zoom;
    zoom.SetIndex(index);
    return zoom;
}
} // namespace

TEST_F(ThemeServiceTest, CreateThemeDefaultZoom)
{
    Theme theme = service_.CreateTheme(ZoomLevel{});
    EXPECT_FLOAT_EQ(theme.zoom, 1.0f);
}

TEST_F(ThemeServiceTest, CreateThemeWithZoom)
{
    Theme theme = service_.CreateTheme(ZoomAt(12));
    EXPECT_FLOAT_EQ(theme.zoom, ZOOM_STEPS[12]);
}

TEST_F(ThemeServiceTest, CreateThemeDarkModeWithZoom)
{
    service_.ToggleDarkMode();
    Theme theme = service_.CreateTheme(ZoomAt(12));

    Theme expected = GetDarkTheme();
    expected.ApplyZoom(ZOOM_STEPS[12]);
    EXPECT_FLOAT_EQ(theme.zoom, expected.zoom);
    EXPECT_EQ(theme.bg_color.r, expected.bg_color.r);
}

TEST_F(ThemeServiceTest, LoadZoomLevelDefault)
{
    const int idx = service_.LoadZoomLevel().Index();
    EXPECT_GE(idx, 0);
    EXPECT_LT(idx, ZOOM_STEP_COUNT);
}
