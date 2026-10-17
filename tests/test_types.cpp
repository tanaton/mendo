#include <gtest/gtest.h>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "document_types.h"
#include "text_arena.h"

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
    EXPECT_TRUE(node.runs().empty());
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

TEST(NodeLineCount, SetTextStoresValueAsIs)
{
    Node node;
    node.SetText("a\nb\nc", 2);
    EXPECT_EQ(node.GetText(), "a\nb\nc");
    EXPECT_EQ(node.line_count, 2);
}

TEST(NodeLineCount, SetTextDoesNotCount)
{
    // 呼び出し側責任で line_count を渡す。再カウントしない。
    // パーサーが marker 除去後に差分計算で渡すケースを想定。
    Node node;
    node.SetText("a\nb\nc\nd\ne", 7);
    EXPECT_EQ(node.line_count, 7);
}

TEST(NodeLineCount, SetTextZeroAllowed)
{
    Node node;
    node.SetText("a\nb", 0);
    EXPECT_EQ(node.line_count, 0);
}

// テキストと runs はコピーせず、渡した領域をそのまま指す (置き場所の寿命は呼び出し側が持つ)。
TEST(NodeTest, SetTextAndRunsDoNotCopy)
{
    static constexpr std::string_view text = "bold text";
    TextRun run;
    run.length = 4;
    run.set_bold(true);
    const TextRun runs[]{ run };

    Node node;
    node.SetText(text, 0);
    node.SetRuns(runs);
    EXPECT_EQ(node.GetText().data(), text.data());
    ASSERT_EQ(node.runs().size(), 1u);
    EXPECT_EQ(node.runs().data(), runs);
}

TEST(NodeTest, SourceOffsetUnsetByDefault)
{
    Node node;
    EXPECT_FALSE(node.HasSourceOffset());
    EXPECT_EQ(node.SourceOffset(), kUnsetSourceOffset);
    node.SetSourceOffset(0);
    EXPECT_TRUE(node.HasSourceOffset());
    EXPECT_EQ(node.SourceOffset(), 0u);
}

TEST(NodeTest, HasTextEmptyByDefault)
{
    Node node;
    EXPECT_FALSE(node.HasText());
}

// payload は種別ごとに使い回すので、種別が合わないノードからは見えない。
TEST(NodeTest, TableDataRequiresTableType)
{
    NodeTableData table;
    Node node;
    node.set_table_data(&table);
    EXPECT_FALSE(node.has_table());
    EXPECT_EQ(node.table_data(), nullptr);
    node.type = NodeType::Table;
    EXPECT_TRUE(node.has_table());
    EXPECT_EQ(node.table_data(), &table);
    EXPECT_EQ(std::as_const(node).table_data(), &table);
}

TEST(NodeTest, ImageDataRequiresImageType)
{
    static constexpr NodeImageData kImage{ "a.png" };
    Node node;
    node.set_image_data(&kImage);
    EXPECT_FALSE(node.has_image());
    EXPECT_EQ(node.image_data(), nullptr);
    node.type = NodeType::Image;
    EXPECT_TRUE(node.has_image());
    EXPECT_EQ(node.image_data()->src, "a.png");
}

TEST(NodeTest, HeadingLevelRequiresHeadingType)
{
    Node node;
    node.set_heading_level(3);
    EXPECT_EQ(node.heading_level(), 0);
    node.type = NodeType::Heading;
    EXPECT_EQ(node.heading_level(), 3);
}

TEST(NodeTest, MermaidCodeBlockTextStored)
{
    Node node;
    node.type = NodeType::Diagram;
    node.set_code_language(SyntaxLanguage::Mermaid);
    node.SetText("graph TD;A-->B", 0);
    EXPECT_EQ(node.GetText(), "graph TD;A-->B");
}

TEST(NodeTest, LatexMathCodeBlockTextStored)
{
    Node node;
    node.type = NodeType::Diagram;
    node.set_code_language(SyntaxLanguage::LatexMath);
    node.SetText("E = mc^2", 0);
    EXPECT_EQ(node.GetText(), "E = mc^2");
}

TEST(NodeTest, NonDiagramCodeBlockTextStored)
{
    Node node;
    node.type = NodeType::CodeBlock;
    node.set_code_language(SyntaxLanguage::Cpp);
    node.SetText("int main() { return 0; }", 0);
    EXPECT_EQ(node.GetText(), "int main() { return 0; }");
}

TEST(NodeTest, ParagraphTextStored)
{
    Node node;
    node.type = NodeType::Paragraph;
    node.SetText("paragraph content", 0);
    EXPECT_EQ(node.GetText(), "paragraph content");
}

// alert_type は伝播ロジック (parser_alerts.cpp:DetectAlertAt) のため payload と別に持つ。
// BlockQuote 本体は両方を持ち、伝播先ノードは alert_type のみでラベル長は 0 のはず。
TEST(NodeTest, AlertTypeAndLabelLengthAreIndependent)
{
    Node body;
    body.type = NodeType::BlockQuote;
    body.alert_type = AlertType::Warning;
    body.set_alert_label_length(8u);
    EXPECT_EQ(body.alert_type, AlertType::Warning);
    EXPECT_EQ(body.alert_label_length(), 8u);

    Node propagated;
    propagated.type = NodeType::Paragraph;
    propagated.alert_type = AlertType::Warning;
    EXPECT_EQ(propagated.alert_type, AlertType::Warning);
    EXPECT_EQ(propagated.alert_label_length(), 0u);
}

// 番号を持たないノードは list_ordered=false, list_number=0。
TEST(NodeTest, ListGettersDefaultsWhenNoData)
{
    Node node;
    EXPECT_FALSE(node.list_ordered());
    EXPECT_EQ(node.list_number(), 0);
    EXPECT_FALSE(node.task_checked());

    node.set_list_number(5);
    node.set_task_checked();
    EXPECT_TRUE(node.list_ordered());
    EXPECT_EQ(node.list_number(), 5);
    EXPECT_TRUE(node.task_checked());
}

// 0 始まりの順序付きリストも番号を持つ。
TEST(NodeTest, ListNumberZeroIsOrdered)
{
    Node node;
    node.set_list_number(0);
    EXPECT_TRUE(node.list_ordered());
    EXPECT_EQ(node.list_number(), 0);
}

// チャンクをまたいでも、専用に確保する大きな要求を挟んでも、先に置いたものは動かず壊れない。
TEST(TextArenaTest, CopiesSurviveChunkGrowthAndLargeRequests)
{
    TextArena arena;
    std::vector<std::string> sources;
    std::vector<std::string_view> copies;
    for (size_t i = 0; i < 4000; ++i) {
        const size_t len = (i % 1000 == 999) ? 300 * 1024 : 1 + (i * 37) % 2000;
        sources.emplace_back(len, static_cast<char>('a' + i % 26));
        copies.push_back(arena.CopyText(sources.back()));
    }
    for (size_t i = 0; i < sources.size(); ++i) {
        ASSERT_EQ(copies[i], sources[i]) << "i=" << i;
        ASSERT_NE(copies[i].data(), sources[i].data());
    }
}

TEST(TextArenaTest, CopyKeepsAlignmentAfterOddSizedText)
{
    TextArena arena;
    (void)arena.CopyText("abc");
    TextRun run;
    run.start = 7;
    run.length = 3;
    const std::vector<TextRun> runs{ run, run };
    const auto copied = arena.Copy(runs);
    ASSERT_EQ(copied.size(), 2u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(copied.data()) % alignof(TextRun), 0u);
    EXPECT_EQ(copied[1].start, 7u);
    EXPECT_EQ(copied[1].length, 3u);
}

TEST(TextArenaTest, EmptyInputsReturnEmpty)
{
    TextArena arena;
    EXPECT_TRUE(arena.CopyText("").empty());
    EXPECT_TRUE(arena.Copy(std::vector<TextRun>{}).empty());
}
