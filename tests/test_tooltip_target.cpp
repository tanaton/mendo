#include <gtest/gtest.h>
#include "tooltip.h"

// Tooltip クラス本体は Win32 HWND 依存のためテスト対象外。
// ここでは値型 TooltipTarget の IsEmpty と同一判定、MakeTooltip のみを検証する。

// 空かどうかは zone だけで決まり、key は見ない。既定は空。
TEST(TooltipTarget, IsEmptyFollowsZoneOnly)
{
    EXPECT_EQ(TooltipTarget{}.zone, TooltipTarget::Zone::None);
    EXPECT_TRUE(TooltipTarget{}.IsEmpty());
    EXPECT_FALSE((TooltipTarget{ TooltipTarget::Zone::OverlayButton, 0 }).IsEmpty());
    EXPECT_TRUE((TooltipTarget{ TooltipTarget::Zone::None, 5 }).IsEmpty());
}

TEST(TooltipTarget, DifferentKeyOrZoneIsNotSameTarget)
{
    const TooltipTarget a{ TooltipTarget::Zone::MdLink, 1 };
    EXPECT_EQ(a, (TooltipTarget{ TooltipTarget::Zone::MdLink, 1 }));
    EXPECT_NE(a, (TooltipTarget{ TooltipTarget::Zone::MdLink, 2 }));
    EXPECT_NE(a, (TooltipTarget{ TooltipTarget::Zone::MdImage, 1 }));
}

// ホバー移動ごとに表示文字列を作らないため、同じ対象なら文言を作らない (同一判定は zone と key だけ)。
TEST(TooltipTarget, MakeTooltipBuildsTextOnlyWhenTargetChanges)
{
    const TooltipHover current = MakeTooltip({}, TooltipTarget::Zone::NavButton, 1, L"back");
    EXPECT_EQ(current.text, L"back");

    int fills = 0;
    const auto same = MakeTooltip(current.target, TooltipTarget::Zone::NavButton, 1, [&](std::wstring&) { ++fills; });
    EXPECT_EQ(fills, 0);
    EXPECT_EQ(same.target, current.target);
    EXPECT_TRUE(same.text.empty());

    const auto other = MakeTooltip(current.target, TooltipTarget::Zone::NavButton, 2, L"forward");
    EXPECT_EQ(other.text, L"forward");
}

// 該当ボタンが無いときは文言が空になるので、ゾーンを残さず非表示にする。
TEST(TooltipTarget, MakeTooltipWithEmptyTextIsEmpty)
{
    EXPECT_TRUE(MakeTooltip({}, TooltipTarget::Zone::NavButton, 0, L"").target.IsEmpty());
}
