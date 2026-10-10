#include <gtest/gtest.h>
#include "last_hover_pos.h"

TEST(LastHoverPos, RepeatsOnlyForSameCoordinate)
{
    LastHoverPos last;
    EXPECT_FALSE(last.IsRepeat(10, 20));
    EXPECT_TRUE(last.IsRepeat(10, 20));
    EXPECT_FALSE(last.IsRepeat(11, 20));
}

TEST(LastHoverPos, ResetReevaluatesSameCoordinate)
{
    LastHoverPos last;
    EXPECT_FALSE(last.IsRepeat(10, 20));

    last.Reset();
    EXPECT_FALSE(last.IsRepeat(10, 20));
}
