#include <gtest/gtest.h>
#include "parser.h"

// ---- 基本的なパース ----

TEST(Parser, EmptyInputReturnsNoNodes)
{
    auto nodes = ParseMarkdown(MENDO_LIT("")).nodes;
    EXPECT_TRUE(nodes.empty());
}

TEST(Parser, WhitespaceOnlyReturnsNoNodes)
{
    auto nodes = ParseMarkdown(MENDO_LIT("   \n\n  ")).nodes;
    EXPECT_TRUE(nodes.empty());
}

TEST(Parser, SingleParagraph)
{
    auto nodes = ParseMarkdown(MENDO_LIT("Hello world")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("Hello world"));
}

TEST(Parser, MultipleParagraphs)
{
    auto nodes = ParseMarkdown(MENDO_LIT("First\n\nSecond\n\nThird")).nodes;
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_EQ(nodes[1].type, NodeType::Paragraph);
    EXPECT_EQ(nodes[2].type, NodeType::Paragraph);
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("First"));
    EXPECT_EQ(nodes[1].GetText(), MENDO_LIT("Second"));
    EXPECT_EQ(nodes[2].GetText(), MENDO_LIT("Third"));
}

// ---- 見出し ----

TEST(Parser, HeadingH1)
{
    auto nodes = ParseMarkdown(MENDO_LIT("# Title")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Heading);
    EXPECT_EQ(nodes[0].heading_level, 1);
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("Title"));
}

TEST(Parser, HeadingH2)
{
    auto nodes = ParseMarkdown(MENDO_LIT("## Subtitle")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].heading_level, 2);
}

TEST(Parser, HeadingH3ToH6)
{
    for (int level = 3; level <= 6; level++) {
        mendo::doc_string_std md(level, MENDO_LIT('#'));
        md += MENDO_LIT(" Test");
        auto nodes = ParseMarkdown(md).nodes;
        ASSERT_EQ(nodes.size(), 1u) << "level=" << level;
        EXPECT_EQ(nodes[0].heading_level, level) << "level=" << level;
    }
}

TEST(Parser, HeadingAnchorId)
{
    auto nodes = ParseMarkdown(MENDO_LIT("# Hello World")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].anchor_id(), MENDO_LIT("hello-world"));
}

TEST(Parser, HeadingAnchorIdCjk)
{
    auto nodes = ParseMarkdown(MENDO_LIT("## コードブロック")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].anchor_id(), MENDO_LIT("コードブロック"));
}

// ---- インライン書式 ----

TEST(Parser, BoldText)
{
    auto nodes = ParseMarkdown(MENDO_LIT("**bold**")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("bold"));
    ASSERT_GE(nodes[0].runs.size(), 1u);
    EXPECT_TRUE(nodes[0].runs[0].bold());
    EXPECT_FALSE(nodes[0].runs[0].italic());
}

TEST(Parser, ItalicText)
{
    auto nodes = ParseMarkdown(MENDO_LIT("*italic*")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("italic"));
    ASSERT_GE(nodes[0].runs.size(), 1u);
    EXPECT_TRUE(nodes[0].runs[0].italic());
    EXPECT_FALSE(nodes[0].runs[0].bold());
}

TEST(Parser, BoldItalicText)
{
    auto nodes = ParseMarkdown(MENDO_LIT("***bolditalic***")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_GE(nodes[0].runs.size(), 1u);
    EXPECT_TRUE(nodes[0].runs[0].bold());
    EXPECT_TRUE(nodes[0].runs[0].italic());
}

TEST(Parser, InlineCode)
{
    auto nodes = ParseMarkdown(MENDO_LIT("`code`")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("code"));
    ASSERT_GE(nodes[0].runs.size(), 1u);
    EXPECT_TRUE(nodes[0].runs[0].code());
}

TEST(Parser, StrikethroughText)
{
    auto nodes = ParseMarkdown(MENDO_LIT("~~deleted~~")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_GE(nodes[0].runs.size(), 1u);
    EXPECT_TRUE(nodes[0].runs[0].strikethrough());
}

TEST(Parser, MixedFormattingPreservesOrder)
{
    auto nodes = ParseMarkdown(MENDO_LIT("normal **bold** normal")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    // 少なくとも3つのランを持つべき: "normal ", "bold", " normal"
    ASSERT_GE(nodes[0].runs.size(), 3u);
    EXPECT_FALSE(nodes[0].runs[0].bold());
    EXPECT_TRUE(nodes[0].runs[1].bold());
    EXPECT_FALSE(nodes[0].runs[2].bold());
}

// ---- リンク ----

TEST(Parser, ExternalLink)
{
    auto nodes = ParseMarkdown(MENDO_LIT("[text](https://example.com)")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_GE(nodes[0].runs.size(), 1u);
    ASSERT_TRUE(nodes[0].runs[0].has_link());
    EXPECT_EQ(nodes[0].view_link_urls()[nodes[0].runs[0].link_url_index], MENDO_LIT("https://example.com"));
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("text"));
}

TEST(Parser, InternalLink)
{
    auto nodes = ParseMarkdown(MENDO_LIT("[section](#my-section)")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_GE(nodes[0].runs.size(), 1u);
    ASSERT_TRUE(nodes[0].runs[0].has_link());
    EXPECT_EQ(nodes[0].view_link_urls()[nodes[0].runs[0].link_url_index], MENDO_LIT("#my-section"));
}

TEST(Parser, ParagraphWithNoLink)
{
    auto nodes = ParseMarkdown(MENDO_LIT("plain text")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    for (const auto& run : nodes[0].runs) {
        EXPECT_FALSE(run.has_link());
    }
}

// ---- コードブロック ----

TEST(Parser, FencedCodeBlock)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```\ncode line 1\ncode line 2\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    EXPECT_NE(nodes[0].GetText().find(MENDO_LIT("code line 1")), mendo::doc_string_std::npos);
    EXPECT_NE(nodes[0].GetText().find(MENDO_LIT("code line 2")), mendo::doc_string_std::npos);
}

TEST(Parser, CodeBlockPreservesNewlines)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```\na\nb\nc\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    // 行間に改行を含むべき
    const auto& text = nodes[0].GetText();
    int newlines = 0;
    for (wchar_t c : text) if (c == MENDO_LIT('\n')) newlines++;
    EXPECT_GE(newlines, 2);
}

// ---- 水平線 ----

TEST(Parser, HorizontalRule)
{
    auto nodes = ParseMarkdown(MENDO_LIT("---")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::HorizontalRule);
}

TEST(Parser, HorizontalRuleWithAsterisks)
{
    auto nodes = ParseMarkdown(MENDO_LIT("***")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::HorizontalRule);
}

// ---- リスト ----

TEST(Parser, UnorderedList)
{
    auto nodes = ParseMarkdown(MENDO_LIT("- item1\n- item2\n- item3")).nodes;
    ASSERT_EQ(nodes.size(), 3u);
    for (const auto& node : nodes) {
        EXPECT_EQ(node.type, NodeType::ListItem);
        EXPECT_EQ(node.list_number, 0); // 順序なしリスト
    }
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("item1"));
    EXPECT_EQ(nodes[1].GetText(), MENDO_LIT("item2"));
    EXPECT_EQ(nodes[2].GetText(), MENDO_LIT("item3"));
}

// ---- バグ #10: ネストされた引用ブロック ----

TEST(Parser, NestedBlockquotePreservesOuterStyle)
{
    auto nodes = ParseMarkdown(MENDO_LIT("> outer\n>\n> > inner\n>\n> still outer")).nodes;
    // 内側の引用ブロックが終了した後、"still outer"はまだBlockQuoteであるべき
    bool found_still_outer = false;
    for (const auto& node : nodes) {
        if (node.GetText().find(MENDO_LIT("still outer")) != mendo::doc_string_std::npos) {
            EXPECT_EQ(node.type, NodeType::BlockQuote)
                << "内側の引用ブロック後のテキストはBlockQuoteのままであるべき";
            found_still_outer = true;
        }
    }
    EXPECT_TRUE(found_still_outer) << "ノード内に'still outer'が見つかるべき";
}

TEST(Parser, SingleBlockquoteIsBlockQuoteType)
{
    auto nodes = ParseMarkdown(MENDO_LIT("> quoted text")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::BlockQuote);
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("quoted text"));
}

// ネスト blockquote の group/quote_depth 設計回帰防止
// (PR #156 で blockquote_group をネスト中も最外側で共有する設計に変更)

TEST(Parser, NestedBlockquote_SharesSameGroup)
{
    auto nodes = ParseMarkdown(MENDO_LIT("> outer\n>\n> > inner\n>\n> still outer")).nodes;
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
    auto nodes = ParseMarkdown(MENDO_LIT("> outer\n> > inner\n> > > deep")).nodes;
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
    auto nodes = ParseMarkdown(MENDO_LIT("> outer\n> > inner\n>\n> back to outer")).nodes;
    bool checked = false;
    for (const auto& n : nodes) {
        if (n.GetText().find(MENDO_LIT("back to outer")) != mendo::doc_string_std::npos) {
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
    auto nodes = ParseMarkdown(MENDO_LIT("&#x1F600;")).nodes;
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
    auto nodes = ParseMarkdown(MENDO_LIT("&#128169;")).nodes;
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
    auto nodes = ParseMarkdown(MENDO_LIT("&#xA9;")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    // UTF-8 2 byte: C2 A9
    ASSERT_EQ(nodes[0].GetText().size(), 2u);
    EXPECT_EQ(static_cast<unsigned char>(nodes[0].GetText()[0]), 0xC2);
    EXPECT_EQ(static_cast<unsigned char>(nodes[0].GetText()[1]), 0xA9);
}

TEST(Parser, HtmlEntityBeyondUnicode)
{
    // U+110000はUnicodeの最大値を超えている; 無視されるべき
    auto nodes = ParseMarkdown(MENDO_LIT("&#x110000;")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    // 生のエンティティテキストとしてそのまま渡されるべき
    EXPECT_NE(nodes[0].GetText().find(MENDO_LIT("110000")), mendo::doc_string_std::npos);
}

// ---- リスト ----

TEST(Parser, OrderedList)
{
    auto nodes = ParseMarkdown(MENDO_LIT("1. first\n2. second\n3. third")).nodes;
    ASSERT_EQ(nodes.size(), 3u);
    for (const auto& node : nodes) {
        EXPECT_EQ(node.type, NodeType::ListItem);
    }
    EXPECT_EQ(nodes[0].list_number, 1);
    EXPECT_EQ(nodes[1].list_number, 2);
    EXPECT_EQ(nodes[2].list_number, 3);
}

TEST(Parser, OrderedListStartsFromN)
{
    auto nodes = ParseMarkdown(MENDO_LIT("5. five\n6. six")).nodes;
    ASSERT_EQ(nodes.size(), 2u);
    EXPECT_EQ(nodes[0].list_number, 5);
    EXPECT_EQ(nodes[1].list_number, 6);
}

TEST(Parser, ListIndentLevel)
{
    auto nodes = ParseMarkdown(MENDO_LIT("- item")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_GT(nodes[0].indent_level, 0);
}

// ---- タスクリスト ----

TEST(Parser, TaskListChecked)
{
    auto nodes = ParseMarkdown(MENDO_LIT("- [x] done")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::TaskListItem);
    EXPECT_TRUE(nodes[0].task_checked);
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("done"));
}

TEST(Parser, TaskListUnchecked)
{
    auto nodes = ParseMarkdown(MENDO_LIT("- [ ] todo")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::TaskListItem);
    EXPECT_FALSE(nodes[0].task_checked);
}

TEST(Parser, TaskListUpperX)
{
    auto nodes = ParseMarkdown(MENDO_LIT("- [X] also done")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_TRUE(nodes[0].task_checked);
}

// ---- 引用ブロック ----

TEST(Parser, BlockQuote)
{
    auto nodes = ParseMarkdown(MENDO_LIT("> quoted text")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::BlockQuote);
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("quoted text"));
}

TEST(Parser, BlockQuoteIndentLevel)
{
    auto nodes = ParseMarkdown(MENDO_LIT("> quoted")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_GT(nodes[0].indent_level, 0);
}

// ---- テーブル ----

TEST(Parser, SimpleTable)
{
    auto nodes = ParseMarkdown(
        MENDO_LIT("| A | B |\n")
        MENDO_LIT("|---|---|\n")
        MENDO_LIT("| 1 | 2 |")
    ).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Table);
    ASSERT_GE(nodes[0].table_data()->row_count, 2u); // header + 1 data row
}

// NodeTableData の concat 構造が parser で正しく populate されることを確認。
TEST(Parser, TableConcatStructure)
{
    auto nodes = ParseMarkdown(
        MENDO_LIT("| A | B |\n")
        MENDO_LIT("|---|---|\n")
        MENDO_LIT("| 1 | 2 |")
    ).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].type, NodeType::Table);
    const auto* tbl = nodes[0].table_data();
    ASSERT_NE(tbl, nullptr);
    EXPECT_EQ(tbl->row_count, 2u);
    EXPECT_EQ(tbl->col_count, 2u);
    EXPECT_EQ(tbl->concat_text, MENDO_LIT("A\tB\n1\t2"));
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
    EXPECT_EQ(tbl->GetCellText(0, 0), MENDO_LIT("A"));
    EXPECT_EQ(tbl->GetCellText(0, 1), MENDO_LIT("B"));
    EXPECT_EQ(tbl->GetCellText(1, 0), MENDO_LIT("1"));
    EXPECT_EQ(tbl->GetCellText(1, 1), MENDO_LIT("2"));
}

TEST(Parser, TableHeaderCells)
{
    auto nodes = ParseMarkdown(
        MENDO_LIT("| H1 | H2 |\n")
        MENDO_LIT("|---|---|\n")
        MENDO_LIT("| D1 | D2 |")
    ).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    const auto* tbl = nodes[0].table_data();
    ASSERT_GE(tbl->row_count, 2u);
    EXPECT_EQ(tbl->col_count, 2u);

    // 最初の行はヘッダーであるべき
    EXPECT_TRUE(tbl->IsHeaderRow(0));
    EXPECT_EQ(tbl->GetCellText(0, 0), MENDO_LIT("H1"));
    EXPECT_EQ(tbl->GetCellText(0, 1), MENDO_LIT("H2"));

    // 2番目の行はヘッダーではないべき
    EXPECT_FALSE(tbl->IsHeaderRow(1));
}

TEST(Parser, TableAlignment)
{
    auto nodes = ParseMarkdown(
        MENDO_LIT("| L | C | R |\n")
        MENDO_LIT("|:--|:--:|--:|\n")
        MENDO_LIT("| a | b | c |")
    ).nodes;
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
    auto nodes = ParseMarkdown(
        MENDO_LIT("| A |\n")
        MENDO_LIT("|---|\n")
        MENDO_LIT("| 1 |\n")
        MENDO_LIT("| 2 |\n")
        MENDO_LIT("| 3 |")
    ).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].table_data()->row_count, 4u); // 1 header + 3 data
}

// ---- HTMLエンティティ ----

TEST(Parser, HtmlEntityAmp)
{
    auto nodes = ParseMarkdown(MENDO_LIT("A &amp; B")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_NE(nodes[0].GetText().find(MENDO_LIT("&")), mendo::doc_string_std::npos);
}

TEST(Parser, HtmlEntityLtGt)
{
    auto nodes = ParseMarkdown(MENDO_LIT("&lt;tag&gt;")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_NE(nodes[0].GetText().find(MENDO_LIT("<")), mendo::doc_string_std::npos);
    EXPECT_NE(nodes[0].GetText().find(MENDO_LIT(">")), mendo::doc_string_std::npos);
}

// ---- 複雑なドキュメント ----

TEST(Parser, ComplexDocumentNodeCount)
{
    auto nodes = ParseMarkdown(
        MENDO_LIT("# Title\n\n")
        MENDO_LIT("Paragraph.\n\n")
        MENDO_LIT("## Section\n\n")
        MENDO_LIT("- item1\n")
        MENDO_LIT("- item2\n\n")
        MENDO_LIT("---\n\n")
        MENDO_LIT("> quote\n\n")
        MENDO_LIT("```\ncode\n```\n")
    ).nodes;
    // タイトル、段落、セクション、item1、item2、水平線、引用、コード
    EXPECT_GE(nodes.size(), 7u);
}

TEST(Parser, NodeTypesInComplexDocument)
{
    auto nodes = ParseMarkdown(
        MENDO_LIT("# H\n\nP\n\n- L\n\n---\n\n> Q\n\n```\nC\n```\n")
    ).nodes;
    bool has_heading = false, has_para = false, has_list = false;
    bool has_hr = false, has_quote = false, has_code = false;
    for (const auto& n : nodes) {
        if (n.type == NodeType::Heading) has_heading = true;
        if (n.type == NodeType::Paragraph) has_para = true;
        if (n.type == NodeType::ListItem) has_list = true;
        if (n.type == NodeType::HorizontalRule) has_hr = true;
        if (n.type == NodeType::BlockQuote) has_quote = true;
        if (n.type == NodeType::CodeBlock) has_code = true;
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
    auto nodes = ParseMarkdown(MENDO_LIT("日本語テスト")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("日本語テスト"));
}

TEST(Parser, EmojiText)
{
    auto nodes = ParseMarkdown(MENDO_LIT("Hello 🎉")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    // クラッシュせず出力が生成されることだけを確認
    EXPECT_FALSE(nodes[0].GetText().empty());
}

// ---- ソフトブレーク処理 ----

TEST(Parser, SoftBreakBecomesSpace)
{
    auto nodes = ParseMarkdown(MENDO_LIT("line1\nline2")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    // 段落内のソフトブレークはスペースになるべき
    EXPECT_NE(nodes[0].GetText().find(MENDO_LIT("line1")), mendo::doc_string_std::npos);
    EXPECT_NE(nodes[0].GetText().find(MENDO_LIT("line2")), mendo::doc_string_std::npos);
}

// ---- ラン位置の整合性 ----

TEST(Parser, RunPositionsAreValid)
{
    auto nodes = ParseMarkdown(MENDO_LIT("normal **bold** `code` *italic*")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    const auto& node = nodes[0];
    for (const auto& run : node.runs) {
        EXPECT_LE(run.start, node.GetText().size());
        EXPECT_LE(run.start + run.length, node.GetText().size());
    }
}

TEST(Parser, RunsCoverEntireText)
{
    auto nodes = ParseMarkdown(MENDO_LIT("aaa **bbb** ccc")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    const auto& node = nodes[0];
    uint32_t total_length = 0;
    for (const auto& run : node.runs) {
        total_length += run.length;
    }
    EXPECT_EQ(total_length, static_cast<uint32_t>(node.GetText().size()));
}

TEST(Parser, RunsAreContiguous)
{
    auto nodes = ParseMarkdown(MENDO_LIT("a **b** c")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    const auto& runs = nodes[0].runs;
    for (size_t i = 1; i < runs.size(); i++) {
        EXPECT_EQ(runs[i].start, runs[i - 1].start + runs[i - 1].length)
            << "ラン " << (i - 1) << " と " << i << " の間にギャップあり";
    }
}

// ---- ネストされたリスト ----

TEST(Parser, NestedUnorderedList)
{
    auto nodes = ParseMarkdown(MENDO_LIT("- a\n  - b\n    - c")).nodes;
    ASSERT_GE(nodes.size(), 3u);
    // より深いアイテムはより高いインデントレベルを持つべき
    EXPECT_LT(nodes[0].indent_level, nodes[1].indent_level);
    EXPECT_LT(nodes[1].indent_level, nodes[2].indent_level);
}

TEST(Parser, NestedOrderedList)
{
    auto nodes = ParseMarkdown(MENDO_LIT("1. a\n   1. b\n      1. c")).nodes;
    ASSERT_GE(nodes.size(), 3u);
    EXPECT_EQ(nodes[0].list_number, 1);
    EXPECT_EQ(nodes[1].list_number, 1);
    EXPECT_EQ(nodes[2].list_number, 1);
    EXPECT_LT(nodes[0].indent_level, nodes[1].indent_level);
}

TEST(Parser, MixedListNesting)
{
    auto nodes = ParseMarkdown(MENDO_LIT("1. ordered\n   - unordered\n   - unordered2")).nodes;
    ASSERT_GE(nodes.size(), 3u);
    EXPECT_GT(nodes[0].list_number, 0);
    EXPECT_EQ(nodes[1].list_number, 0);
    EXPECT_EQ(nodes[2].list_number, 0);
}

// ---- 言語指定付きコードブロック ----

TEST(Parser, CodeBlockWithLanguage)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```cpp\nint x = 1;\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Cpp);
}

TEST(Parser, CodeBlockNoTrailingNewline)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```\nhello\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    // 末尾の改行は除去されるべき
    EXPECT_FALSE(nodes[0].GetText().empty());
    EXPECT_NE(nodes[0].GetText().back(), MENDO_LIT('\n'));
}

// ---- インライン書式付きテーブル ----

TEST(Parser, TableCellWithBold)
{
    auto nodes = ParseMarkdown(
        MENDO_LIT("| A | **B** |\n")
        MENDO_LIT("|---|---|\n")
        MENDO_LIT("| 1 | 2 |")
    ).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    const auto* tbl = nodes[0].table_data();
    ASSERT_GE(tbl->row_count, 1u);
    ASSERT_GE(tbl->col_count, 2u);
    // 2番目のヘッダーセルは太字ランを持つべき
    bool has_bold = false;
    for (const auto& run : tbl->GetCellRuns(0, 1)) {
        if (run.bold()) has_bold = true;
    }
    EXPECT_TRUE(has_bold);
}

TEST(Parser, TableLinearizedText)
{
    auto nodes = ParseMarkdown(
        MENDO_LIT("| A | B |\n")
        MENDO_LIT("|---|---|\n")
        MENDO_LIT("| 1 | 2 |")
    ).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    const auto* tbl = nodes[0].table_data();
    ASSERT_GE(tbl->row_count, 2u);
    EXPECT_EQ(tbl->GetCellText(0, 0), MENDO_LIT("A"));
    EXPECT_EQ(tbl->GetCellText(0, 1), MENDO_LIT("B"));
}

// ---- リンク付きテーブル ----

TEST(Parser, TableCellWithLink)
{
    auto nodes = ParseMarkdown(
        MENDO_LIT("| Name | Link |\n")
        MENDO_LIT("|------|------|\n")
        MENDO_LIT("| foo | [bar](https://example.com) |")
    ).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    const auto* tbl = nodes[0].table_data();
    ASSERT_GE(tbl->row_count, 2u);
    ASSERT_GE(tbl->col_count, 2u);

    // リンクセルはランにlink_urlを持つべき
    bool found_link = false;
    for (const auto& run : tbl->GetCellRuns(1, 1)) {
        if (run.has_link()) {
            EXPECT_EQ(nodes[0].view_link_urls()[run.link_url_index], MENDO_LIT("https://example.com"));
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
    auto nodes = ParseMarkdown(
        MENDO_LIT("| Section |\n")
        MENDO_LIT("|---------|\n")
        MENDO_LIT("| [intro](#introduction) |")
    ).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    const auto* tbl = nodes[0].table_data();
    ASSERT_GE(tbl->row_count, 2u);

    bool found = false;
    for (const auto& run : tbl->GetCellRuns(1, 0)) {
        if (run.has_link()) {
            EXPECT_EQ(nodes[0].view_link_urls()[run.link_url_index], MENDO_LIT("#introduction"));
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(Parser, TableCellWithBoldLink)
{
    auto nodes = ParseMarkdown(
        MENDO_LIT("| Link |\n")
        MENDO_LIT("|------|\n")
        MENDO_LIT("| [**bold**](https://example.com) |")
    ).nodes;
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
    auto nodes = ParseMarkdown(
        MENDO_LIT("| Content |\n")
        MENDO_LIT("|---------|\n")
        MENDO_LIT("| before [link](https://example.com) after |")
    ).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    const auto* tbl = nodes[0].table_data();
    ASSERT_GE(tbl->row_count, 2u);

    // リンク付きとリンクなしのランを持つべき
    bool has_link_run = false;
    bool has_plain_run = false;
    for (const auto& run : tbl->GetCellRuns(1, 0)) {
        if (run.has_link()) has_link_run = true;
        else has_plain_run = true;
    }
    EXPECT_TRUE(has_link_run);
    EXPECT_TRUE(has_plain_run);
}

// ---- HTMLエンティティのエッジケース ----

TEST(Parser, HtmlEntityQuot)
{
    auto nodes = ParseMarkdown(MENDO_LIT("&quot;hello&quot;")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_NE(nodes[0].GetText().find(MENDO_LIT("\"")), mendo::doc_string_std::npos);
}

TEST(Parser, HtmlEntityNbsp)
{
    auto nodes = ParseMarkdown(MENDO_LIT("a&nbsp;b")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_NE(nodes[0].GetText().find(MENDO_LIT('\u00A0')), mendo::doc_string_std::npos);
}

// ---- ハードブレーク ----

TEST(Parser, HardBreakWithTwoSpaces)
{
    auto nodes = ParseMarkdown(MENDO_LIT("line1  \nline2")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    // ハードブレークはテキスト内に改行を生成するべき
    EXPECT_NE(nodes[0].GetText().find(MENDO_LIT('\n')), mendo::doc_string_std::npos);
}

// ---- 複数見出しのアンカー一意性 ----

TEST(Parser, MultipleHeadingsHaveAnchors)
{
    auto nodes = ParseMarkdown(MENDO_LIT("# A\n\n## B\n\n### C")).nodes;
    for (const auto& node : nodes) {
        if (node.type == NodeType::Heading) {
            EXPECT_FALSE(node.anchor_id().empty())
                << "見出しにアンカーがない";
        }
    }
}

// ---- インライン書式付きリンク ----

TEST(Parser, BoldLink)
{
    auto nodes = ParseMarkdown(MENDO_LIT("[**bold link**](https://example.com)")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    bool has_bold_link = false;
    for (const auto& run : nodes[0].runs) {
        if (run.bold() && run.has_link()) {
            has_bold_link = true;
        }
    }
    EXPECT_TRUE(has_bold_link);
}

// ---- 重複アンカーIDに一意のサフィックスが付与される ----

TEST(Parser, DuplicateHeadingAnchorsAreUnique)
{
    auto nodes = ParseMarkdown(MENDO_LIT("# Title\n\n## Title\n\n### Title")).nodes;
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(nodes[0].anchor_id(), MENDO_LIT("title"));
    EXPECT_EQ(nodes[1].anchor_id(), MENDO_LIT("title-1"));
    EXPECT_EQ(nodes[2].anchor_id(), MENDO_LIT("title-2"));
}

TEST(Parser, DuplicateAnchorsWithDifferentText)
{
    auto nodes = ParseMarkdown(MENDO_LIT("# A\n\n## B\n\n### A")).nodes;
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(nodes[0].anchor_id(), MENDO_LIT("a"));
    EXPECT_EQ(nodes[1].anchor_id(), MENDO_LIT("b"));
    EXPECT_EQ(nodes[2].anchor_id(), MENDO_LIT("a-1"));
}

// ---- 数値HTMLエンティティ ----

TEST(Parser, NumericEntityDecimal)
{
    auto nodes = ParseMarkdown(MENDO_LIT("&#65;")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("A"));
}

TEST(Parser, NumericEntityHex)
{
    auto nodes = ParseMarkdown(MENDO_LIT("&#x41;")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("A"));
}

TEST(Parser, NumericEntityJapanese)
{
    // &#x3042; = あ (Hiragana A)
    auto nodes = ParseMarkdown(MENDO_LIT("&#x3042;")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("\u3042"));
}

// ---- 深いネスト ----

TEST(Parser, DeeplyNestedList)
{
    mendo::doc_string_std md;
    md += MENDO_LIT("- L1\n");
    md += MENDO_LIT("  - L2\n");
    md += MENDO_LIT("    - L3\n");
    md += MENDO_LIT("      - L4\n");
    auto nodes = ParseMarkdown(md).nodes;
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
    auto nodes = ParseMarkdown(
        MENDO_LIT("| A | B | C |\n")
        MENDO_LIT("|---|---|---|\n")
        MENDO_LIT("| 1 | 2 |\n")
    ).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Table);
    ASSERT_GE(nodes[0].table_data()->row_count, 2u);
}

// ---- Mermaid言語のコードブロック ----

TEST(Parser, MermaidCodeBlock)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```mermaid\ngraph TD;\n  A-->B;\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Mermaid);
}

// ---- LaTeX display math ($$...$$) ----

TEST(Parser, LatexDisplayMathSingleLinePromotedToCodeBlock)
{
    auto result = ParseMarkdown(MENDO_LIT("$$E = mc^2$$"));
    ASSERT_EQ(result.nodes.size(), 1u);
    EXPECT_EQ(result.nodes[0].type, NodeType::CodeBlock);
    EXPECT_EQ(result.nodes[0].code_language, SyntaxLanguage::LatexMath);
    EXPECT_EQ(result.nodes[0].GetText(), MENDO_LIT("E = mc^2"));
    // diagram_indices に登録される（描画パイプラインに流すため）
    ASSERT_EQ(result.diagram_indices.size(), 1u);
    EXPECT_EQ(result.diagram_indices[0], 0u);
}

TEST(Parser, LatexDisplayMathWithOtherContentFallsBackToText)
{
    // 段落内に数式以外の内容があるときは昇格せず、テキストとして残す
    auto nodes = ParseMarkdown(MENDO_LIT("before $$x+y$$ after")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_NE(nodes[0].code_language, SyntaxLanguage::LatexMath);
    // フォールバック時は $$ 区切りが復元されていること
    const mendo::doc_string_std text(nodes[0].GetText());
    EXPECT_NE(text.find(MENDO_LIT("$$x+y$$")), mendo::doc_string_std::npos);
}

TEST(Parser, LatexMultipleDisplayMathInOneParagraphFallsBackToText)
{
    // 1段落に2つ以上の $$...$$ があるときは昇格しない
    auto nodes = ParseMarkdown(MENDO_LIT("$$a$$ $$b$$")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
}

TEST(Parser, LatexInlineMathRemainsAsText)
{
    // インライン $...$ は昇格対象外。$ 記号を含む元のテキストとして扱う
    auto nodes = ParseMarkdown(MENDO_LIT("value is $x$ here")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_NE(nodes[0].code_language, SyntaxLanguage::LatexMath);
    const mendo::doc_string_std text(nodes[0].GetText());
    EXPECT_NE(text.find(MENDO_LIT("$x$")), mendo::doc_string_std::npos);
}

TEST(Parser, LatexDisplayMathInBlockquoteNotPromoted)
{
    // blockquote 内の $$...$$ は引用文脈維持のため昇格しない
    auto nodes = ParseMarkdown(MENDO_LIT("> $$y=x$$")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::BlockQuote);
}

TEST(Parser, PlainDollarSignsNotMisdetectedAsMath)
{
    // "The price is $5 or $10." のような金額表記は LaTeX ではなくテキストとして扱う。
    // md4c は `$` の直後に空白・数字・記号が続く場合などインライン数式として解釈しないため、
    // フラグ有効化で既存のドル記号テキストが壊れないことを確認する。
    auto nodes = ParseMarkdown(MENDO_LIT("The price is $5 or $10.")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_NE(nodes[0].code_language, SyntaxLanguage::LatexMath);
    const mendo::doc_string_std text(nodes[0].GetText());
    EXPECT_NE(text.find(MENDO_LIT("$5")), mendo::doc_string_std::npos);
    EXPECT_NE(text.find(MENDO_LIT("$10")), mendo::doc_string_std::npos);
}

TEST(Parser, LatexDisplayMathMultiline)
{
    // 複数行にわたる $$...$$ でも昇格される（中身はパーサがそのまま保持）
    auto result = ParseMarkdown(MENDO_LIT("$$\nE = mc^2\n$$"));
    ASSERT_EQ(result.nodes.size(), 1u);
    EXPECT_EQ(result.nodes[0].type, NodeType::CodeBlock);
    EXPECT_EQ(result.nodes[0].code_language, SyntaxLanguage::LatexMath);
    // 中身に 'E = mc^2' が含まれること（md4c の改行扱いに依存するが、式本体は保持される）
    const auto& body = result.nodes[0].GetText();
    EXPECT_NE(body.find(MENDO_LIT("E = mc^2")), mendo::doc_string_std::npos);
}

TEST(Parser, LatexDisplayMathLineCountMatchesNewlines)
{
    // OnText で事前集計した display_math_newlines を line_count に流用しているため、
    // 旧実装 (current_text 全走査の std::ranges::count) と同じ値を保つことを回帰テストする。
    const mendo::doc_string_view sources[] = {
        MENDO_LIT("$$E = mc^2$$"),
        MENDO_LIT("$$\nE = mc^2\n$$"),
        MENDO_LIT("$$\na\nb\nc\n$$"),
    };
    for (const auto src : sources) {
        SCOPED_TRACE(mendo::doc_string_std{ src });
        auto result = ParseMarkdown(src);
        ASSERT_EQ(result.nodes.size(), 1u);
        ASSERT_EQ(result.nodes[0].type, NodeType::CodeBlock);
        ASSERT_EQ(result.nodes[0].code_language, SyntaxLanguage::LatexMath);
        const auto text = result.nodes[0].GetText();
        const auto expected = static_cast<int>(std::ranges::count(text, MENDO_LIT('\n')));
        EXPECT_EQ(result.nodes[0].line_count, expected);
    }
}

TEST(Parser, LatexDisplayMathSurroundingParagraphs)
{
    // 前後に通常段落がある場合も、純粋な $$...$$ 段落のみ昇格される
    auto result = ParseMarkdown(MENDO_LIT("before\n\n$$E=mc^2$$\n\nafter"));
    ASSERT_EQ(result.nodes.size(), 3u);
    EXPECT_EQ(result.nodes[0].type, NodeType::Paragraph);
    EXPECT_EQ(result.nodes[1].type, NodeType::CodeBlock);
    EXPECT_EQ(result.nodes[1].code_language, SyntaxLanguage::LatexMath);
    EXPECT_EQ(result.nodes[1].GetText(), MENDO_LIT("E=mc^2"));
    EXPECT_EQ(result.nodes[2].type, NodeType::Paragraph);
    ASSERT_EQ(result.diagram_indices.size(), 1u);
    EXPECT_EQ(result.diagram_indices[0], 1u);
}

// ---- 特殊文字を含むURL ----

TEST(Parser, LinkWithSpecialCharsInUrl)
{
    auto nodes = ParseMarkdown(MENDO_LIT("[link](https://example.com/path?q=1&r=2#frag)")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    bool found = false;
    for (const auto& run : nodes[0].runs) {
        if (run.has_link()) {
            EXPECT_EQ(nodes[0].view_link_urls()[run.link_url_index], MENDO_LIT("https://example.com/path?q=1&r=2#frag"));
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

// ---- 空の見出し ----

TEST(Parser, EmptyHeading)
{
    auto nodes = ParseMarkdown(MENDO_LIT("# \n\ntext")).nodes;
    // md4cは空テキストの見出しノードを生成する場合がある
    bool found_heading = false;
    for (auto& n : nodes) {
        if (n.type == NodeType::Heading) {
            found_heading = true;
        }
    }
    // md4cの動作により空の見出しが生成されるかどうかは不定;
    // 少なくともクラッシュしないべき。
    (void)found_heading;
}

// ---- GitHub Alerts ----

TEST(Parser, AlertNoteDetected)
{
    auto nodes = ParseMarkdown(MENDO_LIT("> [!NOTE]\n> This is a note")).nodes;
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::BlockQuote);
    EXPECT_EQ(nodes[0].alert_type, AlertType::Note);
}

TEST(Parser, AlertTipDetected)
{
    auto nodes = ParseMarkdown(MENDO_LIT("> [!TIP]\n> Helpful advice")).nodes;
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].alert_type, AlertType::Tip);
}

TEST(Parser, AlertImportantDetected)
{
    auto nodes = ParseMarkdown(MENDO_LIT("> [!IMPORTANT]\n> Key info")).nodes;
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].alert_type, AlertType::Important);
}

TEST(Parser, AlertWarningDetected)
{
    auto nodes = ParseMarkdown(MENDO_LIT("> [!WARNING]\n> Be careful")).nodes;
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].alert_type, AlertType::Warning);
}

TEST(Parser, AlertCautionDetected)
{
    auto nodes = ParseMarkdown(MENDO_LIT("> [!CAUTION]\n> Dangerous")).nodes;
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].alert_type, AlertType::Caution);
}

TEST(Parser, AlertCaseInsensitive)
{
    auto nodes = ParseMarkdown(MENDO_LIT("> [!note]\n> lower case")).nodes;
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].alert_type, AlertType::Note);
}

TEST(Parser, AlertCaseMixed)
{
    auto nodes = ParseMarkdown(MENDO_LIT("> [!Note]\n> mixed case")).nodes;
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].alert_type, AlertType::Note);
}

TEST(Parser, AlertMarkerStrippedAndLabelInserted)
{
    auto nodes = ParseMarkdown(MENDO_LIT("> [!NOTE]\n> Content here")).nodes;
    ASSERT_GE(nodes.size(), 1u);
    // マーカー "[!NOTE]" が除去され、アイコン + ラベル "Note" に置換されているべき
    EXPECT_NE(nodes[0].GetText().find(MENDO_LIT("Note")), mendo::doc_string_std::npos);
    EXPECT_EQ(nodes[0].GetText().find(MENDO_LIT("[!NOTE]")), mendo::doc_string_std::npos);
    // 先頭はアイコン文字列であるべき
    auto icon = GetAlertIcon(AlertType::Note);
    EXPECT_EQ(nodes[0].GetText().substr(0, icon.size()), icon);
    // コンテンツも残っているべき
    EXPECT_NE(nodes[0].GetText().find(MENDO_LIT("Content here")), mendo::doc_string_std::npos);
}

TEST(Parser, AlertLabelIsBold)
{
    auto nodes = ParseMarkdown(MENDO_LIT("> [!NOTE]\n> Some text")).nodes;
    ASSERT_GE(nodes.size(), 1u);
    ASSERT_GE(nodes[0].runs.size(), 1u);
    // 最初のランはラベル部分で太字であるべき
    EXPECT_TRUE(nodes[0].runs[0].bold());
    EXPECT_EQ(nodes[0].runs[0].start, 0u);
    EXPECT_EQ(nodes[0].runs[0].length, nodes[0].alert_label_length);
}

TEST(Parser, AlertLabelLength)
{
    // alert_label_length は UTF-8 byte。ℹ (U+2139) は 3 byte、❗ (U+2757) も 3 byte。
    auto nodes = ParseMarkdown(MENDO_LIT("> [!NOTE]\n> text")).nodes;
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].alert_label_length, 8u); // ℹ 3 + ' ' 1 + "NOTE" 4

    auto nodes2 = ParseMarkdown(MENDO_LIT("> [!IMPORTANT]\n> text")).nodes;
    ASSERT_GE(nodes2.size(), 1u);
    EXPECT_EQ(nodes2[0].alert_label_length, 13u); // ❗ 3 + ' ' 1 + "IMPORTANT" 9
}

TEST(Parser, AlertRunPositionsAreValid)
{
    auto nodes = ParseMarkdown(MENDO_LIT("> [!WARNING]\n> Some **bold** text")).nodes;
    ASSERT_GE(nodes.size(), 1u);
    const auto& node = nodes[0];
    for (const auto& run : node.runs) {
        EXPECT_LE(run.start + run.length, static_cast<uint32_t>(node.GetText().size()))
            << "ラン [" << run.start << ", " << run.start + run.length
            << ") がテキスト長 " << node.GetText().size() << " を超えている";
    }
}

TEST(Parser, AlertMultiParagraphGrouping)
{
    auto nodes = ParseMarkdown(MENDO_LIT("> [!NOTE]\n> First para\n>\n> Second para")).nodes;
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
    auto nodes = ParseMarkdown(MENDO_LIT("> [!TIP]\n> First\n>\n> Second")).nodes;
    // 最初のノードだけ alert_label_length > 0
    int label_count = 0;
    for (const auto& node : nodes) {
        if (node.alert_label_length > 0) label_count++;
    }
    EXPECT_EQ(label_count, 1);
}

TEST(Parser, RegularBlockquoteUnaffected)
{
    auto nodes = ParseMarkdown(MENDO_LIT("> Just a normal quote")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::BlockQuote);
    EXPECT_EQ(nodes[0].alert_type, AlertType::None);
    EXPECT_EQ(nodes[0].alert_label_length, 0u);
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("Just a normal quote"));
}

TEST(Parser, AlertMarkerOnlyNoContent)
{
    auto nodes = ParseMarkdown(MENDO_LIT("> [!NOTE]")).nodes;
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].alert_type, AlertType::Note);
    // マーカーだけの場合、アイコン + スペース + ラベルのみ残る
    mendo::doc_string_std expected = mendo::doc_string_std(GetAlertIcon(AlertType::Note)) + MENDO_LIT(" Note");
    EXPECT_EQ(nodes[0].GetText(), expected);
}

TEST(Parser, AlertFollowedByRegularBlockquote)
{
    auto nodes = ParseMarkdown(MENDO_LIT("> [!NOTE]\n> Alert text\n\n> Normal quote")).nodes;
    // Alert と通常の blockquote が混在
    bool has_alert = false, has_normal = false;
    for (const auto& node : nodes) {
        if (node.type == NodeType::BlockQuote) {
            if (node.alert_type != AlertType::None) has_alert = true;
            else has_normal = true;
        }
    }
    EXPECT_TRUE(has_alert);
    EXPECT_TRUE(has_normal);
}

// ネストを跨いだ Alert 伝播 (PR #156)

TEST(Parser, Alert_PropagatesAcrossNestedBlockquote)
{
    // example/nested.md #9 相当: 親 -> ネスト -> 親の続き
    auto nodes = ParseMarkdown(
        MENDO_LIT("> [!NOTE]\n")
        MENDO_LIT("> Alert head\n")
        MENDO_LIT("> > nested\n")
        MENDO_LIT(">\n")
        MENDO_LIT("> Alert continues")
    ).nodes;
    bool checked_continuation = false;
    for (const auto& n : nodes) {
        if (n.GetText().find(MENDO_LIT("Alert continues")) != mendo::doc_string_std::npos) {
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
    auto nodes = ParseMarkdown(
        MENDO_LIT("> outer\n")
        MENDO_LIT("> > [!NOTE]\n")
        MENDO_LIT("> > inner note text")
    ).nodes;
    for (const auto& n : nodes) {
        EXPECT_EQ(n.alert_type, AlertType::None)
            << "ネスト内の Alert マーカーは無効化される";
        EXPECT_EQ(n.alert_label_length, 0u);
    }
}

TEST(Parser, AlertUnknownTypeIgnored)
{
    auto nodes = ParseMarkdown(MENDO_LIT("> [!UNKNOWN]\n> text")).nodes;
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].alert_type, AlertType::None);
    // マーカーがそのまま残っているべき
    EXPECT_NE(nodes[0].GetText().find(MENDO_LIT("UNKNOWN")), mendo::doc_string_std::npos);
}

TEST(Parser, AlertLabelContents)
{
    // 各AlertTypeのラベル文字列を確認
    EXPECT_EQ(GetAlertLabel(AlertType::Note), MENDO_LIT("Note"));
    EXPECT_EQ(GetAlertLabel(AlertType::Tip), MENDO_LIT("Tip"));
    EXPECT_EQ(GetAlertLabel(AlertType::Important), MENDO_LIT("Important"));
    EXPECT_EQ(GetAlertLabel(AlertType::Warning), MENDO_LIT("Warning"));
    EXPECT_EQ(GetAlertLabel(AlertType::Caution), MENDO_LIT("Caution"));
    EXPECT_EQ(GetAlertLabel(AlertType::None), MENDO_LIT(""));
}

TEST(Parser, DetectAlertsOnEmptyVector)
{
    std::pmr::vector<Node> nodes;
    DetectAlerts(nodes, {}); // クラッシュしないべき
    EXPECT_TRUE(nodes.empty());
}

TEST(Parser, AlertWithInlineFormatting)
{
    auto nodes = ParseMarkdown(MENDO_LIT("> [!TIP]\n> Use **bold** and `code`")).nodes;
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].alert_type, AlertType::Tip);
    // テキストにboldとcodeが含まれるべき
    EXPECT_NE(nodes[0].GetText().find(MENDO_LIT("bold")), mendo::doc_string_std::npos);
    EXPECT_NE(nodes[0].GetText().find(MENDO_LIT("code")), mendo::doc_string_std::npos);
    // フォーマット用のランがあるべき
    bool has_bold = false, has_code = false;
    for (const auto& run : nodes[0].runs) {
        if (run.bold() && run.start > 0) has_bold = true; // ラベル以外の太字
        if (run.code()) has_code = true;
    }
    EXPECT_TRUE(has_bold);
    EXPECT_TRUE(has_code);
}

TEST(Parser, AlertTextStartsWithLabelThenNewline)
{
    auto nodes = ParseMarkdown(MENDO_LIT("> [!CAUTION]\n> Don't do this")).nodes;
    ASSERT_GE(nodes.size(), 1u);
    // テキストは "[icon] Caution\n..." の形式であるべき
    const auto& text = nodes[0].GetText();
    auto nl = text.find(MENDO_LIT('\n'));
    ASSERT_NE(nl, mendo::doc_string_std::npos);
    mendo::doc_string_std expected = mendo::doc_string_std(GetAlertIcon(AlertType::Caution)) + MENDO_LIT(" Caution");
    EXPECT_EQ(text.substr(0, nl), mendo::doc_string_view(expected));
}

// ---- ソースオフセット ----

TEST(Parser, SourceOffsetSingleParagraph)
{
    auto nodes = ParseMarkdown(MENDO_LIT("Hello world")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].source_offset, 0u);
}

TEST(Parser, SourceOffsetHeading)
{
    // "# Title" → テキスト "Title" はオフセット 2 から
    auto nodes = ParseMarkdown(MENDO_LIT("# Title")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].source_offset, 2u);
}

TEST(Parser, SourceOffsetMultipleParagraphs)
{
    // "First\n\nSecond\n\nThird"
    // "First" = offset 0, "Second" = offset 7, "Third" = offset 15
    auto nodes = ParseMarkdown(MENDO_LIT("First\n\nSecond\n\nThird")).nodes;
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(nodes[0].source_offset, 0u);
    EXPECT_EQ(nodes[1].source_offset, 7u);
    EXPECT_EQ(nodes[2].source_offset, 15u);
}

TEST(Parser, SourceOffsetIncreasing)
{
    // ノードの source_offset は単調増加であるべき
    auto nodes = ParseMarkdown(
        MENDO_LIT("# Heading\n\n")
        MENDO_LIT("Paragraph\n\n")
        MENDO_LIT("- item1\n")
        MENDO_LIT("- item2\n\n")
        MENDO_LIT("```\ncode\n```\n\n")
        MENDO_LIT("End")).nodes;
    ASSERT_GE(nodes.size(), 3u);
    uint32_t prev = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].source_offset != kUnsetSourceOffset) {
            EXPECT_GE(nodes[i].source_offset, prev)
                << "ノード " << i << " の source_offset が前のノードより小さい";
            prev = nodes[i].source_offset;
        }
    }
}

TEST(Parser, SourceOffsetCodeBlock)
{
    // "```\nhello\n```" → コードブロック内テキストのオフセット
    auto nodes = ParseMarkdown(MENDO_LIT("```\nhello\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    // コードブロックのテキスト "hello" は "```\n" = 4バイト目から
    EXPECT_EQ(nodes[0].source_offset, 4u);
}

TEST(Parser, SourceOffsetEmptyInput)
{
    auto nodes = ParseMarkdown(MENDO_LIT("")).nodes;
    EXPECT_TRUE(nodes.empty());
}

TEST(Parser, SourceOffsetHorizontalRule)
{
    // "---" はテキストを持たないのでsource_offsetは未設定のまま
    auto nodes = ParseMarkdown(MENDO_LIT("---")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::HorizontalRule);
    EXPECT_EQ(nodes[0].source_offset, kUnsetSourceOffset);
}

TEST(Parser, SourceOffsetCjkMultiCodeUnit)
{
    // source_offset は UTF-8 byte。
    auto nodes = ParseMarkdown(MENDO_LIT("あいう\n\ntest")).nodes;
    ASSERT_EQ(nodes.size(), 2u);
    EXPECT_EQ(nodes[0].source_offset, 0u);
    EXPECT_EQ(nodes[1].source_offset, 11u); // "あいう" 9 byte + "\n\n" 2
}

TEST(Parser, SourceOffsetUnorderedList)
{
    // "- A\n- B\n- C"
    // "- " = 2バイト, "A" offset=2, "\n- " = 3バイト, "B" offset=5+2=7?
    // 実際: "- A\n" = 4, "- B\n" = 4, "- C" = 3
    auto nodes = ParseMarkdown(MENDO_LIT("- A\n- B\n- C")).nodes;
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(nodes[0].source_offset, 2u);  // "A" = "- " の後
    EXPECT_EQ(nodes[1].source_offset, 6u);  // "B" = "- A\n- " の後
    EXPECT_EQ(nodes[2].source_offset, 10u); // "C" = "- A\n- B\n- " の後
}

TEST(Parser, SourceOffsetOrderedList)
{
    // "1. First\n2. Second"
    auto nodes = ParseMarkdown(MENDO_LIT("1. First\n2. Second")).nodes;
    ASSERT_EQ(nodes.size(), 2u);
    EXPECT_EQ(nodes[0].source_offset, 3u);  // "First" = "1. " の後
    EXPECT_EQ(nodes[1].source_offset, 12u); // "Second" = "1. First\n2. " の後
}

TEST(Parser, SourceOffsetBlockQuote)
{
    // "> quoted\n\nnormal"
    auto nodes = ParseMarkdown(MENDO_LIT("> quoted\n\nnormal")).nodes;
    ASSERT_GE(nodes.size(), 2u);
    EXPECT_EQ(nodes[0].source_offset, 2u); // "quoted" = "> " の後
}

TEST(Parser, SourceOffsetNestedList)
{
    // ネストされたリストでも単調増加
    auto nodes = ParseMarkdown(MENDO_LIT("- outer\n  - inner\n- next")).nodes;
    ASSERT_GE(nodes.size(), 3u);
    uint32_t prev = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].source_offset != kUnsetSourceOffset) {
            EXPECT_GE(nodes[i].source_offset, prev)
                << "ノード " << i << " の offset が前のノードより小さい";
            prev = nodes[i].source_offset;
        }
    }
}

TEST(Parser, SourceOffsetTable)
{
    // テーブルノードの source_offset はヘッダの最初のセルテキスト
    auto nodes = ParseMarkdown(
        MENDO_LIT("| A | B |\n")
        MENDO_LIT("|---|---|\n")
        MENDO_LIT("| 1 | 2 |")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Table);
    // "| " の後の "A" = offset 2
    EXPECT_EQ(nodes[0].source_offset, 2u);
}

TEST(Parser, SourceOffsetCodeBlockWithLanguage)
{
    // "```cpp\nint x;\n```" → テキストは "```cpp\n" = 7バイト目から
    auto nodes = ParseMarkdown(MENDO_LIT("```cpp\nint x;\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].source_offset, 7u);
}

TEST(Parser, SourceOffsetMixedDocument)
{
    // 多様なブロック型を含む文書で全ノードの offset が有効かつ単調増加
    mendo::doc_string_std md =
        MENDO_LIT("# Title\n\n")            // heading
        MENDO_LIT("Paragraph\n\n")          // paragraph
        MENDO_LIT("- item\n\n")             // list
        MENDO_LIT("> quote\n\n")             // blockquote
        MENDO_LIT("```\ncode\n```\n\n")     // code
        MENDO_LIT("---\n\n")                // hr (offset 未設定)
        MENDO_LIT("End");                   // paragraph
    auto nodes = ParseMarkdown(md).nodes;
    ASSERT_GE(nodes.size(), 6u);

    uint32_t prev = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].source_offset != kUnsetSourceOffset) {
            EXPECT_GE(nodes[i].source_offset, prev)
                << "ノード " << i << " (type="
                << static_cast<int>(nodes[i].type) << ") の offset が不正";
            EXPECT_LT(nodes[i].source_offset, static_cast<uint32_t>(md.size()))
                << "ノード " << i << " の offset がソース長を超えている";
            prev = nodes[i].source_offset;
        }
    }
}

TEST(Parser, SourceOffsetTaskList)
{
    auto nodes = ParseMarkdown(MENDO_LIT("- [x] done\n- [ ] todo")).nodes;
    ASSERT_EQ(nodes.size(), 2u);
    // "- [x] " = 6バイト, "done" offset=6
    EXPECT_EQ(nodes[0].source_offset, 6u);
    EXPECT_EQ(nodes[1].source_offset, 17u); // "- [x] done\n- [ ] " = 17
}

// ---- line_count ----

TEST(Parser, LineCountSingleLine)
{
    auto nodes = ParseMarkdown(MENDO_LIT("Hello world")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].line_count, 0);
}

TEST(Parser, LineCountCodeBlock)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```\na\nb\nc\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    // "a\nb\nc" → 2個の改行（末尾の\nはパーサーが除去する）
    EXPECT_EQ(nodes[0].line_count, 2);
}

TEST(Parser, LineCountMultilineParagraph)
{
    // softbreakは空白に変換されるため改行にならない
    auto nodes = ParseMarkdown(MENDO_LIT("line1\nline2\nline3")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].line_count, 0);
}

TEST(Parser, LineCountHardBreak)
{
    // 末尾2スペース+改行 = hard break → \n
    auto nodes = ParseMarkdown(MENDO_LIT("line1  \nline2  \nline3")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].line_count, 2);
}

TEST(Parser, LineCountEmptyCodeBlock)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```\n\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    EXPECT_EQ(nodes[0].line_count, 0);
}

TEST(Parser, LineCountLargeCodeBlock)
{
    mendo::doc_string_std md = MENDO_LIT("```\n");
    for (int i = 0; i < 100; i++) {
        md += MENDO_LIT("line ") + mendo::to_doc_string(i) + MENDO_LIT("\n");
    }
    md += MENDO_LIT("```");
    auto nodes = ParseMarkdown(md).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    // 100行 → 99個の改行（末尾の\nが除去される）
    EXPECT_EQ(nodes[0].line_count, 99);
}

// ---- 遅延トークン化 ----

TEST(Parser, SyntaxTokensEmptyAfterParse)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```cpp\nint x = 42;\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Cpp);
    // パース時にはトークン化されないことを確認（レンダラーで遅延実行）
    EXPECT_TRUE(nodes[0].syntax_tokens().empty());
}

TEST(Parser, SyntaxTokensEmptyForMermaid)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```mermaid\ngraph TD\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Mermaid);
    EXPECT_TRUE(nodes[0].syntax_tokens().empty());
}

// ---- UTF-8バッチ変換 ----

TEST(Parser, Utf8BatchPlainText)
{
    auto nodes = ParseMarkdown(MENDO_LIT("Hello world, this is a test.")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("Hello world, this is a test."));
}

TEST(Parser, Utf8BatchWithSpanBoundary)
{
    auto nodes = ParseMarkdown(MENDO_LIT("before **bold** after")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("before bold after"));
    ASSERT_GE(nodes[0].runs.size(), 3u);
    EXPECT_FALSE(nodes[0].runs[0].bold());
    EXPECT_TRUE(nodes[0].runs[1].bold());
    EXPECT_FALSE(nodes[0].runs[2].bold());
}

TEST(Parser, Utf8BatchWithEntity)
{
    auto nodes = ParseMarkdown(MENDO_LIT("a &amp; b")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("a & b"));
}

TEST(Parser, Utf8BatchWithSoftBreak)
{
    auto nodes = ParseMarkdown(MENDO_LIT("line1\nline2")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    // softbreak → space
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("line1 line2"));
}

TEST(Parser, Utf8BatchWithHardBreak)
{
    auto nodes = ParseMarkdown(MENDO_LIT("line1  \nline2")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("line1\nline2"));
}

TEST(Parser, Utf8BatchMultibyteUtf8)
{
    auto nodes = ParseMarkdown(MENDO_LIT("日本語テスト")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("日本語テスト"));
}

TEST(Parser, Utf8BatchMixedAsciiAndMultibyte)
{
    auto nodes = ParseMarkdown(MENDO_LIT("Hello **世界** test")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("Hello 世界 test"));
    ASSERT_GE(nodes[0].runs.size(), 3u);
    EXPECT_TRUE(nodes[0].runs[1].bold());
}

TEST(Parser, Utf8BatchCodeBlockContent)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```\nint x = 0;\nfloat y = 1.0;\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("int x = 0;\nfloat y = 1.0;"));
}

TEST(Parser, Utf8BatchEntityBetweenText)
{
    auto nodes = ParseMarkdown(MENDO_LIT("a&lt;b&gt;c")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("a<b>c"));
}

TEST(Parser, Utf8BatchNestedFormatting)
{
    auto nodes = ParseMarkdown(MENDO_LIT("***bold and italic***")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), MENDO_LIT("bold and italic"));
    ASSERT_GE(nodes[0].runs.size(), 1u);
    EXPECT_TRUE(nodes[0].runs[0].bold());
    EXPECT_TRUE(nodes[0].runs[0].italic());
}

TEST(Parser, Utf8BatchLinkText)
{
    auto nodes = ParseMarkdown(MENDO_LIT("before [link text](https://example.com) after")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_NE(nodes[0].GetText().find(MENDO_LIT("before")), mendo::doc_string_std::npos);
    EXPECT_NE(nodes[0].GetText().find(MENDO_LIT("link text")), mendo::doc_string_std::npos);
    EXPECT_NE(nodes[0].GetText().find(MENDO_LIT("after")), mendo::doc_string_std::npos);
}
