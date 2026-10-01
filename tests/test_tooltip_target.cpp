#include <gtest/gtest.h>
#include "tooltip.h"

// Tooltip クラス本体は Win32 HWND 依存のためテスト対象外。
// ここでは値型 TooltipTarget の IsEmpty のみを検証する (operator== は = default)。

TEST(TooltipTarget, DefaultConstructedIsEmpty)
{
    TooltipTarget t;
    EXPECT_EQ(t.zone, TooltipTarget::Zone::None);
    EXPECT_TRUE(t.text.empty());
    EXPECT_TRUE(t.IsEmpty());
}

TEST(TooltipTarget, ConstructedWithZoneAndTextIsNotEmpty)
{
    TooltipTarget t{ TooltipTarget::Zone::MdLink, L"https://example.com" };
    EXPECT_EQ(t.zone, TooltipTarget::Zone::MdLink);
    EXPECT_EQ(t.text, L"https://example.com");
    EXPECT_FALSE(t.IsEmpty());
}

TEST(TooltipTarget, IsEmptyIgnoresText)
{
    TooltipTarget t{ TooltipTarget::Zone::None, L"still empty" };
    EXPECT_TRUE(t.IsEmpty());
}

TEST(TooltipTarget, IsEmptyFalseForNonNoneZoneWithEmptyText)
{
    TooltipTarget t{ TooltipTarget::Zone::CopyButton, L"" };
    EXPECT_FALSE(t.IsEmpty());
}
