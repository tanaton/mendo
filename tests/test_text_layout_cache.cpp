// TextLayoutCache の上限超過時の整理。実 DirectWrite でレイアウトを作るため DWriteTestBase を使う。
#include <gtest/gtest.h>
#include "dwrite_test_base.h"
#include "command_generator.h"
#include <format>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kOverLimit = static_cast<int>(TextLayoutCache::kMaxEntries) + 88;
constexpr D2D1_SIZE_F kBox{ 0.0f, 0.0f };

class TextLayoutCacheTest : public DWriteTestBase {
protected:
    TextLayoutCache cache_;
    ComPtr<IDWriteTextFormat> fmt_;

    void SetUp() override
    {
        DWriteTestBase::SetUp();
        fmt_ = CreateThemeFormat(theme_.font_size_body);
        ASSERT_NE(fmt_, nullptr);
        cache_.SetFactory(dwrite_factory_.Get());
    }

    static std::wstring Item(int i)
    {
        return std::format(L"item {}", i);
    }

    std::vector<IDWriteTextLayout*> FillOverLimit()
    {
        std::vector<IDWriteTextLayout*> layouts;
        for (int i = 0; i < kOverLimit; ++i) {
            layouts.push_back(cache_.Get(fmt_.Get(), Item(i), kBox));
        }
        return layouts;
    }
};

} // namespace

// 上限を超えても、直前のフレームで使ったレイアウトは作り直さない。
TEST_F(TextLayoutCacheTest, KeepsLayoutsUsedInPreviousFrame)
{
    cache_.BeginFrame();
    const auto layouts = FillOverLimit();
    cache_.BeginFrame();

    for (int i = 0; i < kOverLimit; ++i) {
        EXPECT_EQ(cache_.Get(fmt_.Get(), Item(i), kBox), layouts[i]) << "i=" << i;
    }
}

// 上限を超えたら、直前のフレームで使わなかったものだけ捨てる。
TEST_F(TextLayoutCacheTest, DropsLayoutsUnusedInPreviousFrame)
{
    cache_.BeginFrame();
    const auto layouts = FillOverLimit();
    cache_.BeginFrame();
    ASSERT_EQ(cache_.Get(fmt_.Get(), Item(0), kBox), layouts[0]);
    cache_.BeginFrame();

    EXPECT_EQ(cache_.TextOf(layouts[0]), Item(0));
    EXPECT_TRUE(cache_.TextOf(layouts[1]).empty());
    EXPECT_TRUE(cache_.TextOf(layouts[kOverLimit - 1]).empty());
}

// 上限以下なら前のフレームで使わなかったものも残す。
TEST_F(TextLayoutCacheTest, KeepsEverythingBelowLimit)
{
    cache_.BeginFrame();
    auto* const first = cache_.Get(fmt_.Get(), Item(0), kBox);
    cache_.BeginFrame();
    cache_.BeginFrame();

    EXPECT_EQ(cache_.TextOf(first), Item(0));
}

// 同じ書式でも、フォントを明示したレイアウトとしていないレイアウトを取り違えない。
TEST_F(TextLayoutCacheTest, KeepsLayoutsApartByFallbackFonts)
{
    const FallbackFontMap& fonts = measurer_.BodyFonts();
    auto* const plain = cache_.Get(fmt_.Get(), L"漢字", kBox);
    auto* const hinted = cache_.Get(fmt_.Get(), L"漢字", kBox, {}, &fonts);

    EXPECT_NE(plain, hinted);
    EXPECT_EQ(cache_.Get(fmt_.Get(), L"漢字", kBox, {}, &fonts), hinted);
}

// 長いファイル名でも、タイトルは左右のボタンの間に収める。
TEST_F(TextLayoutCacheTest, LongTitleStaysBetweenTheButtons)
{
    TitleBar bar;
    bar.UpdateLayout(800.0f);
    const auto title_fmt = CreateTextFormat(dwrite_factory_.Get(), { .family = theme_.font_family.c_str(), .size = 14.0f, .text_align = DWRITE_TEXT_ALIGNMENT_CENTER, .para_align = DWRITE_PARAGRAPH_ALIGNMENT_CENTER, .no_wrap = true, .ellipsis = true });
    ASSERT_NE(title_fmt, nullptr);
    const std::wstring title(300, L'長');
    DrawCommandList cmds;
    chrome::EmitTitleBar({ cmds, cache_ }, nullptr, title_fmt.Get(), nullptr, false, { .bar = bar, .title_text = title, .window_width = 800.0f });

    const DrawTextLayoutCmd* text = nullptr;
    for (const auto& cmd : cmds) {
        if (const auto* t = std::get_if<DrawTextLayoutCmd>(&cmd)) {
            text = t;
        }
    }
    ASSERT_NE(text, nullptr);
    DWRITE_TEXT_METRICS metrics{};
    ASSERT_HRESULT_SUCCEEDED(text->layout->GetMetrics(&metrics));
    const auto& rect = bar.GetTitleTextRect();
    EXPECT_GE(text->origin.x + metrics.left, rect.left - 0.5f);
    EXPECT_LE(text->origin.x + metrics.left + metrics.width, rect.right + 0.5f);
}
