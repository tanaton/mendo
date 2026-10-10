#include <gtest/gtest.h>
#include "tooltip.h"

// Tooltip クラス本体は Win32 HWND 依存のためテスト対象外。
// ここでは値型 TooltipTarget の IsEmpty と同一判定、MakeTooltip のみを検証する。

TEST(TooltipTarget, DefaultConstructedIsEmpty)
{
    TooltipTarget t;
    EXPECT_EQ(t.zone, TooltipTarget::Zone::None);
    EXPECT_TRUE(t.text.empty());
    EXPECT_TRUE(t.IsEmpty());
}

TEST(TooltipTarget, ConstructedWithZoneIsNotEmpty)
{
    TooltipTarget t{ TooltipTarget::Zone::CopyButton, 0 };
    EXPECT_EQ(t.zone, TooltipTarget::Zone::CopyButton);
    EXPECT_FALSE(t.IsEmpty());
}

TEST(TooltipTarget, IsEmptyIgnoresKey)
{
    TooltipTarget t{ TooltipTarget::Zone::None, 5 };
    EXPECT_TRUE(t.IsEmpty());
}

// ホバー移動ごとに表示文字列を作らないため、同一判定は zone と key だけで行う。
TEST(TooltipTarget, SameTargetIgnoresText)
{
    TooltipTarget built{ TooltipTarget::Zone::FilePaneItem, 3 };
    built.text = L"dir/a.md";
    const TooltipTarget lazy{ TooltipTarget::Zone::FilePaneItem, 3 };
    EXPECT_TRUE(built.SameTarget(lazy));
}

TEST(TooltipTarget, DifferentKeyOrZoneIsNotSameTarget)
{
    const TooltipTarget a{ TooltipTarget::Zone::MdLink, 1 };
    EXPECT_FALSE(a.SameTarget({ TooltipTarget::Zone::MdLink, 2 }));
    EXPECT_FALSE(a.SameTarget({ TooltipTarget::Zone::MdImage, 1 }));
}

TEST(TooltipTarget, MakeTooltipBuildsTextOnlyWhenTargetChanges)
{
    const TooltipTarget current = MakeTooltip({}, TooltipTarget::Zone::NavButton, 1, L"back");
    EXPECT_EQ(current.text, L"back");

    int fills = 0;
    const auto same = MakeTooltip(current, TooltipTarget::Zone::NavButton, 1, [&](std::pmr::wstring&) { ++fills; });
    EXPECT_EQ(fills, 0);
    EXPECT_TRUE(same.SameTarget(current));

    const auto other = MakeTooltip(current, TooltipTarget::Zone::NavButton, 2, L"forward");
    EXPECT_EQ(other.text, L"forward");
}
