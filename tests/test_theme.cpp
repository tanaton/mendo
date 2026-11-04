#include <gtest/gtest.h>
#include "theme.h"
#include "theme_service.h"
#include "config_service.h"

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

// ---- ThemeService ----

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
