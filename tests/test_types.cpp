#include <gtest/gtest.h>
#include "types.h"

// ---- TextSelection::MakeOrdered ----

TEST(TextSelection, SameNodeForwardOrder) {
    auto s = TextSelection::MakeOrdered(3, 5, 3, 10);
    EXPECT_EQ(s.start_node, 3);
    EXPECT_EQ(s.start_pos, 5u);
    EXPECT_EQ(s.end_node, 3);
    EXPECT_EQ(s.end_pos, 10u);
    EXPECT_TRUE(s.active);
}

TEST(TextSelection, SameNodeReverseOrder) {
    auto s = TextSelection::MakeOrdered(3, 10, 3, 5);
    EXPECT_EQ(s.start_node, 3);
    EXPECT_EQ(s.start_pos, 5u);
    EXPECT_EQ(s.end_node, 3);
    EXPECT_EQ(s.end_pos, 10u);
    EXPECT_TRUE(s.active);
}

TEST(TextSelection, DifferentNodesForwardOrder) {
    auto s = TextSelection::MakeOrdered(1, 0, 5, 3);
    EXPECT_EQ(s.start_node, 1);
    EXPECT_EQ(s.start_pos, 0u);
    EXPECT_EQ(s.end_node, 5);
    EXPECT_EQ(s.end_pos, 3u);
    EXPECT_TRUE(s.active);
}

TEST(TextSelection, DifferentNodesReverseOrder) {
    auto s = TextSelection::MakeOrdered(5, 3, 1, 0);
    EXPECT_EQ(s.start_node, 1);
    EXPECT_EQ(s.start_pos, 0u);
    EXPECT_EQ(s.end_node, 5);
    EXPECT_EQ(s.end_pos, 3u);
    EXPECT_TRUE(s.active);
}

TEST(TextSelection, SamePositionNotActive) {
    auto s = TextSelection::MakeOrdered(2, 7, 2, 7);
    EXPECT_EQ(s.start_node, 2);
    EXPECT_EQ(s.end_node, 2);
    EXPECT_EQ(s.start_pos, 7u);
    EXPECT_EQ(s.end_pos, 7u);
    EXPECT_FALSE(s.active);
}

TEST(TextSelection, ClearResetsState) {
    auto s = TextSelection::MakeOrdered(1, 0, 5, 3);
    EXPECT_TRUE(s.active);
    s.Clear();
    EXPECT_FALSE(s.active);
    EXPECT_EQ(s.start_node, -1);
    EXPECT_EQ(s.end_node, -1);
}

TEST(TextSelection, NodeZeroPositionZero) {
    auto s = TextSelection::MakeOrdered(0, 0, 0, 1);
    EXPECT_TRUE(s.active);
    EXPECT_EQ(s.start_node, 0);
    EXPECT_EQ(s.start_pos, 0u);
}
