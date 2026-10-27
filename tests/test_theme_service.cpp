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
    bool result = service_.ToggleDarkMode(ZoomLevel{});
    EXPECT_TRUE(result);
    EXPECT_TRUE(service_.IsDarkMode());

    result = service_.ToggleDarkMode(ZoomLevel{});
    EXPECT_FALSE(result);
    EXPECT_FALSE(service_.IsDarkMode());
}

// 毎回コピーせず、プロセス共有の定数テーマをそのまま返す。
TEST_F(ThemeServiceTest, BaseThemeFollowsDarkMode)
{
    EXPECT_EQ(&service_.BaseTheme(), &GetLightTheme());
    service_.ToggleDarkMode(ZoomLevel{});
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
    service_.ToggleDarkMode(ZoomLevel{});
    Theme theme = service_.CreateTheme(ZoomAt(12));

    Theme expected = GetDarkTheme();
    expected.ApplyZoom(ZOOM_STEPS[12]);
    EXPECT_FLOAT_EQ(theme.zoom, expected.zoom);
    EXPECT_TRUE(theme.dark);
    EXPECT_EQ(theme.Color(BrushId::Background).r, expected.Color(BrushId::Background).r);
}

TEST_F(ThemeServiceTest, LoadZoomLevelDefault)
{
    const int idx = service_.LoadZoomLevel().Index();
    EXPECT_GE(idx, 0);
    EXPECT_LT(idx, ZOOM_STEP_COUNT);
}

// 今のテーマはフォントを実在する候補に決め、ダークモードとズームの切り替えでも同じ場所を書き換える。
TEST_F(ThemeServiceTest, CurrentThemeIsUpdatedInPlaceKeepingFonts)
{
    service_.Init(ZoomAt(12));
    const Theme* const current = &service_.GetTheme();
    const std::wstring mono = current->monospace_font;
    const std::wstring icon = current->icon_font;
    EXPECT_FLOAT_EQ(current->zoom, ZOOM_STEPS[12]);
    EXPECT_FALSE(mono.empty());
    EXPECT_FALSE(icon.empty());

    service_.ToggleDarkMode(ZoomAt(12));
    EXPECT_EQ(&service_.GetTheme(), current);
    EXPECT_TRUE(current->dark);
    EXPECT_FLOAT_EQ(current->zoom, ZOOM_STEPS[12]);
    EXPECT_EQ(current->Color(BrushId::Text).r, GetDarkTheme().Color(BrushId::Text).r);
    EXPECT_EQ(current->monospace_font, mono);
    EXPECT_EQ(current->icon_font, icon);

    service_.ApplyZoom(ZoomLevel{});
    EXPECT_EQ(&service_.GetTheme(), current);
    EXPECT_TRUE(current->dark);
    EXPECT_FLOAT_EQ(current->zoom, 1.0f);
    EXPECT_FLOAT_EQ(current->font_size_body, GetDarkTheme().font_size_body);
    EXPECT_EQ(current->monospace_font, mono);
    EXPECT_EQ(current->icon_font, icon);
}
