#include <gtest/gtest.h>
#include "i18n.h"
#include "test_helpers.h"
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace {

// 指示付き初期化で書き忘れたフィールドは空文字になるので、全フィールドを全言語で調べる。
constexpr std::wstring_view i18n::Strings::* kStringFields[] = {
    &i18n::Strings::tooltip_open_file,
    &i18n::Strings::tooltip_help,
    &i18n::Strings::tooltip_theme_toggle,
    &i18n::Strings::tooltip_search,
    &i18n::Strings::tooltip_file_pane,
    &i18n::Strings::tooltip_toc_pane,
    &i18n::Strings::tooltip_minimize,
    &i18n::Strings::tooltip_maximize,
    &i18n::Strings::tooltip_restore,
    &i18n::Strings::tooltip_close,
    &i18n::Strings::tooltip_pane_refresh,
    &i18n::Strings::tooltip_pane_reveal,
    &i18n::Strings::tooltip_search_prev,
    &i18n::Strings::tooltip_search_next,
    &i18n::Strings::tooltip_search_case,
    &i18n::Strings::tooltip_search_highlight,
    &i18n::Strings::tooltip_search_close,
    &i18n::Strings::tooltip_nav_back,
    &i18n::Strings::tooltip_nav_forward,
    &i18n::Strings::gesture_back,
    &i18n::Strings::gesture_forward,
    &i18n::Strings::tooltip_save_image,
    &i18n::Strings::tooltip_copy_diagram,
    &i18n::Strings::tooltip_view_diagram,
    &i18n::Strings::viewer_title,
    &i18n::Strings::viewer_zoom_in,
    &i18n::Strings::viewer_zoom_out,
    &i18n::Strings::viewer_fit,
    &i18n::Strings::menu_new_window,
    &i18n::Strings::menu_open_in_new_window,
    &i18n::Strings::menu_edit_file,
    &i18n::Strings::menu_copy,
    &i18n::Strings::copy_formatted,
    &i18n::Strings::menu_dark_mode,
    &i18n::Strings::menu_file_pane,
    &i18n::Strings::menu_toc_pane,
    &i18n::Strings::menu_zoom,
    &i18n::Strings::menu_title,
    &i18n::Strings::pane_header_files,
    &i18n::Strings::pane_header_toc,
    &i18n::Strings::menu_reset_window,
    &i18n::Strings::toast_file_not_found,
    &i18n::Strings::toast_file_too_large,
    &i18n::Strings::toast_file_read_failed,
    &i18n::Strings::toast_image_saved,
    &i18n::Strings::toast_image_save_failed,
    &i18n::Strings::toast_diagram_copying,
    &i18n::Strings::toast_diagram_copied,
    &i18n::Strings::toast_diagram_copy_failed,
    &i18n::Strings::toast_diagram_view_failed,
    &i18n::Strings::loading,
    &i18n::Strings::diagram_error,
    &i18n::Strings::diagram_unsupported,
    &i18n::Strings::settings_title,
    &i18n::Strings::settings_language,
    &i18n::Strings::settings_language_system,
    &i18n::Strings::settings_diagram_cache,
    &i18n::Strings::settings_clear_cache,
    &i18n::Strings::lang_key,
    &i18n::Strings::lang_name,
};
// Strings にフィールドを足したら上の表にも足す。残りは help_resource_id と lang_locale。
static_assert(sizeof(i18n::Strings) == sizeof(kStringFields) / sizeof(kStringFields[0]) * sizeof(std::wstring_view) + 2 * sizeof(void*));

} // namespace

class LocaleTest : public ::testing::Test {
protected:
    ScopedLanguage restore_language_;
};

// 設定のキーでその言語の表を選び、終了時に同じキーを保存するので次回も同じ言語になる。
TEST_F(LocaleTest, ExplicitLanguagePicksItsTableAndKeyRoundTrips)
{
    const std::pair<const wchar_t*, const i18n::Strings*> kCases[] = {
        { L"ja", &i18n::kJa },
        { L"en", &i18n::kEn },
        { L"zh", &i18n::kZh },
        { L"zh-hant", &i18n::kZhHant },
        { L"ko", &i18n::kKo },
    };
    for (const auto& [key, table] : kCases) {
        SCOPED_TRACE(std::wstring(key));
        i18n::Init(key);
        EXPECT_EQ(&i18n::S(), table);
        EXPECT_EQ(i18n::ChosenLanguage(), table);
        EXPECT_EQ(i18n::ConfigLangKey(), key);
    }
}

// 空や知らないキーは OS の表示言語に合わせる。保存は空にし、次回も OS の表示言語で決める (OS の言語を変えたら追従する)。
TEST_F(LocaleTest, FollowingOsSavesEmptyKey)
{
    for (const wchar_t* key : { L"", L"fr" }) {
        SCOPED_TRACE(std::wstring(key));
        i18n::Init(key);
        EXPECT_EQ(&i18n::S(), &i18n::SystemStrings());
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
        for (const auto [index, field] : kStringFields | std::views::enumerate) {
            EXPECT_FALSE((table->*field).empty()) << "field " << index;
        }
        EXPECT_NE(table->lang_locale, nullptr);
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
