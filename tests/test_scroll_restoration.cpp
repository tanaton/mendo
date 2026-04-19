#include <gtest/gtest.h>
#include <cmath>
#include "scroll_restoration.h"

TEST(ScrollRestoration, InitialState)
{
    ScrollRestoration sr;
    EXPECT_FALSE(sr.HasNavScroll());
    EXPECT_FALSE(sr.HasNodeRestore());
    EXPECT_FLOAT_EQ(sr.pending_nav_scroll_y, -1.0f);
    EXPECT_EQ(sr.pending_restore_node, -1);
    EXPECT_EQ(sr.pending_restore_offset, 0);
}

TEST(ScrollRestoration, HasNavScroll)
{
    ScrollRestoration sr;
    EXPECT_FALSE(sr.HasNavScroll());

    sr.pending_nav_scroll_y = 0.0f;
    EXPECT_TRUE(sr.HasNavScroll());

    sr.pending_nav_scroll_y = 500.0f;
    EXPECT_TRUE(sr.HasNavScroll());
}

TEST(ScrollRestoration, ConsumeNavScroll)
{
    ScrollRestoration sr;
    sr.pending_nav_scroll_y = 250.0f;
    EXPECT_TRUE(sr.HasNavScroll());

    const float v = sr.ConsumeNavScroll();
    EXPECT_FLOAT_EQ(v, 250.0f);
    EXPECT_FALSE(sr.HasNavScroll());
    EXPECT_FLOAT_EQ(sr.pending_nav_scroll_y, -1.0f);
}

TEST(ScrollRestoration, HasNodeRestore)
{
    ScrollRestoration sr;
    EXPECT_FALSE(sr.HasNodeRestore());

    sr.pending_restore_node = 0;
    EXPECT_TRUE(sr.HasNodeRestore());

    sr.pending_restore_node = 42;
    EXPECT_TRUE(sr.HasNodeRestore());
}

TEST(ScrollRestoration, SetNodeRestore)
{
    ScrollRestoration sr;
    sr.SetNodeRestore(10, 50);
    EXPECT_EQ(sr.pending_restore_node, 10);
    EXPECT_EQ(sr.pending_restore_offset, 50);
}

TEST(ScrollRestoration, ClearNodeRestore)
{
    ScrollRestoration sr;
    sr.SetNodeRestore(10, 50);
    sr.ClearNodeRestore();
    EXPECT_FALSE(sr.HasNodeRestore());
    EXPECT_EQ(sr.pending_restore_node, -1);
    EXPECT_EQ(sr.pending_restore_offset, 0);
}

TEST(ScrollRestoration, Reset)
{
    ScrollRestoration sr;
    sr.pending_nav_scroll_y = 100.0f;
    sr.SetNodeRestore(10, 50);

    sr.Reset();
    EXPECT_FALSE(sr.HasNavScroll());
    EXPECT_FALSE(sr.HasNodeRestore());
    EXPECT_FLOAT_EQ(sr.pending_nav_scroll_y, -1.0f);
    EXPECT_EQ(sr.pending_restore_node, -1);
    EXPECT_EQ(sr.pending_restore_offset, 0);
}
