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
