#include <gtest/gtest.h>
#include "tooltip.h"

// Tooltip クラス本体は Win32 HWND 依存のためテスト対象外。
// ここでは値型 TooltipTarget の等価性と IsEmpty のみを検証する。

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

TEST(TooltipTarget, EqualityDefault)
{
    TooltipTarget a;
    TooltipTarget b;
    EXPECT_TRUE(a == b);
}

TEST(TooltipTarget, EqualitySameZoneAndText)
{
    TooltipTarget a{ TooltipTarget::Zone::FilePaneItem, L"README.md" };
    TooltipTarget b{ TooltipTarget::Zone::FilePaneItem, L"README.md" };
    EXPECT_TRUE(a == b);
}

TEST(TooltipTarget, InequalityDifferentZone)
{
    TooltipTarget a{ TooltipTarget::Zone::MdLink, L"same" };
    TooltipTarget b{ TooltipTarget::Zone::MdImage, L"same" };
    EXPECT_FALSE(a == b);
}

TEST(TooltipTarget, InequalityDifferentText)
{
    TooltipTarget a{ TooltipTarget::Zone::TitleBarButton, L"Close" };
    TooltipTarget b{ TooltipTarget::Zone::TitleBarButton, L"Minimize" };
    EXPECT_FALSE(a == b);
}

TEST(TooltipTarget, AssignmentPreservesEquality)
{
    TooltipTarget a{ TooltipTarget::Zone::SaveButton, L"save.png" };
    TooltipTarget b;
    b = a;
    EXPECT_TRUE(a == b);
    EXPECT_EQ(b.zone, TooltipTarget::Zone::SaveButton);
    EXPECT_EQ(b.text, L"save.png");
}
