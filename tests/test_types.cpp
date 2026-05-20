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
    EXPECT_EQ(node.heading_level(), 0);
    EXPECT_EQ(node.indent_level, 0);
    EXPECT_EQ(node.list_number(), 0);
    EXPECT_FALSE(node.task_checked());
    EXPECT_TRUE(node.GetText().empty());
    EXPECT_TRUE(node.runs.empty());
    EXPECT_TRUE(node.anchor_id().empty());
    EXPECT_EQ(node.code_language(), SyntaxLanguage::None);
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

// ---- Node::line_count ----

TEST(NodeLineCount, DefaultIsZero)
{
    Node node;
    EXPECT_EQ(node.line_count, 0);
}

TEST(NodeLineCount, SetTextWithLineCountStoresValueAsIs)
{
    Node node;
    node.SetTextWithLineCount(std::string_view{ "a\nb\nc" }, 2);
    EXPECT_EQ(node.GetText(), "a\nb\nc");
    EXPECT_EQ(node.line_count, 2);
}

TEST(NodeLineCount, SetTextWithLineCountDoesNotCount)
{
    // 呼び出し側責任で line_count を渡すバリアント。SetText と違い再カウントしない。
    // パーサーが marker 除去後に差分計算で渡すケースを想定。
    Node node;
    node.SetTextWithLineCount(std::string_view{ "a\nb\nc\nd\ne" }, 7);
    EXPECT_EQ(node.line_count, 7);
}

TEST(NodeLineCount, SetTextWithLineCountZeroAllowed)
{
    Node node;
    node.SetTextWithLineCount(std::string_view{ "a\nb" }, 0);
    EXPECT_EQ(node.line_count, 0);
}

TEST(NodeLineCount, SetTextWithLineCountPmrMoveOverload)
{
    Node node;
    std::pmr::string s = "x\ny\nz";
    node.SetTextWithLineCount(std::move(s), 2);
    EXPECT_EQ(node.GetText(), "x\ny\nz");
    EXPECT_EQ(node.line_count, 2);
}

TEST(NodeTest, HasTextEmptyByDefault)
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
    node.ensure_anchor_id_mut() = "test-anchor";
    EXPECT_EQ(node.anchor_id(), "test-anchor");
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

TEST(NodeTest, MermaidCodeBlockTextStored)
{
    Node node;
    node.type = NodeType::CodeBlock;
    node.ensure_code()->code_language = SyntaxLanguage::Mermaid;
    node.SetTextWithLineCount(std::string_view{ "graph TD;A-->B" }, 0);
    EXPECT_EQ(node.GetText(), "graph TD;A-->B");
}

TEST(NodeTest, LatexMathCodeBlockTextStored)
{
    Node node;
    node.type = NodeType::CodeBlock;
    node.ensure_code()->code_language = SyntaxLanguage::LatexMath;
    node.SetTextWithLineCount(std::string_view{ "E = mc^2" }, 0);
    EXPECT_EQ(node.GetText(), "E = mc^2");
}

TEST(NodeTest, NonDiagramCodeBlockTextStored)
{
    Node node;
    node.type = NodeType::CodeBlock;
    node.ensure_code()->code_language = SyntaxLanguage::Cpp;
    node.SetTextWithLineCount(std::string_view{ "int main() { return 0; }" }, 0);
    EXPECT_EQ(node.GetText(), "int main() { return 0; }");
}

TEST(NodeTest, ParagraphTextStored)
{
    Node node;
    node.type = NodeType::Paragraph;
    node.SetTextWithLineCount(std::string_view{ "paragraph content" }, 0);
    EXPECT_EQ(node.GetText(), "paragraph content");
}

// variant alternative の排他性: 別種を ensure すると元のデータは破棄される。
// parser は BeginNode 直後に 1 種類だけ ensure する契約だが、契約違反を回帰検出するためのテスト。
TEST(NodeTest, EnsureCodeReplacesHeading)
{
    Node node;
    node.ensure_heading()->heading_level = 3;
    EXPECT_TRUE(node.has_heading());

    node.ensure_code()->code_language = SyntaxLanguage::Cpp;
    EXPECT_FALSE(node.has_heading());
    EXPECT_TRUE(node.has_code());
    EXPECT_EQ(node.heading_level(), 0);
    EXPECT_EQ(node.code_language(), SyntaxLanguage::Cpp);
}

// ensure_table() は冪等で、連続呼出しで同じ NodeTableData ポインタを返す。
TEST(NodeTest, EnsureTableIsIdempotent)
{
    Node node;
    auto* tbl1 = node.ensure_table();
    auto* tbl2 = node.ensure_table();
    EXPECT_EQ(tbl1, tbl2);
    EXPECT_TRUE(node.has_table());
}

// ensure_image() の冪等性。
TEST(NodeTest, EnsureImageIsIdempotent)
{
    Node node;
    auto* img1 = node.ensure_image();
    auto* img2 = node.ensure_image();
    EXPECT_EQ(img1, img2);
    EXPECT_TRUE(node.has_image());
}

// alert_type は伝播ロジック (parser.cpp:DetectAlertAt) のため Node 直下に残し、
// alert_label_length のみ alert_data に閉じ込めた。BlockQuote 本体は両方を持ち、
// 伝播先ノードは alert_type のみで alert_data は不在のはず。
TEST(NodeTest, AlertTypeAndDataAreIndependent)
{
    Node body;
    body.type = NodeType::BlockQuote;
    body.alert_type = AlertType::Warning;
    body.ensure_alert()->alert_label_length = 8u;
    EXPECT_EQ(body.alert_type, AlertType::Warning);
    EXPECT_TRUE(body.has_alert());
    EXPECT_EQ(body.alert_label_length(), 8u);

    Node propagated;
    propagated.type = NodeType::Paragraph;
    propagated.alert_type = AlertType::Warning;
    EXPECT_EQ(propagated.alert_type, AlertType::Warning);
    EXPECT_FALSE(propagated.has_alert());
    EXPECT_EQ(propagated.alert_label_length(), 0u);
}

// list_data の互換 getter: 持たないノードは task_checked=false, list_number=0。
TEST(NodeTest, ListGettersDefaultsWhenNoData)
{
    Node node;
    EXPECT_FALSE(node.has_list());
    EXPECT_EQ(node.list_number(), 0);
    EXPECT_FALSE(node.task_checked());

    node.ensure_list()->list_number = 5;
    node.ensure_list()->task_checked = true;
    EXPECT_TRUE(node.has_list());
    EXPECT_EQ(node.list_number(), 5);
    EXPECT_TRUE(node.task_checked());
}
