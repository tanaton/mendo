#include <gtest/gtest.h>
#include "types.h"

// ---- TextSelection::MakeOrdered ----

TEST(TextSelection, SameNodeForwardOrder)
{
    auto s = TextSelection::MakeOrdered(3, 5, 3, 10);
    EXPECT_EQ(s.start_node, 3);
    EXPECT_EQ(s.start_pos, 5u);
    EXPECT_EQ(s.end_node, 3);
    EXPECT_EQ(s.end_pos, 10u);
    EXPECT_TRUE(s.active);
}

TEST(TextSelection, SameNodeReverseOrder)
{
    auto s = TextSelection::MakeOrdered(3, 10, 3, 5);
    EXPECT_EQ(s.start_node, 3);
    EXPECT_EQ(s.start_pos, 5u);
    EXPECT_EQ(s.end_node, 3);
    EXPECT_EQ(s.end_pos, 10u);
    EXPECT_TRUE(s.active);
}

TEST(TextSelection, DifferentNodesForwardOrder)
{
    auto s = TextSelection::MakeOrdered(1, 0, 5, 3);
    EXPECT_EQ(s.start_node, 1);
    EXPECT_EQ(s.start_pos, 0u);
    EXPECT_EQ(s.end_node, 5);
    EXPECT_EQ(s.end_pos, 3u);
    EXPECT_TRUE(s.active);
}

TEST(TextSelection, DifferentNodesReverseOrder)
{
    auto s = TextSelection::MakeOrdered(5, 3, 1, 0);
    EXPECT_EQ(s.start_node, 1);
    EXPECT_EQ(s.start_pos, 0u);
    EXPECT_EQ(s.end_node, 5);
    EXPECT_EQ(s.end_pos, 3u);
    EXPECT_TRUE(s.active);
}

TEST(TextSelection, SamePositionNotActive)
{
    auto s = TextSelection::MakeOrdered(2, 7, 2, 7);
    EXPECT_EQ(s.start_node, 2);
    EXPECT_EQ(s.end_node, 2);
    EXPECT_EQ(s.start_pos, 7u);
    EXPECT_EQ(s.end_pos, 7u);
    EXPECT_FALSE(s.active);
}

TEST(TextSelection, ClearResetsState)
{
    auto s = TextSelection::MakeOrdered(1, 0, 5, 3);
    EXPECT_TRUE(s.active);
    s.Clear();
    EXPECT_FALSE(s.active);
    EXPECT_EQ(s.start_node, -1);
    EXPECT_EQ(s.end_node, -1);
}

TEST(TextSelection, NodeZeroPositionZero)
{
    auto s = TextSelection::MakeOrdered(0, 0, 0, 1);
    EXPECT_TRUE(s.active);
    EXPECT_EQ(s.start_node, 0);
    EXPECT_EQ(s.start_pos, 0u);
}

// ---- 追加エッジケース ----

TEST(TextSelection, LargeNodeIndices)
{
    auto s = TextSelection::MakeOrdered(100000, 50000, 200000, 99999);
    EXPECT_EQ(s.start_node, 100000);
    EXPECT_EQ(s.end_node, 200000);
    EXPECT_TRUE(s.active);
}

TEST(TextSelection, ClearAndRecreate)
{
    auto s = TextSelection::MakeOrdered(1, 0, 5, 3);
    s.Clear();
    EXPECT_FALSE(s.active);
    // クリア状態から新しい選択を作成
    s = TextSelection::MakeOrdered(2, 1, 3, 4);
    EXPECT_TRUE(s.active);
    EXPECT_EQ(s.start_node, 2);
    EXPECT_EQ(s.end_node, 3);
}

TEST(TextSelection, SingleCharSelection)
{
    auto s = TextSelection::MakeOrdered(0, 5, 0, 6);
    EXPECT_TRUE(s.active);
    EXPECT_EQ(s.start_pos, 5u);
    EXPECT_EQ(s.end_pos, 6u);
}

TEST(TextSelection, ZeroPosZeroNode)
{
    auto s = TextSelection::MakeOrdered(0, 0, 0, 0);
    EXPECT_FALSE(s.active); // 同じ位置 = アクティブではない
}

TEST(TextSelection, ReverseWithDifferentNodesSamePos)
{
    auto s = TextSelection::MakeOrdered(5, 0, 1, 0);
    EXPECT_EQ(s.start_node, 1);
    EXPECT_EQ(s.end_node, 5);
    EXPECT_TRUE(s.active);
}

// ---- Nodeのデフォルト状態 ----

TEST(NodeTest, DefaultState)
{
    Node node;
    EXPECT_EQ(node.type, NodeType::Paragraph);
    EXPECT_EQ(node.heading_level, 0);
    EXPECT_EQ(node.indent_level, 0);
    EXPECT_EQ(node.list_number, 0);
    EXPECT_FALSE(node.task_checked);
    EXPECT_TRUE(node.text.empty());
    EXPECT_TRUE(node.runs.empty());
    EXPECT_TRUE(node.anchor_id.empty());
    EXPECT_EQ(node.code_language, SyntaxLanguage::None);
    EXPECT_FALSE(node.has_table());
}

// ---- TextRunのデフォルト状態 ----

TEST(TextRun, DefaultState)
{
    TextRun run;
    EXPECT_EQ(run.start, 0u);
    EXPECT_EQ(run.length, 0u);
    EXPECT_FALSE(run.bold);
    EXPECT_FALSE(run.italic);
    EXPECT_FALSE(run.code);
    EXPECT_FALSE(run.strikethrough);
    EXPECT_FALSE(run.has_link());
}
