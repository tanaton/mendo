#include <gtest/gtest.h>
#include "hover_throttle.h"

TEST(HoverThrottle, InitialState)
{
    HoverThrottle ht;
    EXPECT_EQ(ht.last_md_hit_pos.x, std::numeric_limits<LONG>::min());
    EXPECT_EQ(ht.last_md_hit_pos.y, std::numeric_limits<LONG>::min());
    EXPECT_FALSE(ht.last_md_cursor_hand);
    EXPECT_EQ(ht.last_copy_hit_pos.x, std::numeric_limits<LONG>::min());
    EXPECT_EQ(ht.last_copy_hit_pos.y, std::numeric_limits<LONG>::min());
}

TEST(HoverThrottle, Reset)
{
    HoverThrottle ht;
    ht.last_md_hit_pos = { 100, 200 };
    ht.last_copy_hit_pos = { 300, 400 };

    ht.Reset();
    EXPECT_EQ(ht.last_md_hit_pos.x, std::numeric_limits<LONG>::min());
    EXPECT_EQ(ht.last_md_hit_pos.y, std::numeric_limits<LONG>::min());
    EXPECT_EQ(ht.last_copy_hit_pos.x, std::numeric_limits<LONG>::min());
    EXPECT_EQ(ht.last_copy_hit_pos.y, std::numeric_limits<LONG>::min());
}

TEST(HoverThrottle, ResetDoesNotAffectCursorFlag)
{
    HoverThrottle ht;
    ht.last_md_cursor_hand = true;

    ht.Reset();
    EXPECT_TRUE(ht.last_md_cursor_hand);
}
