#include <gtest/gtest.h>
#include "parse_invariants.h"
#include "parser.h"
#include "parser_alerts.h"
#include "test_nodes.h"
#include <limits>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

// ---- 基本的なパース ----

TEST(Parser, EmptyInputReturnsNoNodes)
{
    auto nodes = ParseNodes("");
    EXPECT_TRUE(nodes.empty());
}

TEST(Parser, WhitespaceOnlyReturnsNoNodes)
{
    auto nodes = ParseNodes("   \n\n  ");
    EXPECT_TRUE(nodes.empty());
}

TEST(Parser, SingleParagraph)
{
    auto nodes = ParseNodes("Hello world");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_EQ(nodes[0].GetText(), "Hello world");
}

TEST(Parser, MultipleParagraphs)
{
    auto nodes = ParseNodes("First\n\nSecond\n\nThird");
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_EQ(nodes[1].type, NodeType::Paragraph);
    EXPECT_EQ(nodes[2].type, NodeType::Paragraph);
    EXPECT_EQ(nodes[0].GetText(), "First");
    EXPECT_EQ(nodes[1].GetText(), "Second");
    EXPECT_EQ(nodes[2].GetText(), "Third");
}

// ---- 見出し ----

TEST(Parser, HeadingH1)
{
    auto nodes = ParseNodes("# Title");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Heading);
    EXPECT_EQ(nodes[0].heading_level(), 1);
    EXPECT_EQ(nodes[0].GetText(), "Title");
}

TEST(Parser, HeadingH2)
{
    auto nodes = ParseNodes("## Subtitle");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].heading_level(), 2);
}

TEST(Parser, HeadingH3ToH6)
{
    for (int level = 3; level <= 6; level++) {
        std::string md(level, '#');
        md += " Test";
        auto nodes = ParseNodes(md);
        ASSERT_EQ(nodes.size(), 1u) << "level=" << level;
        EXPECT_EQ(nodes[0].heading_level(), level) << "level=" << level;
    }
}

// ---- インライン書式 ----

TEST(Parser, BoldText)
{
    auto nodes = ParseNodes("**bold**");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), "bold");
    ASSERT_GE(nodes[0].runs().size(), 1u);
    EXPECT_TRUE(nodes[0].runs()[0].bold());
    EXPECT_FALSE(nodes[0].runs()[0].italic());
}

TEST(Parser, ItalicText)
{
    auto nodes = ParseNodes("*italic*");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), "italic");
    ASSERT_GE(nodes[0].runs().size(), 1u);
    EXPECT_TRUE(nodes[0].runs()[0].italic());
    EXPECT_FALSE(nodes[0].runs()[0].bold());
}

TEST(Parser, InlineCode)
{
    auto nodes = ParseNodes("`code`");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), "code");
    ASSERT_GE(nodes[0].runs().size(), 1u);
    EXPECT_TRUE(nodes[0].runs()[0].code());
}

TEST(Parser, StrikethroughText)
{
    auto nodes = ParseNodes("~~deleted~~");
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_GE(nodes[0].runs().size(), 1u);
    EXPECT_TRUE(nodes[0].runs()[0].strikethrough());
}

// ---- リンク ----

TEST(Parser, ExternalLink)
{
    auto nodes = ParseNodes("[text](https://example.com)");
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_GE(nodes[0].runs().size(), 1u);
    ASSERT_TRUE(nodes[0].runs()[0].has_link());
    EXPECT_EQ(nodes[0].view_link_urls()[nodes[0].runs()[0].link_url_index], "https://example.com");
    EXPECT_EQ(nodes[0].GetText(), "text");
}

TEST(Parser, InternalLink)
{
    auto nodes = ParseNodes("[section](#my-section)");
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_GE(nodes[0].runs().size(), 1u);
    ASSERT_TRUE(nodes[0].runs()[0].has_link());
    EXPECT_EQ(nodes[0].view_link_urls()[nodes[0].runs()[0].link_url_index], "#my-section");
}

TEST(Parser, ParagraphWithNoLink)
{
    auto nodes = ParseNodes("plain text");
    ASSERT_EQ(nodes.size(), 1u);
    for (const auto& run : nodes[0].runs()) {
        EXPECT_FALSE(run.has_link());
    }
}

TEST(Parser, LinkUrlCountCappedAtInt16Max)
{
    // link_url_index は int16_t。上限を超えた URL はリンクなしとして扱われ、
    // wrap して別 URL へ誤マッピングしないこと。テーブルは全体が 1 ノードで
    // link_urls を共有するため上限へ到達しうる。
    constexpr int kUrls = 32769;
    std::string md = "| col |\n|---|\n";
    md.reserve(md.size() + static_cast<size_t>(kUrls) * 20);
    for (int i = 0; i < kUrls; ++i) {
        md += "| [t](u";
        md += std::to_string(i);
        md += ") |\n";
    }
    auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 1u);
    const auto urls = nodes[0].view_link_urls();
    EXPECT_EQ(urls.size(), static_cast<size_t>(std::numeric_limits<int16_t>::max()));
    // 上限超過後のセルはリンクなし (装飾なしなので run も出ない)。全 run の index が urls 範囲内に収まること
    const auto* tbl = nodes[0].table_data();
    ASSERT_NE(tbl, nullptr);
    size_t linked = 0;
    for (const auto& run : tbl->all_runs) {
        ASSERT_TRUE(run.has_link());
        EXPECT_LT(static_cast<size_t>(run.link_url_index), urls.size());
        ++linked;
    }
    EXPECT_EQ(linked, urls.size());
}

// ---- コードブロック ----

TEST(Parser, CodeBlockPreservesNewlines)
{
    auto nodes = ParseNodes("```\na\nb\nc\n```");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    // 行間に改行を含むべき
    const auto& text = nodes[0].GetText();
    int newlines = 0;
    for (wchar_t c : text)
        if (c == '\n')
            newlines++;
    EXPECT_GE(newlines, 2);
}

TEST(Parser, CodeBlockNoTrailingNewline)
{
    auto nodes = ParseNodes("```\nhello\n```");
    ASSERT_EQ(nodes.size(), 1u);
    // 末尾の改行は除去されるべき
    EXPECT_FALSE(nodes[0].GetText().empty());
    EXPECT_NE(nodes[0].GetText().back(), '\n');
}

// ---- 水平線 ----

TEST(Parser, HorizontalRuleWithAsterisks)
{
    auto nodes = ParseNodes("***");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::HorizontalRule);
}

// ---- リスト ----

TEST(Parser, UnorderedList)
{
    auto nodes = ParseNodes("- item1\n- item2\n- item3");
    ASSERT_EQ(nodes.size(), 3u);
    for (const auto& node : nodes) {
        EXPECT_EQ(node.type, NodeType::ListItem);
        EXPECT_EQ(node.list_number(), 0);  // 順序なしリスト
        EXPECT_FALSE(node.list_ordered()); // ordered フラグも false
    }
    EXPECT_EQ(nodes[0].GetText(), "item1");
    EXPECT_EQ(nodes[1].GetText(), "item2");
    EXPECT_EQ(nodes[2].GetText(), "item3");
}

// loose list は md4c が LI 中身を MD_BLOCK_P に外出しするため LI 自体は空 (issue#237 の原因)。
TEST(Parser, UnorderedListLooseProducesParagraphChildren)
{
    auto nodes = ParseNodes("- a\n\n- b");
    ASSERT_EQ(nodes.size(), 4u);
    EXPECT_EQ(nodes[0].type, NodeType::ListItem);
    EXPECT_FALSE(nodes[0].HasText());
    EXPECT_EQ(nodes[1].type, NodeType::Paragraph);
    EXPECT_EQ(nodes[1].GetText(), "a");
    EXPECT_EQ(nodes[2].type, NodeType::ListItem);
    EXPECT_FALSE(nodes[2].HasText());
    EXPECT_EQ(nodes[3].type, NodeType::Paragraph);
    EXPECT_EQ(nodes[3].GetText(), "b");
}

// 装飾 run は 1 テキストあたり 4096 個で打ち切る (DirectWrite の書式設定が二乗で遅くなるため)。
TEST(Parser, StyledRunsAreCappedPerText)
{
    std::string para;
    std::string row = "|h|\n|-|\n|";
    for (int i = 0; i < 5000; i++) {
        para += "`a` ";
        row += "`a` ";
    }
    const auto nodes = ParseNodes(para);
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].runs().size(), 4096u);
    EXPECT_EQ(nodes[0].GetText().size(), para.size() - 2 * 5000 - 1);

    row += "|\n";
    const auto table = ParseNodes(row);
    ASSERT_EQ(table.size(), 1u);
    ASSERT_TRUE(table[0].has_table());
    EXPECT_EQ(table[0].table_data()->GetCellRuns(1, 0).size(), 4096u);
}

// 中身を子ブロックに持つ LI だけが高さ 0 の枠になる。本当に空の LI は枠扱いしない。
TEST(Parser, EmptyListItemContainerRequiresChildBlocks)
{
    const auto loose = ParseNodes("- a\n\n- b");
    ASSERT_EQ(loose.size(), 4u);
    EXPECT_TRUE(IsEmptyListItemContainer(loose[0]));
    EXPECT_TRUE(IsEmptyListItemContainer(loose[2]));

    const auto nested = ParseNodes("-\n  - b");
    ASSERT_EQ(nested.size(), 2u);
    EXPECT_TRUE(IsEmptyListItemContainer(nested[0]));
    EXPECT_FALSE(IsEmptyListItemContainer(nested[1]));

    const auto empty = ParseNodes("1.\n2.\n- [ ]\n");
    ASSERT_EQ(empty.size(), 3u);
    for (const auto& node : empty) {
        EXPECT_TRUE(IsListItem(node));
        EXPECT_FALSE(IsEmptyListItemContainer(node));
    }
}

// md4c は tight list の LI 直下に MD_BLOCK_P を出さないため、子ブロック後の本文は段落ノードで受ける。
TEST(Parser, TightListTextAfterHeadingIsKept)
{
    auto nodes = ParseNodes("- # H\n  text");
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(nodes[1].type, NodeType::Heading);
    EXPECT_EQ(nodes[2].type, NodeType::Paragraph);
    EXPECT_EQ(nodes[2].GetText(), "text");
    EXPECT_EQ(nodes[2].indent_level, nodes[1].indent_level);
}

TEST(Parser, TightListTextAfterFenceIsNotMergedIntoCode)
{
    auto nodes = ParseNodes("- a\n  ```\n  code\n  ```\n  after");
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(nodes[1].type, NodeType::CodeBlock);
    EXPECT_EQ(nodes[1].GetText(), "code");
    EXPECT_EQ(nodes[2].type, NodeType::Paragraph);
    EXPECT_EQ(nodes[2].GetText(), "after");
}

TEST(Parser, TightListLinkAfterHorizontalRuleKeepsUrl)
{
    auto nodes = ParseNodes("- a\n  ***\n  [l](https://example.com)");
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(nodes[2].type, NodeType::Paragraph);
    EXPECT_EQ(nodes[2].GetText(), "l");
    ASSERT_EQ(nodes[2].view_link_urls().size(), 1u);
    ASSERT_EQ(nodes[2].runs().size(), 1u);
    EXPECT_TRUE(nodes[2].runs()[0].has_link());
}

TEST(Parser, TightListInBlockquoteTextAfterHeadingStaysQuoted)
{
    auto nodes = ParseNodes("> - # H\n>   text");
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(nodes[2].type, NodeType::BlockQuote);
    EXPECT_EQ(nodes[2].GetText(), "text");
    EXPECT_EQ(nodes[2].blockquote_group, nodes[1].blockquote_group);
}

// ---- バグ #10: ネストされた引用ブロック ----

TEST(Parser, NestedBlockquotePreservesOuterStyle)
{
    auto nodes = ParseNodes("> outer\n>\n> > inner\n>\n> still outer");
    // 内側の引用ブロックが終了した後、"still outer"はまだBlockQuoteであるべき
    bool found_still_outer = false;
    for (const auto& node : nodes) {
        if (node.GetText().find("still outer") != std::string::npos) {
            EXPECT_EQ(node.type, NodeType::BlockQuote)
                << "内側の引用ブロック後のテキストはBlockQuoteのままであるべき";
            found_still_outer = true;
        }
    }
    EXPECT_TRUE(found_still_outer) << "ノード内に'still outer'が見つかるべき";
}

TEST(Parser, SingleBlockquoteIsBlockQuoteType)
{
    auto nodes = ParseNodes("> quoted text");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::BlockQuote);
    EXPECT_EQ(nodes[0].GetText(), "quoted text");
}

// ネスト blockquote の group/quote_depth 設計回帰防止
// (PR #156 で blockquote_group をネスト中も最外側で共有する設計に変更)

TEST(Parser, NestedBlockquote_SharesSameGroup)
{
    auto nodes = ParseNodes("> outer\n>\n> > inner\n>\n> still outer");
    ASSERT_GE(nodes.size(), 3u);
    const int group = nodes[0].blockquote_group;
    EXPECT_GE(group, 0);
    for (const auto& n : nodes) {
        if (n.type == NodeType::BlockQuote) {
            EXPECT_EQ(n.blockquote_group, group)
                << "ネストの内外を問わず最外側 group を共有する";
        }
    }
}

TEST(Parser, NestedBlockquote_QuoteDepthReflectsNesting)
{
    auto nodes = ParseNodes("> outer\n> > inner\n> > > deep");
    int max_depth = 0;
    for (const auto& n : nodes) {
        if (n.type == NodeType::BlockQuote && n.quote_depth > max_depth) {
            max_depth = n.quote_depth;
        }
    }
    EXPECT_EQ(max_depth, 3) << "3 段ネストの最大 quote_depth は 3";
}

TEST(Parser, NestedBlockquote_OuterReturnAfterInnerKeepsDepthOne)
{
    // 内側 quote の終了は空行 `>` が必要 (md4c の lazy continuation 挙動)
    auto nodes = ParseNodes("> outer\n> > inner\n>\n> back to outer");
    bool checked = false;
    for (const auto& n : nodes) {
        if (n.GetText().find("back to outer") != std::string::npos) {
            EXPECT_EQ(n.quote_depth, 1)
                << "ネストを抜けて外側に戻ったノードは quote_depth=1";
            checked = true;
        }
    }
    EXPECT_TRUE(checked);
}

// ---- バグ #11: Unicode追加面のエンティティ ----

TEST(Parser, HtmlEntitySupplementaryPlane)
{
    // U+1F600 = ニコニコ顔の絵文字（追加面）
    auto nodes = ParseNodes("&#x1F600;");
    ASSERT_EQ(nodes.size(), 1u);
    // UTF-8 4 byte: F0 9F 98 80
    ASSERT_EQ(nodes[0].GetText().size(), 4u);
    EXPECT_EQ(static_cast<unsigned char>(nodes[0].GetText()[0]), 0xF0);
    EXPECT_EQ(static_cast<unsigned char>(nodes[0].GetText()[1]), 0x9F);
    EXPECT_EQ(static_cast<unsigned char>(nodes[0].GetText()[2]), 0x98);
    EXPECT_EQ(static_cast<unsigned char>(nodes[0].GetText()[3]), 0x80);
}

TEST(Parser, HtmlEntityDecimalSupplementaryPlane)
{
    // U+1F4A9 = 128169 decimal
    auto nodes = ParseNodes("&#128169;");
    ASSERT_EQ(nodes.size(), 1u);
    // UTF-8 4 byte: F0 9F 92 A9
    ASSERT_EQ(nodes[0].GetText().size(), 4u);
    EXPECT_EQ(static_cast<unsigned char>(nodes[0].GetText()[0]), 0xF0);
    EXPECT_EQ(static_cast<unsigned char>(nodes[0].GetText()[1]), 0x9F);
    EXPECT_EQ(static_cast<unsigned char>(nodes[0].GetText()[2]), 0x92);
    EXPECT_EQ(static_cast<unsigned char>(nodes[0].GetText()[3]), 0xA9);
}

TEST(Parser, HtmlEntityBmpStillWorks)
{
    // U+00A9 = 著作権記号（基本多言語面）
    auto nodes = ParseNodes("&#xA9;");
    ASSERT_EQ(nodes.size(), 1u);
    // UTF-8 2 byte: C2 A9
    ASSERT_EQ(nodes[0].GetText().size(), 2u);
    EXPECT_EQ(static_cast<unsigned char>(nodes[0].GetText()[0]), 0xC2);
    EXPECT_EQ(static_cast<unsigned char>(nodes[0].GetText()[1]), 0xA9);
}

TEST(Parser, HtmlEntityBeyondUnicode)
{
    // U+110000はUnicodeの最大値を超えている; 無視されるべき
    auto nodes = ParseNodes("&#x110000;");
    ASSERT_EQ(nodes.size(), 1u);
    // 生のエンティティテキストとしてそのまま渡されるべき
    EXPECT_NE(nodes[0].GetText().find("110000"), std::string::npos);
}

// ---- リスト ----

TEST(Parser, OrderedList)
{
    auto nodes = ParseNodes("1. first\n2. second\n3. third");
    ASSERT_EQ(nodes.size(), 3u);
    for (const auto& node : nodes) {
        EXPECT_EQ(node.type, NodeType::ListItem);
        EXPECT_TRUE(node.list_ordered());
    }
    EXPECT_EQ(nodes[0].list_number(), 1);
    EXPECT_EQ(nodes[1].list_number(), 2);
    EXPECT_EQ(nodes[2].list_number(), 3);
}

TEST(Parser, OrderedListStartsFromN)
{
    auto nodes = ParseNodes("5. five\n6. six");
    ASSERT_EQ(nodes.size(), 2u);
    EXPECT_EQ(nodes[0].list_number(), 5);
    EXPECT_EQ(nodes[1].list_number(), 6);
}

// `0.` 始まりの順序リスト: 旧実装の list_number()>0 プロキシでは unordered と誤認される。
// ordered フラグと番号 0 の保持を検証する (PR#249 Copilot レビュー指摘の回帰テスト)。
TEST(Parser, OrderedListStartsFromZero)
{
    auto nodes = ParseNodes("0. zero\n1. one");
    ASSERT_EQ(nodes.size(), 2u);
    EXPECT_EQ(nodes[0].type, NodeType::ListItem);
    EXPECT_TRUE(nodes[0].list_ordered());
    EXPECT_EQ(nodes[0].list_number(), 0);
    EXPECT_TRUE(nodes[1].list_ordered());
    EXPECT_EQ(nodes[1].list_number(), 1);
}

TEST(Parser, ListIndentLevel)
{
    auto nodes = ParseNodes("- item");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_GT(nodes[0].indent_level, 0);
}

// ---- タスクリスト ----

TEST(Parser, TaskListChecked)
{
    auto nodes = ParseNodes("- [x] done");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::TaskListItem);
    EXPECT_TRUE(nodes[0].task_checked());
    EXPECT_EQ(nodes[0].GetText(), "done");
}

TEST(Parser, TaskListUnchecked)
{
    auto nodes = ParseNodes("- [ ] todo");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::TaskListItem);
    EXPECT_FALSE(nodes[0].task_checked());
}

TEST(Parser, TaskListUpperX)
{
    auto nodes = ParseNodes("- [X] also done");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_TRUE(nodes[0].task_checked());
}

// ---- 引用ブロック ----

TEST(Parser, BlockQuoteIndentLevel)
{
    auto nodes = ParseNodes("> quoted");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_GT(nodes[0].indent_level, 0);
}

// ---- テーブル ----

// NodeTableData の concat 構造が parser で正しく populate されることを確認。
TEST(Parser, TableConcatStructure)
{
    auto nodes = ParseNodes(
                     "| A | B |\n"
                     "|---|---|\n"
                     "| 1 | 2 |");
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].type, NodeType::Table);
    const auto* tbl = nodes[0].table_data();
    ASSERT_NE(tbl, nullptr);
    EXPECT_EQ(tbl->row_count, 2u);
    EXPECT_EQ(tbl->col_count, 2u);
    EXPECT_EQ(tbl->concat_text, "A\tB\n1\t2");
    ASSERT_EQ(tbl->cell_text_starts.size(), 5u);
    EXPECT_EQ(tbl->cell_text_starts[0], 0u);
    EXPECT_EQ(tbl->cell_text_starts[1], 2u);
    EXPECT_EQ(tbl->cell_text_starts[2], 4u);
    EXPECT_EQ(tbl->cell_text_starts[3], 6u);
    EXPECT_EQ(tbl->cell_text_starts[4], 7u);
    ASSERT_EQ(tbl->cell_run_starts.size(), 5u);
    ASSERT_EQ(tbl->is_header_row.size(), 2u);
    EXPECT_TRUE(tbl->is_header_row[0]);
    EXPECT_FALSE(tbl->is_header_row[1]);
    EXPECT_EQ(tbl->aligns.size(), 2u);
    EXPECT_EQ(tbl->GetCellText(0, 0), "A");
    EXPECT_EQ(tbl->GetCellText(0, 1), "B");
    EXPECT_EQ(tbl->GetCellText(1, 0), "1");
    EXPECT_EQ(tbl->GetCellText(1, 1), "2");
}

TEST(Parser, TableHeaderCells)
{
    auto nodes = ParseNodes(
                     "| H1 | H2 |\n"
                     "|---|---|\n"
                     "| D1 | D2 |");
    ASSERT_EQ(nodes.size(), 1u);
    const auto* tbl = nodes[0].table_data();
    ASSERT_GE(tbl->row_count, 2u);
    EXPECT_EQ(tbl->col_count, 2u);

    // 最初の行はヘッダーであるべき
    EXPECT_TRUE(tbl->IsHeaderRow(0));
    EXPECT_EQ(tbl->GetCellText(0, 0), "H1");
    EXPECT_EQ(tbl->GetCellText(0, 1), "H2");

    // 2番目の行はヘッダーではないべき
    EXPECT_FALSE(tbl->IsHeaderRow(1));
}

TEST(Parser, TableAlignment)
{
    auto nodes = ParseNodes(
                     "| L | C | R |\n"
                     "|:--|:--:|--:|\n"
                     "| a | b | c |");
    ASSERT_EQ(nodes.size(), 1u);
    // データ行の配置を確認（配置はMD_BLOCK_TD_DETAILから取得、列単位）
    const auto* tbl = nodes[0].table_data();
    ASSERT_GE(tbl->row_count, 2u);
    ASSERT_EQ(tbl->col_count, 3u);
    EXPECT_EQ(tbl->ColAlign(0), TableAlign::Left);
    EXPECT_EQ(tbl->ColAlign(1), TableAlign::Center);
    EXPECT_EQ(tbl->ColAlign(2), TableAlign::Right);
}

TEST(Parser, TableMultipleRows)
{
    auto nodes = ParseNodes(
                     "| A |\n"
                     "|---|\n"
                     "| 1 |\n"
                     "| 2 |\n"
                     "| 3 |");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].table_data()->row_count, 4u); // 1 header + 3 data
}

// ---- HTMLエンティティ ----

TEST(Parser, HtmlEntityLtGt)
{
    auto nodes = ParseNodes("&lt;tag&gt;");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_NE(nodes[0].GetText().find("<"), std::string::npos);
    EXPECT_NE(nodes[0].GetText().find(">"), std::string::npos);
}

// ---- 複雑なドキュメント ----

TEST(Parser, ComplexDocumentNodeCount)
{
    auto nodes = ParseNodes(
                     "# Title\n\n"
                     "Paragraph.\n\n"
                     "## Section\n\n"
                     "- item1\n"
                     "- item2\n\n"
                     "---\n\n"
                     "> quote\n\n"
                     "```\ncode\n```\n");
    // タイトル、段落、セクション、item1、item2、水平線、引用、コード
    EXPECT_GE(nodes.size(), 7u);
}

TEST(Parser, NodeTypesInComplexDocument)
{
    auto nodes = ParseNodes(
                     "# H\n\nP\n\n- L\n\n---\n\n> Q\n\n```\nC\n```\n");
    bool has_heading = false, has_para = false, has_list = false;
    bool has_hr = false, has_quote = false, has_code = false;
    for (const auto& n : nodes) {
        if (n.type == NodeType::Heading)
            has_heading = true;
        if (n.type == NodeType::Paragraph)
            has_para = true;
        if (n.type == NodeType::ListItem)
            has_list = true;
        if (n.type == NodeType::HorizontalRule)
            has_hr = true;
        if (n.type == NodeType::BlockQuote)
            has_quote = true;
        if (n.type == NodeType::CodeBlock)
            has_code = true;
    }
    EXPECT_TRUE(has_heading);
    EXPECT_TRUE(has_para);
    EXPECT_TRUE(has_list);
    EXPECT_TRUE(has_hr);
    EXPECT_TRUE(has_quote);
    EXPECT_TRUE(has_code);
}

// ---- UTF-8処理 ----

TEST(Parser, JapaneseText)
{
    auto nodes = ParseNodes("日本語テスト");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), "日本語テスト");
}

TEST(Parser, EmojiText)
{
    auto nodes = ParseNodes("Hello 🎉");
    ASSERT_EQ(nodes.size(), 1u);
    // クラッシュせず出力が生成されることだけを確認
    EXPECT_FALSE(nodes[0].GetText().empty());
}

// ---- ラン位置の整合性 ----

TEST(Parser, RunPositionsAreValid)
{
    auto nodes = ParseNodes("normal **bold** `code` *italic*");
    ASSERT_EQ(nodes.size(), 1u);
    const auto& node = nodes[0];
    for (const auto& run : node.runs()) {
        EXPECT_LE(run.start, node.GetText().size());
        EXPECT_LE(run.start + run.length, node.GetText().size());
    }
}

TEST(Parser, RunsOmitPlainText)
{
    auto nodes = ParseNodes("aaa **bbb** ccc");
    ASSERT_EQ(nodes.size(), 1u);
    const auto& runs = nodes[0].runs();
    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs[0].start, 4u);
    EXPECT_EQ(runs[0].length, 3u);
    EXPECT_TRUE(runs[0].bold());
}

TEST(Parser, PlainParagraphAndCodeBlockHaveNoRuns)
{
    auto nodes = ParseNodes("plain text\n\n```cpp\nint x;\n```");
    ASSERT_EQ(nodes.size(), 2u);
    EXPECT_TRUE(nodes[0].runs().empty());
    EXPECT_TRUE(nodes[1].runs().empty());
    EXPECT_EQ(nodes[1].GetText(), "int x;");
}

// ---- ネストされたリスト ----

TEST(Parser, NestedUnorderedList)
{
    auto nodes = ParseNodes("- a\n  - b\n    - c");
    ASSERT_GE(nodes.size(), 3u);
    // より深いアイテムはより高いインデントレベルを持つべき
    EXPECT_LT(nodes[0].indent_level, nodes[1].indent_level);
    EXPECT_LT(nodes[1].indent_level, nodes[2].indent_level);
}

TEST(Parser, NestedOrderedList)
{
    auto nodes = ParseNodes("1. a\n   1. b\n      1. c");
    ASSERT_GE(nodes.size(), 3u);
    EXPECT_EQ(nodes[0].list_number(), 1);
    EXPECT_EQ(nodes[1].list_number(), 1);
    EXPECT_EQ(nodes[2].list_number(), 1);
    EXPECT_LT(nodes[0].indent_level, nodes[1].indent_level);
}

TEST(Parser, MixedListNesting)
{
    auto nodes = ParseNodes("1. ordered\n   - unordered\n   - unordered2");
    ASSERT_GE(nodes.size(), 3u);
    EXPECT_GT(nodes[0].list_number(), 0);
    EXPECT_EQ(nodes[1].list_number(), 0);
    EXPECT_EQ(nodes[2].list_number(), 0);
}

// ---- インライン書式付きテーブル ----

TEST(Parser, TableCellWithBold)
{
    auto nodes = ParseNodes(
                     "| A | **B** |\n"
                     "|---|---|\n"
                     "| 1 | 2 |");
    ASSERT_EQ(nodes.size(), 1u);
    const auto* tbl = nodes[0].table_data();
    ASSERT_GE(tbl->row_count, 1u);
    ASSERT_GE(tbl->col_count, 2u);
    // 2番目のヘッダーセルは太字ランを持つべき
    bool has_bold = false;
    for (const auto& run : tbl->GetCellRuns(0, 1)) {
        if (run.bold())
            has_bold = true;
    }
    EXPECT_TRUE(has_bold);
}

// ---- リンク付きテーブル ----

TEST(Parser, TableCellWithLink)
{
    auto nodes = ParseNodes(
                     "| Name | Link |\n"
                     "|------|------|\n"
                     "| foo | [bar](https://example.com) |");
    ASSERT_EQ(nodes.size(), 1u);
    const auto* tbl = nodes[0].table_data();
    ASSERT_GE(tbl->row_count, 2u);
    ASSERT_GE(tbl->col_count, 2u);

    // リンクセルはランにlink_urlを持つべき
    bool found_link = false;
    for (const auto& run : tbl->GetCellRuns(1, 1)) {
        if (run.has_link()) {
            EXPECT_EQ(nodes[0].view_link_urls()[run.link_url_index], "https://example.com");
            found_link = true;
        }
    }
    EXPECT_TRUE(found_link);

    // リンクでないセルはリンクを持たないべき
    for (const auto& run : tbl->GetCellRuns(1, 0)) {
        EXPECT_FALSE(run.has_link());
    }
}

TEST(Parser, TableCellWithInternalLink)
{
    auto nodes = ParseNodes(
                     "| Section |\n"
                     "|---------|\n"
                     "| [intro](#introduction) |");
    ASSERT_EQ(nodes.size(), 1u);
    const auto* tbl = nodes[0].table_data();
    ASSERT_GE(tbl->row_count, 2u);

    bool found = false;
    for (const auto& run : tbl->GetCellRuns(1, 0)) {
        if (run.has_link()) {
            EXPECT_EQ(nodes[0].view_link_urls()[run.link_url_index], "#introduction");
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(Parser, TableCellWithBoldLink)
{
    auto nodes = ParseNodes(
                     "| Link |\n"
                     "|------|\n"
                     "| [**bold**](https://example.com) |");
    ASSERT_EQ(nodes.size(), 1u);
    const auto* tbl = nodes[0].table_data();
    ASSERT_GE(tbl->row_count, 2u);

    bool has_bold_link = false;
    for (const auto& run : tbl->GetCellRuns(1, 0)) {
        if (run.bold() && run.has_link()) {
            has_bold_link = true;
        }
    }
    EXPECT_TRUE(has_bold_link);
}

TEST(Parser, TableCellMixedTextAndLink)
{
    auto nodes = ParseNodes(
                     "| Content |\n"
                     "|---------|\n"
                     "| before [link](https://example.com) after |");
    ASSERT_EQ(nodes.size(), 1u);
    const auto* tbl = nodes[0].table_data();
    ASSERT_GE(tbl->row_count, 2u);

    // リンク部分だけが run になり、前後の装飾なしテキストは run を持たない
    EXPECT_EQ(tbl->GetCellText(1, 0), "before link after");
    const auto runs = tbl->GetCellRuns(1, 0);
    ASSERT_EQ(runs.size(), 1u);
    EXPECT_TRUE(runs[0].has_link());
    EXPECT_EQ(runs[0].start, 7u);
    EXPECT_EQ(runs[0].length, 4u);
}

// ---- HTMLエンティティのエッジケース ----

TEST(Parser, HtmlEntityQuot)
{
    auto nodes = ParseNodes("&quot;hello&quot;");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_NE(nodes[0].GetText().find("\""), std::string::npos);
}

TEST(Parser, HtmlEntityNbsp)
{
    auto nodes = ParseNodes("a&nbsp;b");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_NE(nodes[0].GetText().find("\u00A0"), std::string::npos);
}

// ---- インライン書式付きリンク ----

TEST(Parser, BoldLink)
{
    auto nodes = ParseNodes("[**bold link**](https://example.com)");
    ASSERT_EQ(nodes.size(), 1u);
    bool has_bold_link = false;
    for (const auto& run : nodes[0].runs()) {
        if (run.bold() && run.has_link()) {
            has_bold_link = true;
        }
    }
    EXPECT_TRUE(has_bold_link);
}

// ---- 数値HTMLエンティティ ----

TEST(Parser, NumericEntityDecimal)
{
    auto nodes = ParseNodes("&#65;");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), "A");
}

TEST(Parser, NumericEntityHex)
{
    auto nodes = ParseNodes("&#x41;");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), "A");
}

TEST(Parser, NumericEntityJapanese)
{
    // &#x3042; = あ (Hiragana A)
    auto nodes = ParseNodes("&#x3042;");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), "\u3042");
}

// ---- 深いネスト ----

TEST(Parser, DeeplyNestedList)
{
    std::string md;
    md += "- L1\n";
    md += "  - L2\n";
    md += "    - L3\n";
    md += "      - L4\n";
    auto nodes = ParseNodes(md);
    ASSERT_GE(nodes.size(), 4u);
    // より深いレベルはより高いindent_levelを持つべき
    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GE(nodes[i].indent_level, nodes[i - 1].indent_level);
    }
}

// ---- 不揃いな列数のテーブル ----

TEST(Parser, TableUnevenColumns)
{
    // md4cがこれを処理する - 行内のセルが少ない場合
    auto nodes = ParseNodes(
                     "| A | B | C |\n"
                     "|---|---|---|\n"
                     "| 1 | 2 |\n");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Table);
    ASSERT_GE(nodes[0].table_data()->row_count, 2u);
}

// ---- Mermaid言語のコードブロック ----

TEST(Parser, MermaidCodeBlock)
{
    auto nodes = ParseNodes("```mermaid\ngraph TD;\n  A-->B;\n```");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    EXPECT_EQ(nodes[0].code_language(), SyntaxLanguage::Mermaid);
}

// ---- LaTeX display math ($$...$$) ----

TEST(Parser, LatexDisplayMathSingleLinePromotedToCodeBlock)
{
    auto result = ParseMarkdown("$$E = mc^2$$");
    ASSERT_EQ(result.nodes().size(), 1u);
    EXPECT_EQ(result.nodes()[0].type, NodeType::CodeBlock);
    EXPECT_EQ(result.nodes()[0].code_language(), SyntaxLanguage::LatexMath);
    EXPECT_EQ(result.nodes()[0].GetText(), "E = mc^2");
    // diagram_indices に登録される（描画パイプラインに流すため）
    ASSERT_EQ(result.diagram_indices.size(), 1u);
    EXPECT_EQ(result.diagram_indices[0], 0u);
}

TEST(Parser, LatexDisplayMathWithOtherContentFallsBackToText)
{
    // 段落内に数式以外の内容があるときは昇格せず、テキストとして残す
    auto nodes = ParseNodes("before $$x+y$$ after");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_NE(nodes[0].code_language(), SyntaxLanguage::LatexMath);
    // フォールバック時は $$ 区切りが復元されていること
    const std::string text(nodes[0].GetText());
    EXPECT_NE(text.find("$$x+y$$"), std::string::npos);
}

TEST(Parser, LatexMultipleDisplayMathInOneParagraphFallsBackToText)
{
    // 1段落に2つ以上の $$...$$ があるときは昇格しない
    auto nodes = ParseNodes("$$a$$ $$b$$");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
}

TEST(Parser, LatexInlineMathRemainsAsText)
{
    // インライン $...$ は昇格対象外。$ 記号を含む元のテキストとして扱う
    auto nodes = ParseNodes("value is $x$ here");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_NE(nodes[0].code_language(), SyntaxLanguage::LatexMath);
    const std::string text(nodes[0].GetText());
    EXPECT_NE(text.find("$x$"), std::string::npos);
}

TEST(Parser, LatexDisplayMathInBlockquoteNotPromoted)
{
    // blockquote 内の $$...$$ は引用文脈維持のため昇格しない
    auto nodes = ParseNodes("> $$y=x$$");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::BlockQuote);
}

TEST(Parser, PlainDollarSignsNotMisdetectedAsMath)
{
    // "The price is $5 or $10." のような金額表記は LaTeX ではなくテキストとして扱う。
    // md4c は `$` の直後に空白・数字・記号が続く場合などインライン数式として解釈しないため、
    // フラグ有効化で既存のドル記号テキストが壊れないことを確認する。
    auto nodes = ParseNodes("The price is $5 or $10.");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_NE(nodes[0].code_language(), SyntaxLanguage::LatexMath);
    const std::string text(nodes[0].GetText());
    EXPECT_NE(text.find("$5"), std::string::npos);
    EXPECT_NE(text.find("$10"), std::string::npos);
}

TEST(Parser, LatexDisplayMathMultiline)
{
    // 複数行にわたる $$...$$ でも昇格される（中身はパーサがそのまま保持）
    auto result = ParseMarkdown("$$\nE = mc^2\n$$");
    ASSERT_EQ(result.nodes().size(), 1u);
    EXPECT_EQ(result.nodes()[0].type, NodeType::CodeBlock);
    EXPECT_EQ(result.nodes()[0].code_language(), SyntaxLanguage::LatexMath);
    // 中身に 'E = mc^2' が含まれること（md4c の改行扱いに依存するが、式本体は保持される）
    const auto& body = result.nodes()[0].GetText();
    EXPECT_NE(body.find("E = mc^2"), std::string::npos);
}

TEST(Parser, LatexDisplayMathLineCountMatchesNewlines)
{
    // OnText で事前集計した display_math_newlines を line_count に流用しているため、
    // 旧実装 (current_text 全走査の std::ranges::count) と同じ値を保つことを回帰テストする。
    const std::string_view sources[] = {
        "$$E = mc^2$$",
        "$$\nE = mc^2\n$$",
        "$$\na\nb\nc\n$$",
    };
    for (const auto src : sources) {
        SCOPED_TRACE(std::string{ src });
        auto result = ParseMarkdown(src);
        ASSERT_EQ(result.nodes().size(), 1u);
        ASSERT_EQ(result.nodes()[0].type, NodeType::CodeBlock);
        ASSERT_EQ(result.nodes()[0].code_language(), SyntaxLanguage::LatexMath);
        const auto text = result.nodes()[0].GetText();
        const auto expected = static_cast<int>(std::ranges::count(text, '\n'));
        EXPECT_EQ(result.nodes()[0].line_count, expected);
    }
}

TEST(Parser, LatexDisplayMathSurroundingParagraphs)
{
    // 前後に通常段落がある場合も、純粋な $$...$$ 段落のみ昇格される
    auto result = ParseMarkdown("before\n\n$$E=mc^2$$\n\nafter");
    ASSERT_EQ(result.nodes().size(), 3u);
    EXPECT_EQ(result.nodes()[0].type, NodeType::Paragraph);
    EXPECT_EQ(result.nodes()[1].type, NodeType::CodeBlock);
    EXPECT_EQ(result.nodes()[1].code_language(), SyntaxLanguage::LatexMath);
    EXPECT_EQ(result.nodes()[1].GetText(), "E=mc^2");
    EXPECT_EQ(result.nodes()[2].type, NodeType::Paragraph);
    ASSERT_EQ(result.diagram_indices.size(), 1u);
    EXPECT_EQ(result.diagram_indices[0], 1u);
}

TEST(Parser, MathFencedCodeBlockIsLatexDiagram)
{
    // GitHub / GitLab 互換の ```math も $$...$$ と同じく図として描画する
    auto result = ParseMarkdown("before\n\n```math\n\\frac{a}{b}\n\\sqrt{x}\n```");
    ASSERT_EQ(result.nodes().size(), 2u);
    EXPECT_EQ(result.nodes()[1].type, NodeType::CodeBlock);
    EXPECT_EQ(result.nodes()[1].code_language(), SyntaxLanguage::LatexMath);
    EXPECT_EQ(result.nodes()[1].GetText(), "\\frac{a}{b}\n\\sqrt{x}");
    ASSERT_EQ(result.diagram_indices.size(), 1u);
    EXPECT_EQ(result.diagram_indices[0], 1u);
}

// ---- 特殊文字を含むURL ----

TEST(Parser, LinkWithSpecialCharsInUrl)
{
    auto nodes = ParseNodes("[link](https://example.com/path?q=1&r=2#frag)");
    ASSERT_EQ(nodes.size(), 1u);
    bool found = false;
    for (const auto& run : nodes[0].runs()) {
        if (run.has_link()) {
            EXPECT_EQ(nodes[0].view_link_urls()[run.link_url_index], "https://example.com/path?q=1&r=2#frag");
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

// ---- 空の見出し ----

TEST(Parser, EmptyHeading)
{
    // "# " は CommonMark 上の空 ATX 見出し。テキスト無しでも Heading ノードは残す。
    auto nodes = ParseNodes("# \n\ntext");
    ASSERT_EQ(nodes.size(), 2u);
    EXPECT_EQ(nodes[0].type, NodeType::Heading);
    EXPECT_EQ(nodes[0].heading_level(), 1);
    EXPECT_TRUE(nodes[0].GetText().empty());
    EXPECT_EQ(nodes[1].type, NodeType::Paragraph);
    EXPECT_EQ(nodes[1].GetText(), "text");
}

// ---- GitHub Alerts ----

TEST(Parser, AlertNoteDetected)
{
    auto nodes = ParseNodes("> [!NOTE]\n> This is a note");
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::BlockQuote);
    EXPECT_EQ(nodes[0].alert_type, AlertType::Note);
}

TEST(Parser, AlertTipDetected)
{
    auto nodes = ParseNodes("> [!TIP]\n> Helpful advice");
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].alert_type, AlertType::Tip);
}

TEST(Parser, AlertImportantDetected)
{
    auto nodes = ParseNodes("> [!IMPORTANT]\n> Key info");
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].alert_type, AlertType::Important);
}

TEST(Parser, AlertWarningDetected)
{
    auto nodes = ParseNodes("> [!WARNING]\n> Be careful");
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].alert_type, AlertType::Warning);
}

TEST(Parser, AlertCautionDetected)
{
    auto nodes = ParseNodes("> [!CAUTION]\n> Dangerous");
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].alert_type, AlertType::Caution);
}

TEST(Parser, AlertCaseInsensitive)
{
    auto nodes = ParseNodes("> [!note]\n> lower case");
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].alert_type, AlertType::Note);
}

TEST(Parser, AlertCaseMixed)
{
    auto nodes = ParseNodes("> [!Note]\n> mixed case");
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].alert_type, AlertType::Note);
}

// GitHub はマーカーと本文が同じ行にあると Alert にせず、通常の引用としてマーカーも表示する。
TEST(Parser, AlertMarkerWithTextOnSameLineIsPlainQuote)
{
    for (const char* md : { "> [!NOTE] same line", "> [!NOTE] same line\n> next" }) {
        SCOPED_TRACE(md);
        const auto nodes = ParseNodes(md);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_EQ(nodes[0].alert_type, AlertType::None);
        EXPECT_TRUE(nodes[0].GetText().starts_with("[!NOTE] same line"));
    }
}

// マーカー行の後ろが空白や強制改行でも、本文が次の行なら Alert になる。
TEST(Parser, AlertMarkerFollowedByTrailingSpacesIsAlert)
{
    for (const char* md : { "> [!NOTE] \n> body", "> [!NOTE]  \n> body", "> [!NOTE]\\\n> body" }) {
        SCOPED_TRACE(md);
        const auto nodes = ParseNodes(md);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_EQ(nodes[0].alert_type, AlertType::Note);
        const auto label = std::string{ GetAlertIcon(AlertType::Note) } + " " + std::string{ GetAlertLabel(AlertType::Note) };
        EXPECT_EQ(nodes[0].GetText(), label + "\nbody");
        EXPECT_TRUE(ParseInvariantsHold(md, nodes));
    }
}

// Alert でない引用のソフト改行は従来どおり空白になる。
TEST(Parser, SoftBreakAfterNonAlertMarkerStaysSpace)
{
    for (const char* md : { "> [!NOTX]\n> body", "> intro\n>\n> [!NOTE]\n> body" }) {
        SCOPED_TRACE(md);
        const auto nodes = ParseNodes(md);
        EXPECT_TRUE(nodes.back().GetText().ends_with("] body"));
    }
}

TEST(Parser, AlertMarkerStrippedAndLabelInserted)
{
    auto nodes = ParseNodes("> [!NOTE]\n> Content here");
    ASSERT_GE(nodes.size(), 1u);
    // マーカー "[!NOTE]" が除去され、アイコン + ラベル "Note" に置換されているべき
    EXPECT_NE(nodes[0].GetText().find("Note"), std::string::npos);
    EXPECT_EQ(nodes[0].GetText().find("[!NOTE]"), std::string::npos);
    // 先頭はアイコン文字列であるべき
    auto icon = GetAlertIcon(AlertType::Note);
    EXPECT_EQ(nodes[0].GetText().substr(0, icon.size()), icon);
    // コンテンツも残っているべき
    EXPECT_NE(nodes[0].GetText().find("Content here"), std::string::npos);
}

TEST(Parser, AlertLabelIsBold)
{
    auto nodes = ParseNodes("> [!NOTE]\n> Some text");
    ASSERT_GE(nodes.size(), 1u);
    ASSERT_GE(nodes[0].runs().size(), 1u);
    // 最初のランはラベル部分で太字、アイコン絵文字は含まない
    const auto icon_len = static_cast<uint32_t>(GetAlertIcon(AlertType::Note).size());
    EXPECT_TRUE(nodes[0].runs()[0].bold());
    EXPECT_EQ(nodes[0].runs()[0].start, icon_len);
    EXPECT_EQ(nodes[0].runs()[0].start + nodes[0].runs()[0].length, nodes[0].alert_label_length());
}

TEST(Parser, AlertLabelLength)
{
    // alert_label_length は UTF-8 byte。ℹ (U+2139) は 3 byte、❗ (U+2757) も 3 byte。
    auto nodes = ParseNodes("> [!NOTE]\n> text");
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].alert_label_length(), 8u); // ℹ 3 + ' ' 1 + "NOTE" 4

    auto nodes2 = ParseNodes("> [!IMPORTANT]\n> text");
    ASSERT_GE(nodes2.size(), 1u);
    EXPECT_EQ(nodes2[0].alert_label_length(), 13u); // ❗ 3 + ' ' 1 + "IMPORTANT" 9
}

TEST(Parser, AlertRunPositionsAreValid)
{
    auto nodes = ParseNodes("> [!WARNING]\n> Some **bold** text");
    ASSERT_GE(nodes.size(), 1u);
    const auto& node = nodes[0];
    for (const auto& run : node.runs()) {
        EXPECT_LE(run.start + run.length, static_cast<uint32_t>(node.GetText().size()))
            << "ラン [" << run.start << ", " << run.start + run.length
            << ") がテキスト長 " << node.GetText().size() << " を超えている";
    }
}

TEST(Parser, AlertMultiParagraphGrouping)
{
    auto nodes = ParseNodes("> [!NOTE]\n> First para\n>\n> Second para");
    // 複数の BlockQuote ノードが生成され、すべて同じ alert_type を持つべき
    int alert_count = 0;
    for (const auto& node : nodes) {
        if (node.type == NodeType::BlockQuote && node.alert_type == AlertType::Note) {
            alert_count++;
        }
    }
    EXPECT_GE(alert_count, 2) << "複数段落のAlertは全ノードに伝播されるべき";
}

TEST(Parser, AlertOnlyFirstNodeHasLabel)
{
    auto nodes = ParseNodes("> [!TIP]\n> First\n>\n> Second");
    // 最初のノードだけ alert_label_length > 0
    int label_count = 0;
    for (const auto& node : nodes) {
        if (node.alert_label_length() > 0)
            label_count++;
    }
    EXPECT_EQ(label_count, 1);
}

TEST(Parser, RegularBlockquoteUnaffected)
{
    auto nodes = ParseNodes("> Just a normal quote");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::BlockQuote);
    EXPECT_EQ(nodes[0].alert_type, AlertType::None);
    EXPECT_EQ(nodes[0].alert_label_length(), 0u);
    EXPECT_EQ(nodes[0].GetText(), "Just a normal quote");
}

TEST(Parser, AlertMarkerOnlyNoContent)
{
    auto nodes = ParseNodes("> [!NOTE]");
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].alert_type, AlertType::Note);
    // マーカーだけの場合、アイコン + スペース + ラベルのみ残る
    std::string expected = std::string(GetAlertIcon(AlertType::Note)) + " Note";
    EXPECT_EQ(nodes[0].GetText(), expected);
}

TEST(Parser, AlertFollowedByRegularBlockquote)
{
    auto nodes = ParseNodes("> [!NOTE]\n> Alert text\n\n> Normal quote");
    // Alert と通常の blockquote が混在
    bool has_alert = false, has_normal = false;
    for (const auto& node : nodes) {
        if (node.type == NodeType::BlockQuote) {
            if (node.alert_type != AlertType::None)
                has_alert = true;
            else
                has_normal = true;
        }
    }
    EXPECT_TRUE(has_alert);
    EXPECT_TRUE(has_normal);
}

// GitHub は blockquote の先頭行のマーカーだけを Alert として扱う。
TEST(Parser, AlertMarkerAfterLeadingContentIsPlainQuote)
{
    for (const char* md : { "> intro\n>\n> [!NOTE] x", "> # T\n> [!NOTE] x", "> - item\n>\n> [!TIP]\n> y" }) {
        SCOPED_TRACE(md);
        auto nodes = ParseNodes(md);
        for (const auto& node : nodes) {
            EXPECT_EQ(node.alert_type, AlertType::None);
            EXPECT_EQ(node.alert_label_length(), 0u);
        }
        EXPECT_TRUE(nodes.back().GetText().starts_with("[!"));
    }
}

// マーカー直後の画像は md4c では同じ段落になる。マーカーと画像が別段落の場合と同じく、
// マーカーの引用ノード (Alert ラベル) と、Alert を引き継いだ Image ノードに分かれる。
TEST(Parser, AlertMarkerFollowedByImageStaysAlert)
{
    for (const char* md : { "> [!NOTE]\n> ![img](p.png)", "> [!NOTE]\n>\n> ![img](p.png)" }) {
        SCOPED_TRACE(md);
        const auto nodes = ParseNodes(md);
        ASSERT_EQ(nodes.size(), 2u);
        EXPECT_EQ(nodes[0].type, NodeType::BlockQuote);
        EXPECT_EQ(nodes[0].alert_type, AlertType::Note);
        const auto label = std::string{ GetAlertIcon(AlertType::Note) } + " " + std::string{ GetAlertLabel(AlertType::Note) };
        EXPECT_EQ(nodes[0].GetText(), label);
        ASSERT_EQ(nodes[1].type, NodeType::Image);
        ASSERT_NE(nodes[1].image_data(), nullptr);
        EXPECT_EQ(nodes[1].image_data()->src, "p.png");
        EXPECT_EQ(nodes[1].alert_type, AlertType::Note);
        EXPECT_EQ(nodes[1].blockquote_group, nodes[0].blockquote_group);
        EXPECT_TRUE(ParseInvariantsHold(md, nodes));
    }
}

// 分割はマーカーだけの先頭段落に限る。Alert でない引用や、先頭段落でないマーカーの画像は従来どおり。
TEST(Parser, ImageInQuoteWithoutLeadingAlertMarkerIsNotSplit)
{
    for (const char* md : { "> ![img](p.png)", "> [!NOTX]\n> ![img](p.png)", "> note ![img](p.png)", "> intro\n>\n> [!NOTE]\n> ![img](p.png)", "> [!NOTE] ![img](p.png)" }) {
        SCOPED_TRACE(md);
        const auto nodes = ParseNodes(md);
        ASSERT_FALSE(nodes.empty());
        EXPECT_EQ(nodes.back().type, NodeType::Image);
        for (const auto& node : nodes) {
            EXPECT_EQ(node.alert_type, AlertType::None);
        }
    }
}

// リンク付き画像は MD_SPAN_A が画像より先に来る。分割後の Image ノードでリンク URL が解決できること。
TEST(Parser, AlertMarkerFollowedByLinkedImageKeepsLinkOnImage)
{
    const char* md = "> [!NOTE]\n> [![badge](b.svg)](https://x.example)";
    const auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 2u);
    EXPECT_EQ(nodes[0].alert_type, AlertType::Note);
    ASSERT_EQ(nodes[1].type, NodeType::Image);
    EXPECT_EQ(nodes[1].alert_type, AlertType::Note);
    const auto urls = nodes[1].view_link_urls();
    bool has_link = false;
    for (const auto& run : nodes[1].runs()) {
        if (run.has_link()) {
            has_link = true;
            ASSERT_LT(static_cast<size_t>(run.link_url_index), urls.size());
            EXPECT_EQ(urls[static_cast<size_t>(run.link_url_index)], "https://x.example");
        }
    }
    EXPECT_TRUE(has_link);
    EXPECT_TRUE(ParseInvariantsHold(md, nodes));
}

// src が空の画像は Image に昇格しないので、分割せず Alert の本文として残す。
TEST(Parser, AlertMarkerFollowedByImageWithoutSrcStaysOneAlertNode)
{
    const auto nodes = ParseNodes("> [!NOTE]\n> ![alt]()");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::BlockQuote);
    EXPECT_EQ(nodes[0].alert_type, AlertType::Note);
    EXPECT_TRUE(nodes[0].GetText().ends_with("alt"));
}

// ネストを跨いだ Alert 伝播 (PR #156)

TEST(Parser, Alert_PropagatesAcrossNestedBlockquote)
{
    // example/nested.md #9 相当: 親 -> ネスト -> 親の続き
    auto nodes = ParseNodes(
                     "> [!NOTE]\n"
                     "> Alert head\n"
                     "> > nested\n"
                     ">\n"
                     "> Alert continues");
    bool checked_continuation = false;
    for (const auto& n : nodes) {
        if (n.GetText().find("Alert continues") != std::string::npos) {
            EXPECT_EQ(n.alert_type, AlertType::Note)
                << "ネストを抜けた後段でも Alert が継続する";
            checked_continuation = true;
        }
    }
    EXPECT_TRUE(checked_continuation);
}

TEST(Parser, Alert_IgnoredWhenStartedInsideNestedBlockquote)
{
    // GitHub 仕様: ネスト内 (`> > [!NOTE]`) の Alert マーカーは認識しない
    auto nodes = ParseNodes(
                     "> outer\n"
                     "> > [!NOTE]\n"
                     "> > inner note text");
    for (const auto& n : nodes) {
        EXPECT_EQ(n.alert_type, AlertType::None)
            << "ネスト内の Alert マーカーは無効化される";
        EXPECT_EQ(n.alert_label_length(), 0u);
    }
}

TEST(Parser, AlertUnknownTypeIgnored)
{
    auto nodes = ParseNodes("> [!UNKNOWN]\n> text");
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].alert_type, AlertType::None);
    // マーカーがそのまま残っているべき
    EXPECT_NE(nodes[0].GetText().find("UNKNOWN"), std::string::npos);
}

TEST(Parser, AlertLabelContents)
{
    // 各AlertTypeのラベル文字列を確認
    EXPECT_EQ(GetAlertLabel(AlertType::Note), "Note");
    EXPECT_EQ(GetAlertLabel(AlertType::Tip), "Tip");
    EXPECT_EQ(GetAlertLabel(AlertType::Important), "Important");
    EXPECT_EQ(GetAlertLabel(AlertType::Warning), "Warning");
    EXPECT_EQ(GetAlertLabel(AlertType::Caution), "Caution");
    EXPECT_EQ(GetAlertLabel(AlertType::None), "");
}

// ノードは ParseResult の arena を指すので、一時オブジェクトからノードだけを参照できないこと。
template <class R>
concept HasNodes = requires(R&& r) { std::forward<R>(r).nodes(); };
static_assert(HasNodes<ParseResult&>);
static_assert(!HasNodes<ParseResult>);

TEST(Parser, DetectAlertsOnEmptyVector)
{
    std::pmr::vector<Node> nodes;
    TextArena arena;
    DetectAlerts(nodes, arena, {}); // クラッシュしないべき
    EXPECT_TRUE(nodes.empty());
}

TEST(Parser, AlertWithInlineFormatting)
{
    auto nodes = ParseNodes("> [!TIP]\n> Use **bold** and `code`");
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].alert_type, AlertType::Tip);
    // テキストにboldとcodeが含まれるべき
    EXPECT_NE(nodes[0].GetText().find("bold"), std::string::npos);
    EXPECT_NE(nodes[0].GetText().find("code"), std::string::npos);
    // フォーマット用のランがあるべき
    bool has_bold = false, has_code = false;
    for (const auto& run : nodes[0].runs()) {
        if (run.bold() && run.start > 0)
            has_bold = true; // ラベル以外の太字
        if (run.code())
            has_code = true;
    }
    EXPECT_TRUE(has_bold);
    EXPECT_TRUE(has_code);
}

TEST(Parser, AlertTextStartsWithLabelThenNewline)
{
    auto nodes = ParseNodes("> [!CAUTION]\n> Don't do this");
    ASSERT_GE(nodes.size(), 1u);
    // テキストは "[icon] Caution\n..." の形式であるべき
    const auto& text = nodes[0].GetText();
    auto nl = text.find('\n');
    ASSERT_NE(nl, std::string::npos);
    std::string expected = std::string(GetAlertIcon(AlertType::Caution)) + " Caution";
    EXPECT_EQ(text.substr(0, nl), std::string_view(expected));
}

// ---- ソースオフセット ----

// source_offset は入力 string_view (= 引数のリテラル) 先頭からの byte offset。
TEST(Parser, SourceOffsetSingleParagraph)
{
    std::string_view md = "Hello world";
    auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].SourceOffset(), 0u);
}

TEST(Parser, SourceOffsetHeading)
{
    // "# Title" → テキスト "Title" はオフセット 2 から
    std::string_view md = "# Title";
    auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].SourceOffset(), 2u);
}

TEST(Parser, SourceOffsetMultipleParagraphs)
{
    // "First\n\nSecond\n\nThird"
    // "First" = offset 0, "Second" = offset 7, "Third" = offset 15
    std::string_view md = "First\n\nSecond\n\nThird";
    auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(nodes[0].SourceOffset(), 0u);
    EXPECT_EQ(nodes[1].SourceOffset(), 7u);
    EXPECT_EQ(nodes[2].SourceOffset(), 15u);
}

TEST(Parser, SourceOffsetIncreasing)
{
    // ノードの source_offset は単調増加であるべき
    std::string_view md =
        "# Heading\n\n"
        "Paragraph\n\n"
        "- item1\n"
        "- item2\n\n"
        "```\ncode\n```\n\n"
        "End";
    auto nodes = ParseNodes(md);
    ASSERT_GE(nodes.size(), 3u);
    size_t prev = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const size_t off = nodes[i].SourceOffset();
        if (off != kUnsetSourceOffset) {
            EXPECT_GE(off, prev) << "ノード " << i << " の source_offset が前のノードより小さい";
            prev = off;
        }
    }
}

TEST(Parser, SourceOffsetCodeBlock)
{
    // "```\nhello\n```" → コードブロック内テキストのオフセット
    std::string_view md = "```\nhello\n```";
    auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    // コードブロックのテキスト "hello" は "```\n" = 4バイト目から
    EXPECT_EQ(nodes[0].SourceOffset(), 4u);
}

TEST(Parser, SourceOffsetHorizontalRule)
{
    // "---" はテキストを持たないのでsource_offsetは未設定のまま
    std::string_view md = "---";
    auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::HorizontalRule);
    EXPECT_EQ(nodes[0].SourceOffset(), kUnsetSourceOffset);
}

TEST(Parser, SourceOffsetCjkMultiCodeUnit)
{
    // source_offset は UTF-8 byte。
    std::string_view md = "あいう\n\ntest";
    auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 2u);
    EXPECT_EQ(nodes[0].SourceOffset(), 0u);
    EXPECT_EQ(nodes[1].SourceOffset(), 11u); // "あいう" 9 byte + "\n\n" 2
}

TEST(Parser, SourceOffsetUnorderedList)
{
    // "- A\n- B\n- C"
    // "- " = 2バイト, "A" offset=2, "\n- " = 3バイト, "B" offset=5+2=7?
    // 実際: "- A\n" = 4, "- B\n" = 4, "- C" = 3
    std::string_view md = "- A\n- B\n- C";
    auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(nodes[0].SourceOffset(), 2u);  // "A" = "- " の後
    EXPECT_EQ(nodes[1].SourceOffset(), 6u);  // "B" = "- A\n- " の後
    EXPECT_EQ(nodes[2].SourceOffset(), 10u); // "C" = "- A\n- B\n- " の後
}

TEST(Parser, SourceOffsetOrderedList)
{
    // "1. First\n2. Second"
    std::string_view md = "1. First\n2. Second";
    auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 2u);
    EXPECT_EQ(nodes[0].SourceOffset(), 3u);  // "First" = "1. " の後
    EXPECT_EQ(nodes[1].SourceOffset(), 12u); // "Second" = "1. First\n2. " の後
}

TEST(Parser, SourceOffsetBlockQuote)
{
    // "> quoted\n\nnormal"
    std::string_view md = "> quoted\n\nnormal";
    auto nodes = ParseNodes(md);
    ASSERT_GE(nodes.size(), 2u);
    EXPECT_EQ(nodes[0].SourceOffset(), 2u); // "quoted" = "> " の後
}

TEST(Parser, SourceOffsetNestedList)
{
    // ネストされたリストでも単調増加
    std::string_view md = "- outer\n  - inner\n- next";
    auto nodes = ParseNodes(md);
    ASSERT_GE(nodes.size(), 3u);
    size_t prev = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const size_t off = nodes[i].SourceOffset();
        if (off != kUnsetSourceOffset) {
            EXPECT_GE(off, prev) << "ノード " << i << " の offset が前のノードより小さい";
            prev = off;
        }
    }
}

TEST(Parser, SourceOffsetTable)
{
    // テーブルノードの source_offset はヘッダの最初のセルテキスト
    std::string_view md =
        "| A | B |\n"
        "|---|---|\n"
        "| 1 | 2 |";
    auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Table);
    // "| " の後の "A" = offset 2
    EXPECT_EQ(nodes[0].SourceOffset(), 2u);
}

TEST(Parser, SourceOffsetCodeBlockWithLanguage)
{
    // "```cpp\nint x;\n```" → テキストは "```cpp\n" = 7バイト目から
    std::string_view md = "```cpp\nint x;\n```";
    auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].SourceOffset(), 7u);
}

TEST(Parser, SourceOffsetMixedDocument)
{
    // 多様なブロック型を含む文書で全ノードの offset が有効かつ単調増加
    std::string md =
        "# Title\n\n"        // heading
        "Paragraph\n\n"      // paragraph
        "- item\n\n"         // list
        "> quote\n\n"        // blockquote
        "```\ncode\n```\n\n" // code
        "---\n\n"            // hr (offset 未設定)
        "End";               // paragraph
    auto nodes = ParseNodes(md);
    ASSERT_GE(nodes.size(), 6u);

    size_t prev = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const size_t off = nodes[i].SourceOffset();
        if (off != kUnsetSourceOffset) {
            EXPECT_GE(off, prev)
                << "ノード " << i << " (type="
                << static_cast<int>(nodes[i].type) << ") の offset が不正";
            EXPECT_LT(off, md.size())
                << "ノード " << i << " の offset がソース長を超えている";
            prev = off;
        }
    }
}

TEST(Parser, SourceOffsetTaskList)
{
    std::string_view md = "- [x] done\n- [ ] todo";
    auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 2u);
    // "- [x] " = 6バイト, "done" offset=6
    EXPECT_EQ(nodes[0].SourceOffset(), 6u);
    EXPECT_EQ(nodes[1].SourceOffset(), 17u); // "- [x] done\n- [ ] " = 17
}

// ---- line_count ----

TEST(Parser, LineCountSingleLine)
{
    auto nodes = ParseNodes("Hello world");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].line_count, 0);
}

TEST(Parser, LineCountCodeBlock)
{
    auto nodes = ParseNodes("```\na\nb\nc\n```");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    // "a\nb\nc" → 2個の改行（末尾の\nはパーサーが除去する）
    EXPECT_EQ(nodes[0].line_count, 2);
}

TEST(Parser, LineCountMultilineParagraph)
{
    // softbreakは空白に変換されるため改行にならない
    auto nodes = ParseNodes("line1\nline2\nline3");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].line_count, 0);
}

TEST(Parser, LineCountHardBreak)
{
    // 末尾2スペース+改行 = hard break → \n
    auto nodes = ParseNodes("line1  \nline2  \nline3");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].line_count, 2);
}

TEST(Parser, LineCountEmptyCodeBlock)
{
    auto nodes = ParseNodes("```\n\n```");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    EXPECT_EQ(nodes[0].line_count, 0);
}

TEST(Parser, LineCountLargeCodeBlock)
{
    std::string md = "```\n";
    for (int i = 0; i < 100; i++) {
        md += "line " + std::to_string(i) + "\n";
    }
    md += "```";
    auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 1u);
    // 100行 → 99個の改行（末尾の\nが除去される）
    EXPECT_EQ(nodes[0].line_count, 99);
}

// ---- UTF-8バッチ変換 ----

TEST(Parser, Utf8BatchWithSpanBoundary)
{
    auto nodes = ParseNodes("before **bold** after");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), "before bold after");
    ASSERT_EQ(nodes[0].runs().size(), 1u);
    EXPECT_TRUE(nodes[0].runs()[0].bold());
    EXPECT_EQ(nodes[0].runs()[0].start, 7u);
    EXPECT_EQ(nodes[0].runs()[0].length, 4u);
}

TEST(Parser, Utf8BatchWithEntity)
{
    auto nodes = ParseNodes("a &amp; b");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), "a & b");
}

TEST(Parser, Utf8BatchWithSoftBreak)
{
    auto nodes = ParseNodes("line1\nline2");
    ASSERT_EQ(nodes.size(), 1u);
    // softbreak → space
    EXPECT_EQ(nodes[0].GetText(), "line1 line2");
}

TEST(Parser, Utf8BatchWithHardBreak)
{
    auto nodes = ParseNodes("line1  \nline2");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), "line1\nline2");
}

TEST(Parser, Utf8BatchMixedAsciiAndMultibyte)
{
    auto nodes = ParseNodes("Hello **世界** test");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), "Hello 世界 test");
    ASSERT_EQ(nodes[0].runs().size(), 1u);
    EXPECT_TRUE(nodes[0].runs()[0].bold());
    EXPECT_EQ(nodes[0].runs()[0].start, 6u);
    EXPECT_EQ(nodes[0].runs()[0].length, 6u);
}

TEST(Parser, Utf8BatchCodeBlockContent)
{
    auto nodes = ParseNodes("```\nint x = 0;\nfloat y = 1.0;\n```");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    EXPECT_EQ(nodes[0].GetText(), "int x = 0;\nfloat y = 1.0;");
}

TEST(Parser, Utf8BatchEntityBetweenText)
{
    auto nodes = ParseNodes("a&lt;b&gt;c");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), "a<b>c");
}

TEST(Parser, Utf8BatchNestedFormatting)
{
    auto nodes = ParseNodes("***bold and italic***");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), "bold and italic");
    ASSERT_GE(nodes[0].runs().size(), 1u);
    EXPECT_TRUE(nodes[0].runs()[0].bold());
    EXPECT_TRUE(nodes[0].runs()[0].italic());
}

TEST(Parser, Utf8BatchLinkText)
{
    auto nodes = ParseNodes("before [link text](https://example.com) after");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_NE(nodes[0].GetText().find("before"), std::string::npos);
    EXPECT_NE(nodes[0].GetText().find("link text"), std::string::npos);
    EXPECT_NE(nodes[0].GetText().find("after"), std::string::npos);
}

// ---- 協調キャンセル (AsyncLoadCoordinator 用) ----

TEST(Parser, ParseMarkdownStopTokenAlreadyRequestedAbortsEarly)
{
    // 大きめの markdown を用意 (キャンセルなしなら 1000 ノード以上構築されるはず)
    std::string markdown;
    markdown.reserve(64 * 1024);
    for (int i = 0; i < 1000; i++) {
        markdown += "# Heading " + std::to_string(i) + "\n\nSome paragraph text.\n\n";
    }

    std::stop_source ss;
    ss.request_stop();

    auto result = ParseMarkdown(markdown, ss.get_token());
    // 初回 OnEnterBlock で abort されるためノード列は空。
    EXPECT_TRUE(result.nodes().empty());
}

TEST(Parser, ParseMarkdownDefaultStopTokenWorksAsUsual)
{
    // default-constructed の stop_token は never stop_requested。従来動作を維持。
    auto result = ParseMarkdown("# Title\n\nBody");
    EXPECT_FALSE(result.nodes().empty());
}

// ---- Alert マーカー判定の補助関数 ----

// マーカーと直後の区切り 1 文字だけなら true。
TEST(Parser, IsAlertMarkerOnly)
{
    struct Case {
        std::string_view text;
        bool expected;
    };
    constexpr Case kCases[] = {
        { "[!NOTE]", true },
        { "[!note]", true },
        { "[!WARNING] ", false },
        { "[!TIP]\n", true },
        { "", false },
        { "[!NOTE] body", false },
        { "[!NOTE]  ", false },
        { "[!FOO]", false },
        { "[!]", false },
        { "[NOTE]", false },
        { " [!NOTE]", false },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(c.text));
        EXPECT_EQ(IsAlertMarkerOnly(c.text), c.expected);
    }
}

// 最外側 blockquote グループの先頭ノードだけが候補。
TEST(Parser, IsAlertHeadCandidateOnlyForFirstNodeOfOuterQuote)
{
    const auto nodes = ParseNodes("> first\n>\n> second\n\npara\n\n> other\n\n> > nested");
    ASSERT_EQ(nodes.size(), 5u);
    EXPECT_TRUE(IsAlertHeadCandidate(nodes, 0));
    EXPECT_FALSE(IsAlertHeadCandidate(nodes, 1)) << "同じ引用の 2 段落目";
    EXPECT_FALSE(IsAlertHeadCandidate(nodes, 2)) << "引用外の段落";
    EXPECT_TRUE(IsAlertHeadCandidate(nodes, 3)) << "別の引用グループの先頭";
    EXPECT_FALSE(IsAlertHeadCandidate(nodes, 4)) << "ネストした引用";
}

// ---- 脚注 ----

namespace {

// text 内の needle を覆う run のリンク URL。リンクでなければ空。
std::string LinkUrlAt(const Node& node, std::string_view needle)
{
    const auto pos = node.GetText().find(needle);
    if (pos == std::string_view::npos) {
        return "<not found>";
    }
    for (const auto& run : node.runs()) {
        if (run.start == pos && run.length == needle.size()) {
            return run.has_link() ? std::string(node.view_link_urls()[static_cast<size_t>(run.link_url_index)]) : std::string{};
        }
    }
    return "<no run>";
}

} // namespace

TEST(Parser, FootnoteRefBecomesNumberedLinkAndDefinitionMovesToEnd)
{
    const std::string md = "Text[^1].\n\n[^1]: Note **here**.\n\nAfter.";
    const auto result = ParseMarkdown(md);
    const auto& nodes = result.nodes();
    ASSERT_EQ(nodes.size(), 4u);
    EXPECT_EQ(nodes[0].GetText(), "Text[1].");
    EXPECT_EQ(LinkUrlAt(nodes[0], "[1]"), "#user-content-fn-1");
    EXPECT_EQ(nodes[1].GetText(), "After.");
    EXPECT_EQ(nodes[2].type, NodeType::HorizontalRule);
    EXPECT_EQ(nodes[3].type, NodeType::ListItem);
    EXPECT_TRUE(nodes[3].list_ordered());
    EXPECT_EQ(nodes[3].list_number(), 1);
    EXPECT_EQ(nodes[3].indent_level, 1);
    EXPECT_EQ(nodes[3].GetText(), "Note here. ↩");
    EXPECT_EQ(LinkUrlAt(nodes[3], "↩"), "#user-content-fnref-1");
    EXPECT_EQ(LinkUrlAt(nodes[3], "here"), "");
    EXPECT_FALSE(nodes[3].HasSourceOffset()) << "末尾に並ぶ定義はソース位置の単調増加を崩さないよう持たない";
    EXPECT_TRUE(ParseInvariantsHold(md, nodes));
    ASSERT_EQ(result.footnote_def_indices.size(), 1u);
    EXPECT_EQ(result.footnote_def_indices[0], 3);
}

TEST(Parser, FootnotesAreNumberedInReferenceOrder)
{
    const auto result = ParseMarkdown("A[^b] B[^a]\n\n[^a]: first\n[^b]: second");
    const auto& nodes = result.nodes();
    ASSERT_EQ(nodes.size(), 4u);
    EXPECT_EQ(nodes[0].GetText(), "A[1] B[2]");
    EXPECT_EQ(nodes[2].GetText(), "second ↩");
    EXPECT_EQ(nodes[2].list_number(), 1);
    EXPECT_EQ(nodes[3].GetText(), "first ↩");
    EXPECT_EQ(nodes[3].list_number(), 2);
}

TEST(Parser, FootnoteWithMultipleRefsGetsNumberedBackrefs)
{
    const auto result = ParseMarkdown("x[^1] y[^1]\n\n[^1]: n");
    const auto& nodes = result.nodes();
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(nodes[2].GetText(), "n ↩ ↩2");
    EXPECT_EQ(LinkUrlAt(nodes[2], "↩2"), "#user-content-fnref-1-2");
    ASSERT_EQ(result.footnote_refs.size(), 2u);
    EXPECT_EQ(result.footnote_refs[1].ref_id, 2u);
}

// md4c はテーブル行の判定でもインラインを解析して参照数を数えるため、ref_id / ref_count が実際の参照数とずれる。
TEST(Parser, FootnoteRefInTableIsCountedOnce)
{
    const std::string md = "| h |\n|---|\n| c[^1] |\n\nz[^1]\n\n[^1]: n";
    const auto result = ParseMarkdown(md);
    const auto& nodes = result.nodes();
    ASSERT_EQ(nodes.size(), 4u);
    EXPECT_EQ(nodes[0].table_data()->GetCellText(1, 0), "c[1]");
    EXPECT_EQ(nodes[3].GetText(), "n ↩ ↩2");
    ASSERT_EQ(result.footnote_refs.size(), 2u);
    EXPECT_EQ(result.footnote_refs[0].node_index, 0);
    EXPECT_EQ(result.footnote_refs[1].ref_id, 2u);
    EXPECT_TRUE(ParseInvariantsHold(md, nodes));
}

TEST(Parser, FootnoteRefInsideLaterDefinitionGetsBackref)
{
    const auto result = ParseMarkdown("a[^1] b[^2]\n\n[^1]: one\n[^2]: two[^1]");
    const auto& nodes = result.nodes();
    ASSERT_EQ(nodes.size(), 4u);
    EXPECT_EQ(nodes[2].GetText(), "one ↩ ↩2");
    EXPECT_EQ(nodes[3].GetText(), "two[1] ↩");
    ASSERT_EQ(result.footnote_refs.size(), 3u);
    EXPECT_EQ(result.footnote_refs[2].node_index, 3);
}

TEST(Parser, UndefinedFootnoteRefStaysPlainText)
{
    const auto result = ParseMarkdown("[^x] text");
    ASSERT_EQ(result.nodes().size(), 1u);
    EXPECT_EQ(result.nodes()[0].GetText(), "[^x] text");
    EXPECT_TRUE(result.footnote_refs.empty());
}

// GitHub と同じく、参照されない定義は表示しない。
TEST(Parser, UnreferencedFootnoteDefinitionIsHidden)
{
    const auto result = ParseMarkdown("Text\n\n[^1]: unused");
    ASSERT_EQ(result.nodes().size(), 1u);
    EXPECT_EQ(result.nodes()[0].GetText(), "Text");
    EXPECT_TRUE(result.footnote_def_indices.empty());
}

TEST(Parser, FootnoteRefInsideQuoteKeepsAlert)
{
    const auto nodes = ParseNodes("> [!NOTE]\n> See[^1]\n\n[^1]: n");
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(nodes[0].alert_type, AlertType::Note);
    EXPECT_NE(nodes[0].GetText().find("See[1]"), std::string_view::npos);
}
