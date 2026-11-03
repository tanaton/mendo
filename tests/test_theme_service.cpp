#include <gtest/gtest.h>
#include "theme_service.h"
#include "config_service.h"

class ThemeServiceTest : public ::testing::Test {
protected:
    ConfigService config_;
    ThemeService service_;
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

namespace {
ZoomLevel ZoomAt(int index)
{
    ZoomLevel zoom;
    zoom.SetIndex(index);
    return zoom;
}
} // namespace

// 作るテーマは今のライト/ダークに従い、渡したズームを掛ける。
TEST_F(ThemeServiceTest, CreateThemeFollowsDarkModeAndZoom)
{
    const Theme light_default = service_.CreateTheme(ZoomLevel{});
    EXPECT_FALSE(light_default.dark);
    EXPECT_FLOAT_EQ(light_default.zoom, 1.0f);
    EXPECT_FLOAT_EQ(service_.CreateTheme(ZoomAt(12)).zoom, ZOOM_STEPS[12]);

    service_.ToggleDarkMode();
    const Theme theme = service_.CreateTheme(ZoomAt(12));
    Theme expected = GetDarkTheme();
    expected.ApplyZoom(ZOOM_STEPS[12]);
    EXPECT_FLOAT_EQ(theme.zoom, expected.zoom);
    EXPECT_TRUE(theme.dark);
    EXPECT_EQ(theme.Color(BrushId::Background).r, expected.Color(BrushId::Background).r);
}

// 保存が無ければ等倍、保存した倍率はそのまま読み戻す。
TEST_F(ThemeServiceTest, LoadZoomLevelDefaultsToActualSizeAndRoundTrips)
{
    EXPECT_TRUE(session::LoadZoomLevel(config_).IsDefault());
    session::SaveZoomLevel(config_, ZoomAt(12));
    EXPECT_EQ(session::LoadZoomLevel(config_).Index(), 12);
}

// 今のテーマはフォントを実在する候補に決め、ダークモードとズームの切り替えでも同じ場所を書き換える。
TEST_F(ThemeServiceTest, CurrentThemeIsUpdatedInPlaceKeepingFonts)
{
    service_.Init(false, ZoomAt(12));
    const Theme* const current = &service_.GetTheme();
    const std::wstring mono = current->monospace_font;
    const std::wstring icon = current->icon_font;
    EXPECT_FLOAT_EQ(current->zoom, ZOOM_STEPS[12]);
    EXPECT_FALSE(mono.empty());
    EXPECT_FALSE(icon.empty());

    service_.ToggleDarkMode();
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
