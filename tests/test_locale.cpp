#include <gtest/gtest.h>
#include "i18n.h"
#include <string>
#include <string_view>

class LocaleTest : public ::testing::Test {
protected:
    void TearDown() override
    {
        // テスト後にデフォルト状態に戻す
        i18n::g_strings = &i18n::kJa;
    }
};

TEST_F(LocaleTest, ExplicitJa)
{
    i18n::Init(L"ja");
    EXPECT_EQ(&i18n::S(), &i18n::kJa);
}

TEST_F(LocaleTest, ExplicitEn)
{
    i18n::Init(L"en");
    EXPECT_EQ(&i18n::S(), &i18n::kEn);
}

TEST_F(LocaleTest, ExplicitZh)
{
    i18n::Init(L"zh");
    EXPECT_EQ(&i18n::S(), &i18n::kZh);
}

// 終了時に保存したキーで次回も同じ言語になる。
TEST_F(LocaleTest, LangKeyRoundTrips)
{
    for (const wchar_t* key : { L"ja", L"en", L"zh" }) {
        i18n::Init(key);
        EXPECT_EQ(i18n::GetLangKey(), key);
    }
}

TEST_F(LocaleTest, UnknownLangFallsBackToOsDetection)
{
    i18n::Init(L"fr");
    // OS依存だが、用意した言語のどれかであること
    EXPECT_TRUE(&i18n::S() == &i18n::kJa || &i18n::S() == &i18n::kEn || &i18n::S() == &i18n::kZh);
}

TEST_F(LocaleTest, EmptyLangFallsBackToOsDetection)
{
    i18n::Init(L"");
    EXPECT_TRUE(&i18n::S() == &i18n::kJa || &i18n::S() == &i18n::kEn || &i18n::S() == &i18n::kZh);
}

// 繁体字の地域には簡体字の文言を出さない。
TEST_F(LocaleTest, OnlySimplifiedChineseRegionsAreChinese)
{
    EXPECT_TRUE(i18n::IsSimplifiedChinese(MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED)));
    EXPECT_TRUE(i18n::IsSimplifiedChinese(MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SINGAPORE)));
    EXPECT_FALSE(i18n::IsSimplifiedChinese(MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL)));
    EXPECT_FALSE(i18n::IsSimplifiedChinese(MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_HONGKONG)));
    EXPECT_FALSE(i18n::IsSimplifiedChinese(MAKELANGID(LANG_JAPANESE, SUBLANG_JAPANESE_JAPAN)));
}

TEST_F(LocaleTest, TextLocaleIsJapaneseOrChinese)
{
    const std::wstring_view loc = i18n::TextLocale();
    EXPECT_TRUE(loc == L"ja-jp" || loc == L"zh-cn" || loc == L"zh-tw" || loc == L"zh-hk") << std::wstring(loc);
}

TEST_F(LocaleTest, JaStringsAreNotEmpty)
{
    const auto& s = i18n::kJa;
    EXPECT_FALSE(s.tooltip_help.empty());
    EXPECT_FALSE(s.tooltip_theme_toggle.empty());
    EXPECT_FALSE(s.tooltip_search.empty());
    EXPECT_FALSE(s.tooltip_file_pane.empty());
    EXPECT_FALSE(s.tooltip_toc_pane.empty());
    EXPECT_FALSE(s.tooltip_minimize.empty());
    EXPECT_FALSE(s.tooltip_maximize.empty());
    EXPECT_FALSE(s.tooltip_restore.empty());
    EXPECT_FALSE(s.tooltip_close.empty());
    EXPECT_FALSE(s.tooltip_pane_close.empty());
    EXPECT_FALSE(s.tooltip_pane_refresh.empty());
    EXPECT_FALSE(s.tooltip_pane_reveal.empty());
    EXPECT_FALSE(s.tooltip_search_prev.empty());
    EXPECT_FALSE(s.tooltip_search_next.empty());
    EXPECT_FALSE(s.tooltip_search_case.empty());
    EXPECT_FALSE(s.tooltip_search_highlight.empty());
    EXPECT_FALSE(s.tooltip_search_close.empty());
    EXPECT_FALSE(s.tooltip_nav_back.empty());
    EXPECT_FALSE(s.tooltip_nav_forward.empty());
    EXPECT_FALSE(s.tooltip_copy.empty());
    EXPECT_FALSE(s.menu_edit_file.empty());
    EXPECT_FALSE(s.menu_copy.empty());
    EXPECT_FALSE(s.menu_dark_mode.empty());
    EXPECT_FALSE(s.menu_file_pane.empty());
    EXPECT_FALSE(s.menu_toc_pane.empty());
    EXPECT_FALSE(s.menu_zoom.empty());
    EXPECT_FALSE(s.pane_header_files.empty());
    EXPECT_FALSE(s.pane_header_toc.empty());
    EXPECT_FALSE(s.menu_reset_window.empty());
    EXPECT_FALSE(s.toast_file_not_found.empty());
    EXPECT_FALSE(s.loading.empty());
}

TEST_F(LocaleTest, EnStringsAreNotEmpty)
{
    const auto& s = i18n::kEn;
    EXPECT_FALSE(s.tooltip_help.empty());
    EXPECT_FALSE(s.tooltip_theme_toggle.empty());
    EXPECT_FALSE(s.tooltip_search.empty());
    EXPECT_FALSE(s.tooltip_file_pane.empty());
    EXPECT_FALSE(s.tooltip_toc_pane.empty());
    EXPECT_FALSE(s.tooltip_minimize.empty());
    EXPECT_FALSE(s.tooltip_maximize.empty());
    EXPECT_FALSE(s.tooltip_restore.empty());
    EXPECT_FALSE(s.tooltip_close.empty());
    EXPECT_FALSE(s.tooltip_pane_close.empty());
    EXPECT_FALSE(s.tooltip_pane_refresh.empty());
    EXPECT_FALSE(s.tooltip_pane_reveal.empty());
    EXPECT_FALSE(s.tooltip_search_prev.empty());
    EXPECT_FALSE(s.tooltip_search_next.empty());
    EXPECT_FALSE(s.tooltip_search_case.empty());
    EXPECT_FALSE(s.tooltip_search_highlight.empty());
    EXPECT_FALSE(s.tooltip_search_close.empty());
    EXPECT_FALSE(s.tooltip_nav_back.empty());
    EXPECT_FALSE(s.tooltip_nav_forward.empty());
    EXPECT_FALSE(s.tooltip_copy.empty());
    EXPECT_FALSE(s.menu_edit_file.empty());
    EXPECT_FALSE(s.menu_copy.empty());
    EXPECT_FALSE(s.menu_dark_mode.empty());
    EXPECT_FALSE(s.menu_file_pane.empty());
    EXPECT_FALSE(s.menu_toc_pane.empty());
    EXPECT_FALSE(s.menu_zoom.empty());
    EXPECT_FALSE(s.pane_header_files.empty());
    EXPECT_FALSE(s.pane_header_toc.empty());
    EXPECT_FALSE(s.menu_reset_window.empty());
    EXPECT_FALSE(s.toast_file_not_found.empty());
    EXPECT_FALSE(s.loading.empty());
}

TEST_F(LocaleTest, ZhStringsAreNotEmpty)
{
    const auto& s = i18n::kZh;
    EXPECT_FALSE(s.tooltip_help.empty());
    EXPECT_FALSE(s.tooltip_theme_toggle.empty());
    EXPECT_FALSE(s.tooltip_search.empty());
    EXPECT_FALSE(s.tooltip_file_pane.empty());
    EXPECT_FALSE(s.tooltip_toc_pane.empty());
    EXPECT_FALSE(s.tooltip_minimize.empty());
    EXPECT_FALSE(s.tooltip_maximize.empty());
    EXPECT_FALSE(s.tooltip_restore.empty());
    EXPECT_FALSE(s.tooltip_close.empty());
    EXPECT_FALSE(s.tooltip_pane_close.empty());
    EXPECT_FALSE(s.tooltip_pane_refresh.empty());
    EXPECT_FALSE(s.tooltip_pane_reveal.empty());
    EXPECT_FALSE(s.tooltip_search_prev.empty());
    EXPECT_FALSE(s.tooltip_search_next.empty());
    EXPECT_FALSE(s.tooltip_search_case.empty());
    EXPECT_FALSE(s.tooltip_search_highlight.empty());
    EXPECT_FALSE(s.tooltip_search_close.empty());
    EXPECT_FALSE(s.tooltip_nav_back.empty());
    EXPECT_FALSE(s.tooltip_nav_forward.empty());
    EXPECT_FALSE(s.tooltip_copy.empty());
    EXPECT_FALSE(s.menu_edit_file.empty());
    EXPECT_FALSE(s.menu_copy.empty());
    EXPECT_FALSE(s.menu_dark_mode.empty());
    EXPECT_FALSE(s.menu_file_pane.empty());
    EXPECT_FALSE(s.menu_toc_pane.empty());
    EXPECT_FALSE(s.menu_zoom.empty());
    EXPECT_FALSE(s.pane_header_files.empty());
    EXPECT_FALSE(s.pane_header_toc.empty());
    EXPECT_FALSE(s.menu_reset_window.empty());
    EXPECT_FALSE(s.toast_file_not_found.empty());
    EXPECT_FALSE(s.loading.empty());
}

TEST_F(LocaleTest, HelpResourceIdsAreDistinct)
{
    EXPECT_NE(i18n::kJa.help_resource_id, i18n::kEn.help_resource_id);
    EXPECT_NE(i18n::kJa.help_resource_id, i18n::kZh.help_resource_id);
    EXPECT_NE(i18n::kEn.help_resource_id, i18n::kZh.help_resource_id);
}

TEST_F(LocaleTest, SReturnsCorrectTableAfterInit)
{
    i18n::Init(L"en");
    EXPECT_EQ(i18n::S().tooltip_help, i18n::kEn.tooltip_help);

    i18n::Init(L"ja");
    EXPECT_EQ(i18n::S().tooltip_help, i18n::kJa.tooltip_help);
}
