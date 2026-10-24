#include <gtest/gtest.h>
#include "settings_dialog_impl.h"
#include "theme.h"
#include <dwrite.h>
#include <string>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using namespace settings_dialog_constants;
using Hit = SettingsDialog::Impl::Hit;
using HitKind = SettingsDialog::Impl::HitKind;

namespace {

DipPoint Center(const DipRect& r)
{
    return { (r.left + r.right) / 2.0f, (r.top + r.bottom) / 2.0f };
}

} // namespace

TEST(FormatByteSizeTest, PicksUnitAndDigits)
{
    EXPECT_EQ(FormatByteSize(0), L"0 B");
    EXPECT_EQ(FormatByteSize(1023), L"1023 B");
    EXPECT_EQ(FormatByteSize(1024), L"1.0 KB");
    EXPECT_EQ(FormatByteSize(1536), L"1.5 KB");
    EXPECT_EQ(FormatByteSize(512ULL * 1024), L"512 KB");
    EXPECT_EQ(FormatByteSize(12ULL * 1024 * 1024 + 300 * 1024), L"12.3 MB");
    EXPECT_EQ(FormatByteSize(1024ULL * 1024 * 1024), L"1.0 GB");
}

TEST(SettingsDialogRowsTest, FirstRowFollowsOsThenAllLanguages)
{
    EXPECT_EQ(SettingsDialog::Impl::LanguageOfRow(0), nullptr);
    for (size_t i = 0; i < std::size(i18n::kAllStrings); ++i) {
        EXPECT_EQ(SettingsDialog::Impl::LanguageOfRow(i + 1), i18n::kAllStrings[i]);
    }
}

// CreateTextFormats / ComputeLayout は DWrite だけを使う。D2D は Show で描くときにしか要らない。
class SettingsDialogLayoutTest : public ::testing::Test {
protected:
    static inline ComPtr<IDWriteFactory> dwrite_;

    static void SetUpTestSuite()
    {
        const HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
        ASSERT_TRUE(SUCCEEDED(hr));
    }

    static void TearDownTestSuite()
    {
        dwrite_.Reset();
    }

    void SetUp() override
    {
        i18n::SetLanguage(&i18n::kJa);
        dialog_.dwrite_factory = dwrite_.Get();
        dialog_.theme = &theme_;
        dialog_.cache_bytes = 1024 * 1024;
        dialog_.on_language = [this](const i18n::Strings* chosen) {
            ++language_calls_;
            i18n::SetLanguage(chosen);
        };
        dialog_.on_clear_cache = [this] { ++clear_calls_; };
        dialog_.Rebuild();
    }

    void TearDown() override
    {
        i18n::SetLanguage(&i18n::kJa);
    }

    Hit HitAt(const DipRect& rect) const
    {
        const auto [x, y] = Center(rect);
        return dialog_.HitTest(x, y);
    }

    Theme theme_ = GetLightTheme();
    SettingsDialog::Impl dialog_;
    int language_calls_ = 0;
    int clear_calls_ = 0;
};

TEST_F(SettingsDialogLayoutTest, RowsStackInsideDialog)
{
    const auto& l = dialog_.layout;
    EXPECT_GE(dialog_.width, MIN_WIDTH);
    EXPECT_FLOAT_EQ(l.languages.front().top, l.language_heading.bottom);
    for (size_t row = 1; row < l.languages.size(); ++row) {
        EXPECT_FLOAT_EQ(l.languages[row].top, l.languages[row - 1].bottom);
    }
    EXPECT_GT(l.separator_y, l.languages.back().bottom);
    EXPECT_GT(l.cache_heading.top, l.separator_y);
    EXPECT_LE(l.clear.right, dialog_.width - PAD_X);
    EXPECT_LT(l.cache_size.right, l.clear.left);
    EXPECT_LT(l.clear.bottom, dialog_.height);
}

// どの言語の表示中でも、一番長い行がダイアログの幅に収まる。
TEST_F(SettingsDialogLayoutTest, WidthFitsLongestTextInEveryLanguage)
{
    for (const i18n::Strings* language : i18n::kAllStrings) {
        SCOPED_TRACE(std::wstring(language->lang_key));
        i18n::SetLanguage(language);
        dialog_.Rebuild();
        const float system_w = dialog_.Measure(dialog_.system_language_text, dialog_.fmt_text.Get());
        EXPECT_LE(PAD_X + CHECK_WIDTH + system_w, dialog_.width - PAD_X + 0.5f);
        const float button_w = dialog_.Measure(language->settings_clear_cache, dialog_.fmt_text.Get());
        EXPECT_LE(button_w, dialog_.layout.clear.right - dialog_.layout.clear.left);
    }
}

TEST_F(SettingsDialogLayoutTest, HitTestFindsEachControl)
{
    const auto& l = dialog_.layout;
    // タイトル行に押せるものは無い (範囲外のクリックで閉じる)。
    EXPECT_EQ(dialog_.HitTest(dialog_.width - 4.0f, 4.0f), Hit{});
    for (size_t row = 0; row < l.languages.size(); ++row) {
        EXPECT_EQ(HitAt(l.languages[row]), (Hit{ HitKind::Language, row }));
    }
    EXPECT_EQ(HitAt(l.clear), (Hit{ HitKind::ClearCache }));
    EXPECT_EQ(HitAt(l.language_heading), Hit{});
    EXPECT_EQ(HitAt(l.cache_size), Hit{});
}

TEST_F(SettingsDialogLayoutTest, EmptyCacheCannotBeCleared)
{
    dialog_.cache_bytes = 0;
    EXPECT_EQ(HitAt(dialog_.layout.clear), Hit{});
}

TEST_F(SettingsDialogLayoutTest, ChoosingLanguageSwitchesAndRelayouts)
{
    const size_t ko_row = std::size(i18n::kAllStrings);
    ASSERT_EQ(SettingsDialog::Impl::LanguageOfRow(ko_row), &i18n::kKo);

    dialog_.Activate({ HitKind::Language, ko_row });
    EXPECT_EQ(language_calls_, 1);
    EXPECT_EQ(&i18n::S(), &i18n::kKo);
    EXPECT_TRUE(dialog_.system_language_text.starts_with(i18n::kKo.settings_language_system));

    // 選び済みの言語を押しても呼ばない。
    dialog_.Activate({ HitKind::Language, ko_row });
    EXPECT_EQ(language_calls_, 1);

    dialog_.Activate({ HitKind::Language, 0 });
    EXPECT_EQ(language_calls_, 2);
    EXPECT_EQ(i18n::ChosenLanguage(), nullptr);
}

TEST_F(SettingsDialogLayoutTest, ClearingCacheDisablesButton)
{
    dialog_.Activate({ HitKind::ClearCache });
    EXPECT_EQ(clear_calls_, 1);
    EXPECT_EQ(dialog_.cache_size_text, L"0 B");
    EXPECT_FALSE(dialog_.CanClearCache());
}

// 範囲外を押したら閉じる。中を押しても閉じない。
TEST_F(SettingsDialogLayoutTest, ClickOutsideCloses)
{
    const auto click = [this](int x, int y) { dialog_.HandleMessage(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y)); };
    const auto [rx, ry] = Center(dialog_.layout.languages.back());
    click(static_cast<int>(rx), static_cast<int>(ry));
    EXPECT_FALSE(dialog_.done);
    EXPECT_EQ(language_calls_, 1);

    click(-5, 10);
    EXPECT_TRUE(dialog_.done);

    dialog_.done = false;
    click(static_cast<int>(dialog_.width) + 5, 10);
    EXPECT_TRUE(dialog_.done);
}
