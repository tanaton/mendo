#include <gtest/gtest.h>
#include "document_types.h"

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
    EXPECT_TRUE(node.GetText().empty());
    EXPECT_TRUE(node.runs.empty());
    EXPECT_TRUE(node.anchor_id().empty());
    EXPECT_EQ(node.code_language, SyntaxLanguage::None);
    EXPECT_FALSE(node.has_table());
}

// ---- TextRunのデフォルト状態 ----

TEST(TextRun, DefaultState)
{
    TextRun run;
    EXPECT_EQ(run.start, 0u);
    EXPECT_EQ(run.length, 0u);
    EXPECT_FALSE(run.bold());
    EXPECT_FALSE(run.italic());
    EXPECT_FALSE(run.code());
    EXPECT_FALSE(run.strikethrough());
    EXPECT_FALSE(run.has_link());
}

// ---- TextRunフラグ操作 ----

TEST(TextRun, SetBold)
{
    TextRun run;
    run.set_bold(true);
    EXPECT_TRUE(run.bold());
}

TEST(TextRun, SetItalic)
{
    TextRun run;
    run.set_italic(true);
    EXPECT_TRUE(run.italic());
}

TEST(TextRun, SetCode)
{
    TextRun run;
    run.set_code(true);
    EXPECT_TRUE(run.code());
}

TEST(TextRun, SetStrikethrough)
{
    TextRun run;
    run.set_strikethrough(true);
    EXPECT_TRUE(run.strikethrough());
}

TEST(TextRun, MultipleFlags)
{
    TextRun run;
    run.set_bold(true);
    run.set_italic(true);
    run.set_code(true);
    run.set_strikethrough(true);
    EXPECT_TRUE(run.bold());
    EXPECT_TRUE(run.italic());
    EXPECT_TRUE(run.code());
    EXPECT_TRUE(run.strikethrough());
}

TEST(TextRun, ClearFlag)
{
    TextRun run;
    run.set_bold(true);
    run.set_italic(true);
    EXPECT_TRUE(run.bold());
    EXPECT_TRUE(run.italic());

    run.set_bold(false);
    EXPECT_FALSE(run.bold());
    EXPECT_TRUE(run.italic());
}

TEST(TextRun, SetAllThenClearAll)
{
    TextRun run;
    run.set_bold(true);
    run.set_italic(true);
    run.set_code(true);
    run.set_strikethrough(true);

    run.set_bold(false);
    run.set_italic(false);
    run.set_code(false);
    run.set_strikethrough(false);

    EXPECT_FALSE(run.bold());
    EXPECT_FALSE(run.italic());
    EXPECT_FALSE(run.code());
    EXPECT_FALSE(run.strikethrough());
}

TEST(TextRun, HasLinkWithIndex)
{
    TextRun run;
    run.link_url_index = 0;
    EXPECT_TRUE(run.has_link());
}

TEST(TextRun, HasLinkNegativeIndex)
{
    TextRun run;
    run.link_url_index = -1;
    EXPECT_FALSE(run.has_link());
}

// ---- AlertColorIndex ----

TEST(AlertColorIndex, NoneReturnsSentinel)
{
    EXPECT_EQ(AlertColorIndex(AlertType::None), ALERT_TYPE_COUNT);
}

TEST(AlertColorIndex, NoteReturnsZero)
{
    EXPECT_EQ(AlertColorIndex(AlertType::Note), 0u);
}

TEST(AlertColorIndex, TipReturnsOne)
{
    EXPECT_EQ(AlertColorIndex(AlertType::Tip), 1u);
}

TEST(AlertColorIndex, ImportantReturnsTwo)
{
    EXPECT_EQ(AlertColorIndex(AlertType::Important), 2u);
}

TEST(AlertColorIndex, WarningReturnsThree)
{
    EXPECT_EQ(AlertColorIndex(AlertType::Warning), 3u);
}

TEST(AlertColorIndex, CautionReturnsFour)
{
    EXPECT_EQ(AlertColorIndex(AlertType::Caution), 4u);
}

// ---- Nodeアクセサ ----

TEST(NodeTest, SetTextAndGetText)
{
    Node node;
    node.SetText(L"Hello");
    EXPECT_EQ(node.GetText(), L"Hello");
    EXPECT_TRUE(node.HasText());
}

TEST(NodeTest, SetTextStringView)
{
    Node node;
    std::wstring_view sv = L"Test text";
    node.SetText(sv);
    EXPECT_EQ(node.GetText(), L"Test text");
}

TEST(NodeTest, SetTextMove)
{
    Node node;
    std::pmr::wstring s = L"Moved text";
    node.SetText(std::move(s));
    EXPECT_EQ(node.GetText(), L"Moved text");
}

TEST(NodeTest, SetTextCountsNewlines)
{
    Node node;
    node.SetText(L"line1\nline2\nline3");
    EXPECT_EQ(node.line_count, 2);
}

TEST(NodeTest, SetTextClearsUtf8)
{
    Node node;
    node.text_utf8 = "original";
    node.SetText(L"new text");
    EXPECT_TRUE(node.text_utf8.empty());
}

TEST(NodeTest, LazyConversionFromUtf8)
{
    Node node;
    node.text_utf8 = "Lazy conversion";
    EXPECT_TRUE(node.HasText());
    EXPECT_EQ(node.GetText(), L"Lazy conversion");
}

TEST(NodeTest, LazyConversionJapanese)
{
    Node node;
    node.text_utf8 = "日本語";
    EXPECT_EQ(node.GetText(), L"日本語");
}

TEST(NodeTest, HasTextEmptyUtf8AndNoWide)
{
    Node node;
    EXPECT_FALSE(node.HasText());
}

TEST(NodeTest, EnsureTable)
{
    Node node;
    EXPECT_FALSE(node.has_table());
    node.ensure_table();
    EXPECT_TRUE(node.has_table());
    // 再度呼んでもクラッシュしない
    node.ensure_table();
    EXPECT_TRUE(node.has_table());
}

TEST(NodeTest, EnsureImage)
{
    Node node;
    EXPECT_FALSE(node.has_image());
    node.ensure_image();
    EXPECT_TRUE(node.has_image());
}

TEST(NodeTest, EnsureHeading)
{
    Node node;
    EXPECT_FALSE(node.has_heading());
    node.ensure_heading();
    EXPECT_TRUE(node.has_heading());
}

TEST(NodeTest, EnsureCode)
{
    Node node;
    EXPECT_FALSE(node.has_code());
    node.ensure_code();
    EXPECT_TRUE(node.has_code());
}

TEST(NodeTest, AnchorIdWithoutHeadingData)
{
    Node node;
    EXPECT_TRUE(node.anchor_id().empty());
}

TEST(NodeTest, AnchorIdWithHeadingData)
{
    Node node;
    node.ensure_heading();
    node.heading_data->anchor_id = L"test-anchor";
    EXPECT_EQ(node.anchor_id(), L"test-anchor");
}

TEST(NodeTest, SyntaxTokensWithoutCodeData)
{
    Node node;
    EXPECT_TRUE(node.syntax_tokens().empty());
}

TEST(NodeTest, SyntaxTokensMutCreatesCodeData)
{
    Node node;
    EXPECT_FALSE(node.has_code());
    auto& tokens = node.syntax_tokens_mut();
    EXPECT_TRUE(node.has_code());
    EXPECT_TRUE(tokens.empty());
}

TEST(NodeTest, CodeBlockPreservesUtf8OnGetText)
{
    Node node;
    node.type = NodeType::CodeBlock;
    node.text_utf8 = "code content";
    node.GetText(); // 遅延変換を発火
    // CodeBlockはtext_utf8を保持する
    EXPECT_FALSE(node.text_utf8.empty());
}

TEST(NodeTest, ParagraphReleasesUtf8OnGetText)
{
    Node node;
    node.type = NodeType::Paragraph;
    node.text_utf8 = "paragraph content";
    node.GetText(); // 遅延変換を発火
    // Paragraph等はtext_utf8を解放する
    EXPECT_TRUE(node.text_utf8.empty());
}
