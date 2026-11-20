#include <gtest/gtest.h>
#include "settings_dialog.h"
#include "test_helpers.h"
#include "theme.h"
#include <dwrite.h>
#include <optional>
#include <string>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using namespace settings_dialog_constants;

TEST(SettingsDialogRowsTest, FirstRowFollowsOsThenAllLanguages)
{
    EXPECT_EQ(SettingsDialog::LanguageOfRow(0), nullptr);
    for (size_t i = 0; i < std::size(i18n::kAllStrings); ++i) {
        EXPECT_EQ(SettingsDialog::LanguageOfRow(i + 1), i18n::kAllStrings[i]);
    }
}

// CreateTextFormats / ComputeLayout は DWrite だけを使う。D2D は Show で描くときにしか要らない。
class SettingsDialogLayoutTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        dialog_.dwrite_factory = SharedDWriteFactory();
        ASSERT_NE(dialog_.dwrite_factory, nullptr);
        dialog_.theme = &theme_;
        dialog_.on_language = [this](const i18n::Strings* chosen) {
            ++language_calls_;
            i18n::SetLanguage(chosen);
        };
        dialog_.Rebuild();
    }

    std::optional<size_t> HitAt(const DipRect& rect) const
    {
        const auto [x, y] = RectCenter(rect);
        return dialog_.HitTest(x, y);
    }

    // 選んだ言語は on_language が切り替えるので、テストごとに日本語から始めて元に戻す。
    ScopedLanguage language_{ &i18n::kJa };
    Theme theme_ = GetLightTheme();
    SettingsDialog dialog_;
    int language_calls_ = 0;
};

TEST_F(SettingsDialogLayoutTest, RowsStackInsideDialog)
{
    const auto& l = dialog_.layout;
    EXPECT_GE(dialog_.width, MIN_WIDTH);
    EXPECT_FLOAT_EQ(l.languages.front().top, l.language_heading.bottom);
    for (size_t row = 1; row < l.languages.size(); ++row) {
        EXPECT_FLOAT_EQ(l.languages[row].top, l.languages[row - 1].bottom);
    }
    EXPECT_FLOAT_EQ(l.separator.top, l.languages.back().bottom);
    EXPECT_FLOAT_EQ(l.version.top, l.separator.bottom);
    EXPECT_LT(l.version.bottom, dialog_.height);
}

TEST_F(SettingsDialogLayoutTest, WidthFitsLongestTextInEveryLanguage)
{
    for (const i18n::Strings* language : i18n::kAllStrings) {
        SCOPED_TRACE(std::wstring(language->lang_key));
        i18n::SetLanguage(language);
        dialog_.Rebuild();
        const float system_w = dialog_.Measure(dialog_.system_language_text, dialog_.fmt_text.Get());
        EXPECT_LE(PAD_X + CHECK_WIDTH + system_w, dialog_.width - PAD_X + 0.5f);
        EXPECT_LE(dialog_.Measure(SettingsDialog::VersionText(), dialog_.fmt_text.Get()), dialog_.layout.version.right - dialog_.layout.version.left + 0.5f);
    }
}

TEST_F(SettingsDialogLayoutTest, HitTestFindsEachControl)
{
    const auto& l = dialog_.layout;
    // タイトル行に押せるものは無い (範囲外のクリックで閉じる)。
    EXPECT_EQ(dialog_.HitTest(dialog_.width - 4.0f, 4.0f), std::nullopt);
    for (size_t row = 0; row < l.languages.size(); ++row) {
        EXPECT_EQ(HitAt(l.languages[row]), row);
    }
    EXPECT_EQ(HitAt(l.language_heading), std::nullopt);
    // バージョンは見るだけ。
    EXPECT_EQ(HitAt(l.separator), std::nullopt);
    EXPECT_EQ(HitAt(l.version), std::nullopt);
}

TEST_F(SettingsDialogLayoutTest, ChoosingLanguageSwitchesAndRelayouts)
{
    const size_t ko_row = std::size(i18n::kAllStrings);
    ASSERT_EQ(SettingsDialog::LanguageOfRow(ko_row), &i18n::kKo);

    dialog_.Activate(ko_row);
    EXPECT_EQ(language_calls_, 1);
    EXPECT_EQ(&i18n::S(), &i18n::kKo);
    EXPECT_TRUE(dialog_.system_language_text.starts_with(i18n::kKo.settings_language_system));

    // 選び済みの言語を押しても呼ばない。
    dialog_.Activate(ko_row);
    EXPECT_EQ(language_calls_, 1);

    dialog_.Activate(0);
    EXPECT_EQ(language_calls_, 2);
    EXPECT_EQ(i18n::ChosenLanguage(), nullptr);
}

// 範囲外を押したら閉じる。中を押しても閉じない。
TEST_F(SettingsDialogLayoutTest, ClickOutsideCloses)
{
    const auto click = [this](int x, int y) { dialog_.HandleMessage(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y)); };
    const auto [rx, ry] = RectCenter(dialog_.layout.languages.back());
    click(static_cast<int>(rx), static_cast<int>(ry));
    EXPECT_FALSE(dialog_.done);
    EXPECT_EQ(language_calls_, 1);

    click(-5, 10);
    EXPECT_TRUE(dialog_.done);

    dialog_.done = false;
    click(static_cast<int>(dialog_.width) + 5, 10);
    EXPECT_TRUE(dialog_.done);
}
