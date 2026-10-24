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
        i18n::g_chosen = nullptr;
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

TEST_F(LocaleTest, ExplicitZhHant)
{
    i18n::Init(L"zh-hant");
    EXPECT_EQ(&i18n::S(), &i18n::kZhHant);
}

TEST_F(LocaleTest, ExplicitKo)
{
    i18n::Init(L"ko");
    EXPECT_EQ(&i18n::S(), &i18n::kKo);
}

// 終了時に保存したキーで次回も同じ言語になる。
TEST_F(LocaleTest, LangKeyRoundTrips)
{
    for (const wchar_t* key : { L"ja", L"en", L"zh", L"zh-hant", L"ko" }) {
        i18n::Init(key);
        EXPECT_EQ(i18n::ConfigLangKey(), key);
    }
}

// OS に合わせる設定は空で保存し、次回も OS の表示言語で決める (OS の言語を変えたら追従する)。
TEST_F(LocaleTest, FollowingOsSavesEmptyKey)
{
    for (const wchar_t* key : { L"", L"fr" }) {
        i18n::Init(key);
        EXPECT_EQ(i18n::ChosenLanguage(), nullptr);
        EXPECT_TRUE(i18n::ConfigLangKey().empty());
    }
}

// 設定ダイアログで選び直すと、表示中の文言と保存する値がすぐ変わる。
TEST_F(LocaleTest, SetLanguageSwitchesImmediately)
{
    i18n::Init(L"ja");
    i18n::SetLanguage(&i18n::kKo);
    EXPECT_EQ(&i18n::S(), &i18n::kKo);
    EXPECT_EQ(i18n::ConfigLangKey(), L"ko");

    i18n::SetLanguage(nullptr);
    EXPECT_EQ(&i18n::S(), &i18n::SystemStrings());
    EXPECT_TRUE(i18n::ConfigLangKey().empty());
}

TEST_F(LocaleTest, UnknownLangFallsBackToOsDetection)
{
    i18n::Init(L"fr");
    EXPECT_EQ(&i18n::S(), &i18n::SystemStrings());
}

TEST_F(LocaleTest, EmptyLangFallsBackToOsDetection)
{
    i18n::Init(L"");
    EXPECT_EQ(&i18n::S(), &i18n::SystemStrings());
}

TEST_F(LocaleTest, UiLanguagePicksStrings)
{
    struct Case {
        LANGID langid;
        const i18n::Strings* expected;
    };
    const Case cases[] = {
        { MAKELANGID(LANG_JAPANESE, SUBLANG_JAPANESE_JAPAN), &i18n::kJa },
        { MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), &i18n::kEn },
        { MAKELANGID(LANG_FRENCH, SUBLANG_FRENCH), &i18n::kEn },
        { MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED), &i18n::kZh },
        { MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SINGAPORE), &i18n::kZh },
        { LANG_CHINESE_SIMPLIFIED, &i18n::kZh },
        { MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL), &i18n::kZhHant },
        { MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_HONGKONG), &i18n::kZhHant },
        { MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_MACAU), &i18n::kZhHant },
        { LANG_CHINESE_TRADITIONAL, &i18n::kZhHant },
        { MAKELANGID(LANG_KOREAN, SUBLANG_KOREAN), &i18n::kKo },
    };
    for (const auto& c : cases) {
        EXPECT_EQ(i18n::StringsForUiLanguage(c.langid), c.expected) << std::hex << c.langid;
    }
}

TEST_F(LocaleTest, UiLanguagePicksTextLocale)
{
    struct Case {
        LANGID langid;
        std::wstring_view expected;
    };
    const Case cases[] = {
        { MAKELANGID(LANG_JAPANESE, SUBLANG_JAPANESE_JAPAN), L"ja-jp" },
        { MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), L"ja-jp" },
        { MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED), L"zh-cn" },
        { MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL), L"zh-tw" },
        { LANG_CHINESE_TRADITIONAL, L"zh-tw" },
        { MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_HONGKONG), L"zh-hk" },
        { MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_MACAU), L"zh-hk" },
        { MAKELANGID(LANG_KOREAN, SUBLANG_KOREAN), L"ko-kr" },
    };
    for (const auto& c : cases) {
        EXPECT_EQ(i18n::TextLocaleForUiLanguage(c.langid), c.expected) << std::hex << c.langid;
    }
}

TEST_F(LocaleTest, TextLocaleFollowsOsUiLanguage)
{
    EXPECT_EQ(std::wstring_view(i18n::TextLocale()), i18n::TextLocaleForUiLanguage(GetUserDefaultUILanguage()));
}

TEST_F(LocaleTest, StringsAreNotEmpty)
{
    for (const i18n::Strings* table : i18n::kAllStrings) {
        SCOPED_TRACE(std::wstring(table->lang_key));
        const auto& s = *table;
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
        EXPECT_FALSE(s.menu_title.empty());
        EXPECT_FALSE(s.pane_header_files.empty());
        EXPECT_FALSE(s.pane_header_toc.empty());
        EXPECT_FALSE(s.menu_reset_window.empty());
        EXPECT_FALSE(s.toast_file_not_found.empty());
        EXPECT_FALSE(s.loading.empty());
        EXPECT_FALSE(s.settings_title.empty());
        EXPECT_FALSE(s.settings_language.empty());
        EXPECT_FALSE(s.settings_language_system.empty());
        EXPECT_FALSE(s.settings_diagram_cache.empty());
        EXPECT_FALSE(s.settings_clear_cache.empty());
        EXPECT_FALSE(s.lang_name.empty());
        EXPECT_NE(s.lang_locale, nullptr);
    }
}

TEST_F(LocaleTest, HelpResourceIdsAndLangKeysAreDistinct)
{
    for (const i18n::Strings* a : i18n::kAllStrings) {
        for (const i18n::Strings* b : i18n::kAllStrings) {
            if (a != b) {
                EXPECT_NE(a->help_resource_id, b->help_resource_id);
                EXPECT_NE(a->lang_key, b->lang_key);
            }
        }
    }
}

TEST_F(LocaleTest, SReturnsCorrectTableAfterInit)
{
    i18n::Init(L"en");
    EXPECT_EQ(i18n::S().tooltip_help, i18n::kEn.tooltip_help);

    i18n::Init(L"ja");
    EXPECT_EQ(i18n::S().tooltip_help, i18n::kJa.tooltip_help);
}
