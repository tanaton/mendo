#include <gtest/gtest.h>
#include <cmath>
#include "ui_types.h"

TEST(ScrollRestoration, InitialState)
{
    ScrollRestoration sr;
    EXPECT_FALSE(sr.HasNodeRestore());
    EXPECT_EQ(sr.pending_restore_node, -1);
    EXPECT_EQ(sr.pending_restore_offset, 0);
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
