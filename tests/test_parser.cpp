#include <gtest/gtest.h>
#include "document_test_helpers.h"
#include "parse_invariants.h"
#include "parser.h"
#include "test_nodes.h"
#include "utility.h"
#include <algorithm>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// 表のセル (row, col) にあるリンクの URL。リンクが無ければ nullopt。
std::optional<std::string_view> CellLinkUrl(const Node& table, size_t row, size_t col)
{
    const auto runs = table.table_data()->GetCellRuns(row, col);
    const auto it = std::ranges::find_if(runs, &TextRun::has_link);
    if (it == runs.end()) {
        return std::nullopt;
    }
    return table.view_link_urls()[it->link_url_index];
}

} // namespace

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

TEST(Parser, HeadingLevels)
{
    for (int level = 1; level <= 6; level++) {
        SCOPED_TRACE(level);
        const std::string md = std::string(level, '#') + " Title";
        auto nodes = ParseNodes(md);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_EQ(nodes[0].type, NodeType::Heading);
        EXPECT_EQ(nodes[0].heading_level, level);
        EXPECT_EQ(nodes[0].GetText(), "Title");
    }
}

// ---- インライン書式 ----

// 書式の span はマークアップを除いたテキスト全体を覆う 1 つの run になる。
TEST(Parser, InlineSpanBecomesStyledRun)
{
    struct Case {
        std::string_view md;
        std::string_view text;
        bool bold;
        bool italic;
        bool code;
        bool strikethrough;
    };
    constexpr Case kCases[] = {
        { "**bold**", "bold", true, false, false, false },
        { "*italic*", "italic", false, true, false, false },
        { "`code`", "code", false, false, true, false },
        { "~~deleted~~", "deleted", false, false, false, true },
        { "***bold and italic***", "bold and italic", true, true, false, false },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.md);
        auto nodes = ParseNodes(c.md);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_EQ(nodes[0].GetText(), c.text);
        ASSERT_EQ(nodes[0].runs().size(), 1u);
        const auto& run = nodes[0].runs()[0];
        EXPECT_EQ(run.start, 0u);
        EXPECT_EQ(run.length, c.text.size());
        EXPECT_EQ(run.bold(), c.bold);
        EXPECT_EQ(run.italic(), c.italic);
        EXPECT_EQ(run.code(), c.code);
        EXPECT_EQ(run.strikethrough(), c.strikethrough);
        EXPECT_FALSE(run.has_link());
    }
}

// ---- リンク ----

TEST(Parser, LinkRunPointsToUrl)
{
    struct Case {
        std::string_view md;
        std::string_view text;
        std::string_view url;
    };
    constexpr Case kCases[] = {
        { "[text](https://example.com)", "text", "https://example.com" },
        { "[section](#my-section)", "section", "#my-section" },
        { "[link](https://example.com/path?q=1&r=2#frag)", "link", "https://example.com/path?q=1&r=2#frag" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.md);
        auto nodes = ParseNodes(c.md);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_EQ(nodes[0].GetText(), c.text);
        ASSERT_EQ(nodes[0].runs().size(), 1u);
        ASSERT_TRUE(nodes[0].runs()[0].has_link());
        EXPECT_EQ(nodes[0].view_link_urls()[nodes[0].runs()[0].link_url_index], c.url);
    }
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
    for (const auto& run : tbl->all_runs) {
        ASSERT_TRUE(run.has_link());
        EXPECT_LT(static_cast<size_t>(run.link_url_index), urls.size());
    }
    EXPECT_EQ(tbl->all_runs.size(), urls.size());
}

// ---- コードブロック ----

TEST(Parser, CodeBlockPreservesNewlines)
{
    auto nodes = ParseNodes("```\na\nb\nc\n```");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    // 行間に改行を含むべき
    EXPECT_GE(std::ranges::count(nodes[0].GetText(), '\n'), 2);
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

// 上限ちょうどは全部残し、超えた分は末尾から捨てる。打ち切り後も改行は行数に数える。
TEST(Parser, StyledRunCapKeepsLeadingRunsAndLineCount)
{
    for (const size_t n : { size_t{ 4096 }, size_t{ 4097 } }) {
        SCOPED_TRACE(n);
        std::string md;
        for (size_t i = 0; i < n; i++) {
            md += i == 0 ? "`a`" : "\\\n`a`";
        }
        const auto nodes = ParseNodes(md);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_TRUE(ParseInvariantsHold(md, nodes));
        EXPECT_EQ(nodes[0].GetText().size(), 2 * n - 1);
        EXPECT_EQ(nodes[0].line_count, static_cast<int32_t>(n - 1));
        const auto runs = nodes[0].runs();
        ASSERT_EQ(runs.size(), 4096u);
        EXPECT_EQ(runs.back().start, 2u * 4095);
        EXPECT_TRUE(runs.back().code());
    }
}

// 上限はセルごと。表全体で数えると 2 セル目以降の装飾が消える。
TEST(Parser, StyledRunCapIsPerTableCell)
{
    std::string cell;
    for (int i = 0; i < 4097; i++) {
        cell += "`a` ";
    }
    const std::string md = "|a|b|\n|-|-|\n|" + cell + "|" + cell + "|\n";
    const auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_TRUE(nodes[0].has_table());
    EXPECT_TRUE(ParseInvariantsHold(md, nodes));
    const auto* tbl = nodes[0].table_data();
    EXPECT_EQ(tbl->GetCellRuns(1, 0).size(), 4096u);
    EXPECT_EQ(tbl->GetCellRuns(1, 1).size(), 4096u);
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
        if (node.GetText().contains("still outer")) {
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
        if (n.GetText().contains("back to outer")) {
            EXPECT_EQ(n.quote_depth, 1)
                << "ネストを抜けて外側に戻ったノードは quote_depth=1";
            checked = true;
        }
    }
    EXPECT_TRUE(checked);
}

// ---- 文字参照 ----

// 文字参照は UTF-8 に解く。解けないもの (U+10FFFF 超など) は原文のまま残す。
TEST(Parser, CharacterReferencesAreDecoded)
{
    struct Case {
        std::string_view md;
        std::string_view text;
    };
    constexpr Case kCases[] = {
        { "&#x1F600;", "\xF0\x9F\x98\x80" }, // 追加面 (bug #11)
        { "&#128169;", "\xF0\x9F\x92\xA9" }, // 追加面の 10 進表記
        { "&#xA9;", "\xC2\xA9" },
        { "&#x3042;", "\xE3\x81\x82" },
        { "&#65;", "A" },
        { "&#x41;", "A" },
        { "&#x110000;", "&#x110000;" },
        { "&lt;tag&gt;", "<tag>" },
        { "a&lt;b&gt;c", "a<b>c" },
        { "a &amp; b", "a & b" },
        { "&quot;hello&quot;", "\"hello\"" },
        { "a&nbsp;b", "a\xC2\xA0" "b" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.md);
        auto nodes = ParseNodes(c.md);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_EQ(nodes[0].GetText(), c.text);
    }
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

// 記号と番号の書式はリストの段で決まる。番号付きと順序なしは混ぜて数え、引用は数えない (issue#365)。
TEST(Parser, ListDepthCountsOnlyListNesting)
{
    struct Case {
        std::string_view md;
        std::vector<int> depths;
    };
    const Case kCases[] = {
        { "- a\n  - b\n    - c\n      - d", { 1, 2, 3, 4 } },
        { "1. a\n   - b\n     1. c", { 1, 2, 3 } },
        { "> - a\n>   - b", { 1, 2 } },
        { "- a\n\n  > - b", { 1, 2 } },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.md);
        std::vector<int> depths;
        for (const auto& node : ParseNodes(c.md)) {
            if (node.type == NodeType::ListItem) {
                depths.push_back(node.list_depth());
            }
        }
        EXPECT_EQ(depths, c.depths);
    }
}

TEST(Parser, FootnoteDefIsOutermostList)
{
    const auto result = ParseMarkdown("text[^1]\n\n[^1]: note");
    ASSERT_EQ(result.indices.footnote_def_indices.size(), 1u);
    EXPECT_EQ(result.nodes[result.indices.footnote_def_indices[0]].list_depth(), 1);
}

// ---- タスクリスト ----

TEST(Parser, TaskListItem)
{
    struct Case {
        std::string_view md;
        bool checked;
        std::string_view text;
    };
    constexpr Case kCases[] = {
        { "- [x] done", true, "done" },
        { "- [X] also done", true, "also done" },
        { "- [ ] todo", false, "todo" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.md);
        auto nodes = ParseNodes(c.md);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_EQ(nodes[0].type, NodeType::TaskListItem);
        EXPECT_EQ(nodes[0].task_checked(), c.checked);
        EXPECT_EQ(nodes[0].GetText(), c.text);
    }
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
    EXPECT_TRUE(tbl->cell_run_starts.empty()) << "装飾の無い表は持たない";
    EXPECT_EQ(tbl->head_row_count, 1u);
    EXPECT_EQ(tbl->aligns.size(), 2u);
    EXPECT_EQ(tbl->GetCellText(0, 0), "A");
    EXPECT_EQ(tbl->GetCellText(0, 1), "B");
    EXPECT_EQ(tbl->GetCellText(1, 0), "1");
    EXPECT_EQ(tbl->GetCellText(1, 1), "2");
}

// 最初の装飾が途中のセルに来ても、それより前のセルの run の開始位置を埋めてセルごとの run を引ける。
// 足りない行のセルも run の無いセルとして埋まる。
TEST(Parser, TableCellRunStartsBackfilledAtFirstRun)
{
    for (const std::string_view md : { std::string_view{ "| a | b |\n|---|---|\n| c | **d** |\n| e | f |" },
                                       std::string_view{ "| a | b |\n|---|---|\n| c | **d** |\n| e |" } }) {
        SCOPED_TRACE(md);
        const auto nodes = ParseNodes(md);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_TRUE(ParseInvariantsHold(md, nodes));
        const auto* tbl = nodes[0].table_data();
        ASSERT_NE(tbl, nullptr);
        EXPECT_EQ(tbl->cell_run_starts, (std::vector<uint32_t>{ 0, 0, 0, 0, 1, 1, 1 }));
        for (size_t r = 0; r < tbl->row_count; ++r) {
            for (size_t c = 0; c < tbl->col_count; ++c) {
                const auto runs = tbl->GetCellRuns(r, c);
                ASSERT_EQ(runs.size(), r == 1 && c == 1 ? 1u : 0u) << r << "," << c;
                if (!runs.empty()) {
                    EXPECT_TRUE(runs[0].bold());
                }
            }
        }
    }
}

// md4c は短い行も列数ぶんのセルで出すので、最後のセルが空でも区切りは前のセルに入らない。
TEST(Parser, TableEmptyLastCellsKeepSeparatorsOut)
{
    for (const std::string_view md : { std::string_view{ "| a | b |\n|---|---|\n| x |   |" },
                                       std::string_view{ "| a | b |\n|---|---|\n| x |" },
                                       std::string_view{ "| a | b |\n|---|---|\n|   |   |" } }) {
        SCOPED_TRACE(md);
        const auto nodes = ParseNodes(md);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_TRUE(ParseInvariantsHold(md, nodes));
        const auto* tbl = nodes[0].table_data();
        ASSERT_NE(tbl, nullptr);
        for (size_t r = 0; r < tbl->row_count; ++r) {
            for (size_t c = 0; c < tbl->col_count; ++c) {
                EXPECT_FALSE(tbl->GetCellText(r, c).contains('\t')) << r << "," << c;
                EXPECT_FALSE(tbl->GetCellText(r, c).contains('\n')) << r << "," << c;
            }
        }
        EXPECT_EQ(tbl->GetCellText(1, 1), "");
    }
}

// md4c は列数を 16 bit で持つので、65536 列を超える表も 16 bit に収まった列数のセルで出る。
TEST(Parser, TableColumnCountWrapsInMd4c)
{
    constexpr size_t kCols = size_t{ std::numeric_limits<uint16_t>::max() } + 2;
    std::string md;
    for (const std::string_view cell : { "h", "-", "d" }) {
        for (size_t c = 0; c < kCols; ++c) {
            md += '|';
            md += cell;
        }
        md += "|\n";
    }
    const auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_TRUE(ParseInvariantsHold(md, nodes));
    const auto* tbl = nodes[0].table_data();
    ASSERT_NE(tbl, nullptr);
    EXPECT_EQ(tbl->col_count, kCols & 0xFFFF);
    EXPECT_EQ(tbl->cell_text_starts.size(), size_t{ tbl->row_count } * tbl->col_count + 1);
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
    for (const auto type : { NodeType::Heading, NodeType::Paragraph, NodeType::ListItem,
                             NodeType::HorizontalRule, NodeType::BlockQuote, NodeType::CodeBlock }) {
        SCOPED_TRACE(static_cast<int>(type));
        EXPECT_TRUE(std::ranges::any_of(nodes, [type](const Node& n) { return n.type == type; }));
    }
}

// ---- UTF-8処理 ----

TEST(Parser, MultibyteTextIsKept)
{
    for (const std::string_view md : { std::string_view{ "日本語テスト" }, std::string_view{ "Hello 🎉" } }) {
        SCOPED_TRACE(md);
        auto nodes = ParseNodes(md);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_EQ(nodes[0].GetText(), md);
    }
}

// ---- ラン位置の整合性 ----

TEST(Parser, RunPositionsAreValid)
{
    constexpr std::string_view md = "normal **bold** `code` *italic*";
    auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_TRUE(ParseInvariantsHold(md, nodes));
}

// 装飾の無い前後のテキストは run にならない。位置は UTF-8 byte。
TEST(Parser, RunsOmitPlainText)
{
    struct Case {
        std::string_view md;
        std::string_view text;
        uint32_t start;
        uint32_t length;
    };
    constexpr Case kCases[] = {
        { "aaa **bbb** ccc", "aaa bbb ccc", 4, 3 },
        { "before **bold** after", "before bold after", 7, 4 },
        { "Hello **世界** test", "Hello 世界 test", 6, 6 },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.md);
        auto nodes = ParseNodes(c.md);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_EQ(nodes[0].GetText(), c.text);
        const auto& runs = nodes[0].runs();
        ASSERT_EQ(runs.size(), 1u);
        EXPECT_EQ(runs[0].start, c.start);
        EXPECT_EQ(runs[0].length, c.length);
        EXPECT_TRUE(runs[0].bold());
    }
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
    auto nodes = ParseNodes("- a\n  - b\n    - c\n      - d");
    ASSERT_GE(nodes.size(), 4u);
    for (size_t i = 1; i < 4; ++i) {
        EXPECT_LT(nodes[i - 1].indent_level, nodes[i].indent_level) << "i=" << i;
    }
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
    EXPECT_TRUE(std::ranges::any_of(tbl->GetCellRuns(0, 1), &TextRun::bold));
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

    EXPECT_EQ(CellLinkUrl(nodes[0], 1, 1), "https://example.com");
    EXPECT_EQ(CellLinkUrl(nodes[0], 1, 0), std::nullopt);
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

    EXPECT_EQ(CellLinkUrl(nodes[0], 1, 0), "#introduction");
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

    EXPECT_TRUE(std::ranges::any_of(tbl->GetCellRuns(1, 0), [](const TextRun& run) static { return run.bold() && run.has_link(); }));
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

// ---- インライン書式付きリンク ----

TEST(Parser, BoldLink)
{
    auto nodes = ParseNodes("[**bold link**](https://example.com)");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_TRUE(std::ranges::any_of(nodes[0].runs(), [](const TextRun& run) static { return run.bold() && run.has_link(); }));
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
    EXPECT_EQ(nodes[0].type, NodeType::Diagram);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Mermaid);
}

// ---- LaTeX display math ($$...$$) ----

TEST(Parser, LatexDisplayMathSingleLinePromotedToDiagram)
{
    auto result = ParseMarkdown("$$E = mc^2$$");
    ASSERT_EQ(result.nodes.size(), 1u);
    EXPECT_EQ(result.nodes[0].type, NodeType::Diagram);
    EXPECT_EQ(result.nodes[0].code_language, SyntaxLanguage::LatexMath);
    EXPECT_EQ(result.nodes[0].GetText(), "E = mc^2");
    // diagram_indices に登録される（描画パイプラインに流すため）
    ASSERT_EQ(result.indices.diagram_indices.size(), 1u);
    EXPECT_EQ(result.indices.diagram_indices[0], 0u);
}

TEST(Parser, LatexDisplayMathWithOtherContentFallsBackToText)
{
    // 段落内に数式以外の内容があるときは昇格せず、テキストとして残す
    auto nodes = ParseNodes("before $$x+y$$ after");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_NE(nodes[0].code_language, SyntaxLanguage::LatexMath);
    // フォールバック時は $$ 区切りが復元されていること
    EXPECT_TRUE(nodes[0].GetText().contains("$$x+y$$"));
}

TEST(Parser, LatexMultipleDisplayMathInOneParagraphFallsBackToText)
{
    // 1段落に2つ以上の $$...$$ があるときは昇格しない
    auto nodes = ParseNodes("$$a$$ $$b$$");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
}

// 数式以外のテキストの種別ごとに、$$..$$ だけの段落かを判定する。HTML は表示しないので数えない。
TEST(Parser, LatexDisplayMathPromotionDependsOnOtherTextType)
{
    struct Case {
        std::string_view md;
        bool promoted;
    };
    for (const auto& c : {
             Case{ "$$x$$ &amp;", false },
             Case{ "$$x$$&amp;", false },
             Case{ "$$x$$  \nnext", false },
             Case{ "$$x$$\nnext", false },
             Case{ "$$x$$ `c`", false },
             Case{ "$$x$$<!-- c -->", true },
         }) {
        SCOPED_TRACE(c.md);
        auto nodes = ParseNodes(c.md);
        ASSERT_FALSE(nodes.empty());
        EXPECT_EQ(nodes[0].type == NodeType::Diagram, c.promoted);
    }
}

// 長くて区切った $$..$$ は昇格させず、本文のまま続きのノードに並べる。
TEST(Parser, LatexDisplayMathSplitIntoChunksNotPromoted)
{
    std::string md = "$$";
    while (md.size() < kTextChunkTarget * 2) {
        md += "x + ";
    }
    md += "y$$";
    auto nodes = ParseNodes(md);
    ASSERT_GT(nodes.size(), 1u);
    for (const auto& n : nodes) {
        EXPECT_EQ(n.type, NodeType::Paragraph);
    }
}

namespace {

std::vector<std::string_view> MathRunTexts(std::string_view text, std::span<const TextRun> runs)
{
    std::vector<std::string_view> out;
    for (const auto& r : runs) {
        if (r.math()) {
            out.push_back(text.substr(r.start, r.length));
        }
    }
    return out;
}

std::vector<std::string_view> MathRunTexts(const Node& node)
{
    return MathRunTexts(node.GetText(), node.runs());
}

} // namespace

// テキストは $ 付きの原文のまま残し、前後の $ を含む範囲に kMath の run を付ける。
TEST(Parser, LatexInlineMathKeepsTextAndMarksRun)
{
    auto nodes = ParseNodes("value is $x^2$ here");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_EQ(nodes[0].GetText(), "value is $x^2$ here");
    EXPECT_EQ(MathRunTexts(nodes[0]), (std::vector<std::string_view>{ "$x^2$" }));
}

TEST(Parser, LatexInlineMathRunsCombineWithDecorationAndStaySeparate)
{
    auto nodes = ParseNodes("**a $x$ b** $y$ [$z$](https://example.com)");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(MathRunTexts(nodes[0]), (std::vector<std::string_view>{ "$x$", "$y$", "$z$" }));
    for (const auto& r : nodes[0].runs()) {
        if (r.math() && nodes[0].GetText().substr(r.start, r.length) == "$x$") {
            EXPECT_TRUE(r.bold());
        }
        if (r.math() && nodes[0].GetText().substr(r.start, r.length) == "$z$") {
            EXPECT_TRUE(r.has_link());
        }
    }
}

TEST(Parser, LatexInlineMathInTableCellMarksCellRun)
{
    auto nodes = ParseNodes("| a | b |\n|---|---|\n| $x$ | c |");
    ASSERT_EQ(nodes.size(), 1u);
    const auto* tbl = nodes[0].table_data();
    ASSERT_NE(tbl, nullptr);
    EXPECT_EQ(MathRunTexts(tbl->GetCellText(1, 0), tbl->GetCellRuns(1, 0)), (std::vector<std::string_view>{ "$x$" }));
}

// 長い本文の途中で区切っても、インライン数式は 1 つのノードに収め、$ が片側だけの数式を作らない。
TEST(Parser, LatexInlineMathNotSplitAcrossChunks)
{
    std::string md;
    while (md.size() < kTextChunkTarget + kTextChunkWindow - 64) {
        md += "word ";
    }
    md += "$";
    while (md.size() < kTextChunkTarget + kTextChunkWindow * 3) {
        md += "x + ";
    }
    md += "y$ tail";
    auto nodes = ParseNodes(md);
    ASSERT_GT(nodes.size(), 1u);
    size_t math_count = 0;
    for (const auto& n : nodes) {
        for (const auto m : MathRunTexts(n)) {
            EXPECT_TRUE(m.starts_with('$') && m.ends_with('$')) << m;
            ++math_count;
        }
    }
    EXPECT_EQ(math_count, 1u);
}

TEST(Parser, LatexDisplayMathInBlockquoteNotPromoted)
{
    // blockquote 内の $$...$$ は引用文脈維持のため昇格せず、本文の中に描く数式の run にする
    auto nodes = ParseNodes("> $$y=x$$");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::BlockQuote);
    EXPECT_EQ(MathRunTexts(nodes[0]), (std::vector<std::string_view>{ "$$y=x$$" }));
    EXPECT_TRUE(nodes[0].runs()[0].display_math());
}

// 本文と混ざった $$...$$ は昇格せず、前後の $$ を含む範囲に kMathDisplay の run を付ける。
TEST(Parser, LatexDisplayMathInTextMarksDisplayRun)
{
    auto nodes = ParseNodes("a $$x+y$$ b $z$");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_EQ(MathRunTexts(nodes[0]), (std::vector<std::string_view>{ "$$x+y$$", "$z$" }));
    EXPECT_TRUE(nodes[0].runs()[0].display_math());
    EXPECT_FALSE(nodes[0].runs()[1].display_math());
}

// $$...$$ だけの段落は今までどおり図にし、数式の run は残さない。
TEST(Parser, PromotedDisplayMathHasNoRuns)
{
    auto nodes = ParseNodes("$$x$$");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Diagram);
    EXPECT_TRUE(nodes[0].runs().empty());
}

TEST(Parser, PlainDollarSignsNotMisdetectedAsMath)
{
    // "The price is $5 or $10." のような金額表記は LaTeX ではなくテキストとして扱う。
    // md4c は `$` の直後に空白・数字・記号が続く場合などインライン数式として解釈しないため、
    // フラグ有効化で既存のドル記号テキストが壊れないことを確認する。
    auto nodes = ParseNodes("The price is $5 or $10.");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_NE(nodes[0].code_language, SyntaxLanguage::LatexMath);
    EXPECT_TRUE(MathRunTexts(nodes[0]).empty());
    EXPECT_TRUE(nodes[0].GetText().contains("$5"));
    EXPECT_TRUE(nodes[0].GetText().contains("$10"));
}

TEST(Parser, LatexDisplayMathMultiline)
{
    // 複数行にわたる $$...$$ でも昇格される（中身はパーサがそのまま保持）
    auto result = ParseMarkdown("$$\nE = mc^2\n$$");
    ASSERT_EQ(result.nodes.size(), 1u);
    EXPECT_EQ(result.nodes[0].type, NodeType::Diagram);
    EXPECT_EQ(result.nodes[0].code_language, SyntaxLanguage::LatexMath);
    // 中身に 'E = mc^2' が含まれること（md4c の改行扱いに依存するが、式本体は保持される）
    const auto& body = result.nodes[0].GetText();
    EXPECT_TRUE(body.contains("E = mc^2"));
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
        ASSERT_EQ(result.nodes.size(), 1u);
        ASSERT_EQ(result.nodes[0].type, NodeType::Diagram);
        ASSERT_EQ(result.nodes[0].code_language, SyntaxLanguage::LatexMath);
        const auto text = result.nodes[0].GetText();
        const auto expected = static_cast<int>(std::ranges::count(text, '\n'));
        EXPECT_EQ(result.nodes[0].line_count, expected);
    }
}

TEST(Parser, LatexDisplayMathSurroundingParagraphs)
{
    // 前後に通常段落がある場合も、純粋な $$...$$ 段落のみ昇格される
    auto result = ParseMarkdown("before\n\n$$E=mc^2$$\n\nafter");
    ASSERT_EQ(result.nodes.size(), 3u);
    EXPECT_EQ(result.nodes[0].type, NodeType::Paragraph);
    EXPECT_EQ(result.nodes[1].type, NodeType::Diagram);
    EXPECT_EQ(result.nodes[1].code_language, SyntaxLanguage::LatexMath);
    EXPECT_EQ(result.nodes[1].GetText(), "E=mc^2");
    EXPECT_EQ(result.nodes[2].type, NodeType::Paragraph);
    ASSERT_EQ(result.indices.diagram_indices.size(), 1u);
    EXPECT_EQ(result.indices.diagram_indices[0], 1u);
}

TEST(Parser, MathFencedCodeBlockIsLatexDiagram)
{
    // GitHub / GitLab 互換の ```math も $$...$$ と同じく図として描画する
    auto result = ParseMarkdown("before\n\n```math\n\\frac{a}{b}\n\\sqrt{x}\n```");
    ASSERT_EQ(result.nodes.size(), 2u);
    EXPECT_EQ(result.nodes[1].type, NodeType::Diagram);
    EXPECT_EQ(result.nodes[1].code_language, SyntaxLanguage::LatexMath);
    EXPECT_EQ(result.nodes[1].GetText(), "\\frac{a}{b}\n\\sqrt{x}");
    ASSERT_EQ(result.indices.diagram_indices.size(), 1u);
    EXPECT_EQ(result.indices.diagram_indices[0], 1u);
}

// ---- 空の見出し ----

TEST(Parser, EmptyHeading)
{
    // "# " は CommonMark 上の空 ATX 見出し。テキスト無しでも Heading ノードは残す。
    auto nodes = ParseNodes("# \n\ntext");
    ASSERT_EQ(nodes.size(), 2u);
    EXPECT_EQ(nodes[0].type, NodeType::Heading);
    EXPECT_EQ(nodes[0].heading_level, 1);
    EXPECT_TRUE(nodes[0].GetText().empty());
    EXPECT_EQ(nodes[1].type, NodeType::Paragraph);
    EXPECT_EQ(nodes[1].GetText(), "text");
}

// ---- GitHub Alerts ----

// マーカーはアイコンとラベルの行に置き換わり、本文はその次の行に続く。種別名の大小は問わない。
TEST(Parser, AlertMarkerBecomesLabelLine)
{
    struct Case {
        std::string_view md;
        AlertType type;
        std::string_view body;
    };
    constexpr Case kCases[] = {
        { "> [!NOTE]\n> This is a note", AlertType::Note, "This is a note" },
        { "> [!TIP]\n> Helpful advice", AlertType::Tip, "Helpful advice" },
        { "> [!IMPORTANT]\n> Key info", AlertType::Important, "Key info" },
        { "> [!WARNING]\n> Be careful", AlertType::Warning, "Be careful" },
        { "> [!CAUTION]\n> Don't do this", AlertType::Caution, "Don't do this" },
        { "> [!note]\n> lower case", AlertType::Note, "lower case" },
        { "> [!Note]\n> mixed case", AlertType::Note, "mixed case" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.md);
        auto nodes = ParseNodes(c.md);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_EQ(nodes[0].type, NodeType::BlockQuote);
        EXPECT_EQ(nodes[0].alert_type, c.type);
        const std::string label{ AlertHeadText(c.type) };
        EXPECT_EQ(nodes[0].GetText(), label + "\n" + std::string{ c.body });
        EXPECT_EQ(nodes[0].alert_label_length(), label.size());
    }
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
        EXPECT_EQ(nodes[0].GetText(), std::string{ AlertHeadText(AlertType::Note) } + "\nbody");
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

TEST(Parser, AlertLabelIsBold)
{
    auto nodes = ParseNodes("> [!NOTE]\n> Some text");
    ASSERT_GE(nodes.size(), 1u);
    ASSERT_GE(nodes[0].runs().size(), 1u);
    // 最初のランはラベル部分で太字、アイコン絵文字は含まない
    const auto icon_len = static_cast<uint32_t>(AlertHeadText(AlertType::Note).find(' '));
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

// マーカー直後の改行は除去範囲に入るので、本文の run はラベル行の次へちょうど平行移動する。
TEST(Parser, AlertRunsShiftExactlyPastLabelLine)
{
    constexpr std::string_view md = "> [!NOTE]\n> **b** [l](u)";
    const auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 1u);
    const auto& node = nodes[0];
    ASSERT_EQ(node.alert_type, AlertType::Note);
    EXPECT_TRUE(ParseInvariantsHold(md, nodes));

    const std::string head{ AlertHeadText(AlertType::Note) };
    const size_t icon_size = head.find(' ');
    EXPECT_EQ(node.GetText(), head + "\nb l");
    EXPECT_EQ(node.line_count, 1);
    EXPECT_EQ(node.alert_label_length(), head.size());

    const auto runs = node.runs();
    ASSERT_EQ(runs.size(), 3u);
    EXPECT_EQ(runs[0].start, icon_size);
    EXPECT_EQ(runs[0].length, head.size() - icon_size);
    EXPECT_TRUE(runs[0].bold());
    EXPECT_EQ(runs[1].start, head.size() + 1);
    EXPECT_EQ(runs[1].length, 1u);
    EXPECT_TRUE(runs[1].bold());
    EXPECT_EQ(runs[2].start, head.size() + 3);
    EXPECT_EQ(runs[2].length, 1u);
    ASSERT_TRUE(runs[2].has_link());
    EXPECT_EQ(node.view_link_urls()[runs[2].link_url_index], "u");
}

TEST(Parser, AlertMultiParagraphGrouping)
{
    auto nodes = ParseNodes("> [!NOTE]\n> First para\n>\n> Second para");
    // 複数の BlockQuote ノードが生成され、すべて同じ alert_type を持つべき
    EXPECT_GE(std::ranges::count_if(nodes, [](const Node& node) static { return node.type == NodeType::BlockQuote && node.alert_type == AlertType::Note; }), 2)
        << "複数段落のAlertは全ノードに伝播されるべき";
}

TEST(Parser, AlertOnlyFirstNodeHasLabel)
{
    auto nodes = ParseNodes("> [!TIP]\n> First\n>\n> Second");
    // 最初のノードだけ alert_label_length > 0
    EXPECT_EQ(std::ranges::count_if(nodes, [](const Node& node) static { return node.alert_label_length() > 0; }), 1);
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
    EXPECT_EQ(nodes[0].GetText(), AlertHeadText(AlertType::Note));
}

TEST(Parser, AlertFollowedByRegularBlockquote)
{
    auto nodes = ParseNodes("> [!NOTE]\n> Alert text\n\n> Normal quote");
    // Alert と通常の blockquote が混在
    auto quotes = nodes | std::views::filter([](const Node& node) static { return node.type == NodeType::BlockQuote; });
    EXPECT_TRUE(std::ranges::any_of(quotes, [](const Node& node) static { return node.alert_type != AlertType::None; }));
    EXPECT_TRUE(std::ranges::contains(quotes, AlertType::None, &Node::alert_type));
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
        EXPECT_EQ(nodes[0].GetText(), AlertHeadText(AlertType::Note));
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
        if (n.GetText().contains("Alert continues")) {
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
    EXPECT_TRUE(nodes[0].GetText().contains("UNKNOWN"));
}

// ノードが細かい文書でも粗い文書でも、パース後のノード配列に大きな余りを残さない。
TEST(Parser, NodeCapacityStaysNearSize)
{
    std::string dense;
    for (int i = 0; i < 20000; ++i) {
        dense += std::format("- [a](u{})\n", i);
    }
    std::string sparse = "```\n";
    sparse.append(1 << 20, 'x');
    sparse += "\n```\n";
    for (const std::string& md : { dense, sparse }) {
        auto result = ParseMarkdown(md);
        const auto& nodes = result.nodes;
        ASSERT_FALSE(nodes.empty());
        EXPECT_LE(nodes.capacity(), nodes.size() + nodes.size() / 4) << "size=" << nodes.size();
    }
}

TEST(Parser, AlertWithInlineFormatting)
{
    auto nodes = ParseNodes("> [!TIP]\n> Use **bold** and `code`");
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].alert_type, AlertType::Tip);
    // テキストにboldとcodeが含まれるべき
    EXPECT_TRUE(nodes[0].GetText().contains("bold"));
    EXPECT_TRUE(nodes[0].GetText().contains("code"));
    // ラベル以外の太字と code のランがあるべき
    EXPECT_TRUE(std::ranges::any_of(nodes[0].runs(), [](const TextRun& run) static { return run.bold() && run.start > 0; }));
    EXPECT_TRUE(std::ranges::any_of(nodes[0].runs(), &TextRun::code));
}

// ---- ソースオフセット ----

// source_offset はノードのテキストが始まる原文の UTF-8 byte 位置。テキストを持たない HR は未設定。
TEST(Parser, SourceOffsetPointsAtTextStart)
{
    struct Case {
        std::string_view md;
        std::vector<size_t> offsets;
    };
    const Case kCases[] = {
        { "Hello world", { 0 } },
        { "# Title", { 2 } },
        { "First\n\nSecond\n\nThird", { 0, 7, 15 } },
        { "```\nhello\n```", { 4 } },
        { "```cpp\nint x;\n```", { 7 } },
        { "---", { kUnsetSourceOffset } },
        { "あいう\n\ntest", { 0, 11 } },
        { "- A\n- B\n- C", { 2, 6, 10 } },
        { "1. First\n2. Second", { 3, 12 } },
        { "- [x] done\n- [ ] todo", { 6, 17 } },
        { "> quoted\n\nnormal", { 2, 10 } },
        // 表はヘッダの最初のセル
        { "| A | B |\n|---|---|\n| 1 | 2 |", { 2 } },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.md);
        const auto nodes = ParseNodes(c.md);
        const auto offsets = nodes | std::views::transform(&Node::SourceOffset) | std::ranges::to<std::vector>();
        EXPECT_EQ(offsets, c.offsets);
    }
}

// 色々なブロックが混ざっても、設定済みの source_offset は単調増加で原文の中に収まる。
TEST(Parser, SourceOffsetsIncreaseAcrossBlocks)
{
    struct Case {
        std::string_view md;
        size_t min_nodes;
    };
    constexpr Case kCases[] = {
        { "# Heading\n\nParagraph\n\n- item1\n- item2\n\n```\ncode\n```\n\nEnd", 3 },
        { "- outer\n  - inner\n- next", 3 },
        { "# Title\n\nParagraph\n\n- item\n\n> quote\n\n```\ncode\n```\n\n---\n\nEnd", 6 },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.md);
        const auto nodes = ParseNodes(c.md);
        ASSERT_GE(nodes.size(), c.min_nodes);
        EXPECT_TRUE(ParseInvariantsHold(c.md, nodes));
    }
}

// ---- line_count ----

// ソフト改行は空白、強制改行とコードの行は '\n' になり、line_count はその数。コード末尾の改行は落とす。
TEST(Parser, NewlinesInTextAndLineCount)
{
    struct Case {
        std::string_view md;
        std::string_view text;
        int32_t line_count;
    };
    constexpr Case kCases[] = {
        { "Hello world", "Hello world", 0 },
        { "line1\nline2", "line1 line2", 0 },
        { "line1\nline2\nline3", "line1 line2 line3", 0 },
        { "line1  \nline2", "line1\nline2", 1 },
        { "line1  \nline2  \nline3", "line1\nline2\nline3", 2 },
        { "```\na\nb\nc\n```", "a\nb\nc", 2 },
        { "```\nint x = 0;\nfloat y = 1.0;\n```", "int x = 0;\nfloat y = 1.0;", 1 },
        { "```\n\n```", "", 0 },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.md);
        auto nodes = ParseNodes(c.md);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_EQ(nodes[0].GetText(), c.text);
        EXPECT_EQ(nodes[0].line_count, c.line_count);
    }
}

TEST(Parser, LineCountLargeCodeBlock)
{
    const std::string md = MakeCodeBlockMd(100);
    auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 1u);
    // 100行 → 99個の改行（末尾の\nが除去される）
    EXPECT_EQ(nodes[0].line_count, 99);
}

// ---- UTF-8バッチ変換 ----

TEST(Parser, Utf8BatchLinkText)
{
    auto nodes = ParseNodes("before [link text](https://example.com) after");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), "before link text after");
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
    EXPECT_TRUE(result.nodes.empty());
}

TEST(Parser, ParseMarkdownDefaultStopTokenWorksAsUsual)
{
    // default-constructed の stop_token は never stop_requested。従来動作を維持。
    auto result = ParseMarkdown("# Title\n\nBody");
    EXPECT_FALSE(result.nodes.empty());
}

// ---- Alert マーカーの判定 ----

// 先頭行がマーカーだけなら Alert になり、ソフト改行は改行のまま残る。そうでなければ空白になる。
TEST(Parser, AlertMarkerOnlyFirstLine)
{
    struct Case {
        std::string_view marker;
        AlertType type;
    };
    constexpr Case kCases[] = {
        { "[!NOTE]", AlertType::Note },
        { "[!note]", AlertType::Note },
        { "[!TIP]", AlertType::Tip },
        { "[!NOTE] x", AlertType::None },
        { "[!FOO]", AlertType::None },
        { "[!]", AlertType::None },
        { "[NOTE]", AlertType::None },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.marker);
        const std::string md = std::format("> {}\n> body", c.marker);
        const auto nodes = ParseNodes(md);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_EQ(nodes[0].alert_type, c.type);
        const std::string expected = c.type == AlertType::None ? std::format("{} body", c.marker)
                                                               : std::format("{}\nbody", AlertHeadText(c.type));
        EXPECT_EQ(nodes[0].GetText(), expected);
    }
}

// マーカーは最外側の引用グループの先頭段落でだけ効く。
TEST(Parser, AlertMarkerOnlyAtHeadOfOuterQuote)
{
    const auto nodes = ParseNodes("> [!NOTE]\n>\n> [!TIP]\n\n[!NOTE]\n\n> [!TIP]\n\n> > [!NOTE]");
    ASSERT_EQ(nodes.size(), 5u);
    EXPECT_EQ(nodes[0].GetText(), AlertHeadText(AlertType::Note));
    EXPECT_EQ(nodes[1].GetText(), "[!TIP]") << "同じ引用の 2 段落目";
    EXPECT_EQ(nodes[1].alert_type, AlertType::Note);
    EXPECT_EQ(nodes[2].alert_type, AlertType::None) << "引用外の段落";
    EXPECT_EQ(nodes[3].GetText(), AlertHeadText(AlertType::Tip)) << "別の引用グループの先頭";
    EXPECT_EQ(nodes[4].alert_type, AlertType::None) << "ネストした引用";
    EXPECT_EQ(nodes[4].GetText(), "[!NOTE]");
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
    const auto& nodes = result.nodes;
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
    ASSERT_EQ(result.indices.footnote_def_indices.size(), 1u);
    EXPECT_EQ(result.indices.footnote_def_indices[0], 3);
}

TEST(Parser, FootnotesAreNumberedInReferenceOrder)
{
    const auto result = ParseMarkdown("A[^b] B[^a]\n\n[^a]: first\n[^b]: second");
    const auto& nodes = result.nodes;
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
    const auto& nodes = result.nodes;
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(nodes[2].GetText(), "n ↩ ↩2");
    EXPECT_EQ(LinkUrlAt(nodes[2], "↩2"), "#user-content-fnref-1-2");
    ASSERT_EQ(result.indices.footnote_refs.size(), 2u);
    EXPECT_EQ(result.indices.footnote_refs[1].ref_id, 2u);
}

// md4c はテーブル行の判定でもインラインを解析して参照数を数えるため、ref_id / ref_count が実際の参照数とずれる。
TEST(Parser, FootnoteRefInTableIsCountedOnce)
{
    const std::string md = "| h |\n|---|\n| c[^1] |\n\nz[^1]\n\n[^1]: n";
    const auto result = ParseMarkdown(md);
    const auto& nodes = result.nodes;
    ASSERT_EQ(nodes.size(), 4u);
    EXPECT_EQ(nodes[0].table_data()->GetCellText(1, 0), "c[1]");
    EXPECT_EQ(nodes[3].GetText(), "n ↩ ↩2");
    ASSERT_EQ(result.indices.footnote_refs.size(), 2u);
    EXPECT_EQ(result.indices.footnote_refs[0].node_index, 0);
    EXPECT_EQ(result.indices.footnote_refs[1].ref_id, 2u);
    EXPECT_TRUE(ParseInvariantsHold(md, nodes));
}

TEST(Parser, FootnoteRefInsideLaterDefinitionGetsBackref)
{
    const auto result = ParseMarkdown("a[^1] b[^2]\n\n[^1]: one\n[^2]: two[^1]");
    const auto& nodes = result.nodes;
    ASSERT_EQ(nodes.size(), 4u);
    EXPECT_EQ(nodes[2].GetText(), "one ↩ ↩2");
    EXPECT_EQ(nodes[3].GetText(), "two[1] ↩");
    ASSERT_EQ(result.indices.footnote_refs.size(), 3u);
    EXPECT_EQ(result.indices.footnote_refs[2].node_index, 3);
}

TEST(Parser, UndefinedFootnoteRefStaysPlainText)
{
    const auto result = ParseMarkdown("[^x] text");
    ASSERT_EQ(result.nodes.size(), 1u);
    EXPECT_EQ(result.nodes[0].GetText(), "[^x] text");
    EXPECT_TRUE(result.indices.footnote_refs.empty());
}

// GitHub と同じく、参照されない定義は表示しない。
TEST(Parser, UnreferencedFootnoteDefinitionIsHidden)
{
    const auto result = ParseMarkdown("Text\n\n[^1]: unused");
    ASSERT_EQ(result.nodes.size(), 1u);
    EXPECT_EQ(result.nodes[0].GetText(), "Text");
    EXPECT_TRUE(result.indices.footnote_def_indices.empty());
}

TEST(Parser, FootnoteRefInsideQuoteKeepsAlert)
{
    const auto nodes = ParseNodes("> [!NOTE]\n> See[^1]\n\n[^1]: n");
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(nodes[0].alert_type, AlertType::Note);
    EXPECT_TRUE(nodes[0].GetText().contains("See[1]"));
}

// md4c が URL を原文の外 (エスケープや文字参照を解いた一時バッファ) で渡したときだけ arena にコピーする。
// コピー漏れは callback 後に解放される領域を指し、全部コピーすると原文を指せる URL まで複製する。
TEST(Parser, LinkUrlPointsIntoRawUnlessDecoded)
{
    struct Case {
        std::string_view md;
        std::string_view url;
        bool in_raw;
    };
    static constexpr Case kCases[] = {
        { "[t](https://e.com/a)", "https://e.com/a", true },
        { "[t](https://e.com/a\\_b)", "https://e.com/a_b", false },
        // 文字参照は md4c が解かずに渡すので、パーサが解いて arena に置く
        { "[t](https://e.com/?a=1&amp;b=2)", "https://e.com/?a=1&b=2", false },
        { "[t](https://e.com/&#x41;&unknown;)", "https://e.com/A&unknown;", false },
        { "[t][r]\n\n[r]: https://e.com/a\\_b", "https://e.com/a_b", false },
        { "[t][r]\n\n[r]: https://e.com/?a&amp;b", "https://e.com/?a&b", false },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.md);
        const auto nodes = ParseNodes(c.md);
        ASSERT_GE(nodes.size(), 1u);
        EXPECT_TRUE(ParseInvariantsHold(c.md, nodes));
        const auto urls = nodes[0].view_link_urls();
        ASSERT_EQ(urls.size(), 1u);
        EXPECT_EQ(urls[0], c.url);
        EXPECT_EQ(IsSubrangeOf(urls[0], c.md), c.in_raw);
    }
}

// 同じノードの 2 つ目のリンクで一時領域を書き換えても、1 つ目の URL は arena に残っていること。
TEST(Parser, DecodedLinkUrlsDoNotShareScratch)
{
    constexpr std::string_view md = "[a](?x&amp;1) [b](?y&amp;2) [c](?x&amp;1)";
    const auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 1u);
    const auto urls = nodes[0].view_link_urls();
    ASSERT_EQ(urls.size(), 2u);
    EXPECT_EQ(urls[0], "?x&1");
    EXPECT_EQ(urls[1], "?y&2");
    const auto runs = nodes[0].runs();
    ASSERT_EQ(runs.size(), 3u);
    EXPECT_EQ(runs[2].link_url_index, runs[0].link_url_index);
}

// 画像の src も文字参照を解いてからパスとして使う。
TEST(Parser, ImageSrcResolvesCharacterReferences)
{
    const auto nodes = ParseNodes("![a](img/a&amp;b.png)");
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_NE(nodes[0].image_data(), nullptr);
    EXPECT_EQ(nodes[0].image_data()->src, "img/a&b.png");
}

// ---- 長いテキストの区切り ----

namespace {

// 区切ったノードはこの範囲に収まる (改行の後ろの空白や span が足す "$" の分だけ超えうる)。
constexpr size_t kChunkMax = kTextChunkTarget + kTextChunkWindow + 16;

// first から始まる区切ったノードの並びを、区切る前のテキストに戻す。
std::string JoinChunks(const std::vector<Node>& nodes, size_t first)
{
    std::string out{ nodes[first].GetText() };
    for (size_t i = first + 1; i < nodes.size() && nodes[i].continues_prev(); ++i) {
        out += ChunkSeparator(nodes[i]);
        out += nodes[i].GetText();
    }
    return out;
}

// ノードの先頭の語が、ソース位置の原文と一致するか。
bool StartsAtSourceOffset(std::string_view md, const Node& node)
{
    const auto text = node.GetText();
    const auto word = text.substr(0, text.find_first_of(" \n"));
    return node.HasSourceOffset() && md.substr(node.SourceOffset()).starts_with(word);
}

} // namespace

// 空行の無い長いテキストは、目安の長さ付近のソフト改行で区切る。改行の代わりの空白は前のノードに残る。
TEST(ParserChunks, LongParagraphSplitsAtSoftBreaks)
{
    const std::string md = MakeNumberedLines(4000);
    const auto nodes = ParseNodes(md);
    ASSERT_GT(nodes.size(), 3u);
    EXPECT_TRUE(ParseInvariantsHold(md, nodes));

    std::string expected = md.substr(0, md.size() - 1);
    std::ranges::replace(expected, '\n', ' ');
    EXPECT_EQ(JoinChunks(nodes, 0), expected);
    EXPECT_FALSE(nodes.front().continues_prev());
    EXPECT_FALSE(nodes.back().continues_next());
    for (size_t i = 0; i < nodes.size(); ++i) {
        SCOPED_TRACE(i);
        const auto text = nodes[i].GetText();
        EXPECT_EQ(nodes[i].type, NodeType::Paragraph);
        EXPECT_FALSE(nodes[i].newline_before_chunk());
        EXPECT_LE(text.size(), kChunkMax);
        EXPECT_TRUE(StartsAtSourceOffset(md, nodes[i]));
        if (i + 1 < nodes.size()) {
            EXPECT_GE(text.size(), kTextChunkTarget - kTextChunkWindow);
            EXPECT_TRUE(text.ends_with(' '));
        }
    }
}

// 改行の無い長い行は、目安の長さ付近の空白で区切る。
TEST(ParserChunks, LongLineSplitsAtSpaces)
{
    std::string md;
    for (int i = 0; i < 6000; ++i) {
        std::format_to(std::back_inserter(md), "word{} ", i);
    }
    md.pop_back();
    const auto nodes = ParseNodes(md);
    ASSERT_GT(nodes.size(), 2u);
    EXPECT_TRUE(ParseInvariantsHold(md, nodes));
    EXPECT_EQ(JoinChunks(nodes, 0), md);
    for (size_t i = 0; i + 1 < nodes.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_TRUE(nodes[i].GetText().ends_with(' '));
        EXPECT_LE(nodes[i].GetText().size(), kChunkMax);
        EXPECT_TRUE(StartsAtSourceOffset(md, nodes[i + 1]));
    }
}

// 空白の無い長い行は目安の長さで、UTF-8 の文字の途中を避けて区切る。
TEST(ParserChunks, LongLineWithoutSpacesSplitsAtCharacterBoundary)
{
    std::string md;
    for (int i = 0; i < 20000; ++i) {
        md += "あ";
    }
    const auto nodes = ParseNodes(md);
    ASSERT_GT(nodes.size(), 2u);
    EXPECT_TRUE(ParseInvariantsHold(md, nodes));
    EXPECT_EQ(JoinChunks(nodes, 0), md);
    for (const auto& node : nodes) {
        EXPECT_EQ(node.GetText().size() % 3, 0u);
        EXPECT_LE(node.GetText().size(), kChunkMax);
    }
}

// コードブロックは行末で区切り、区切った改行は続きのノードの印に移る。言語は続きにも引き継ぐ。
TEST(ParserChunks, LongCodeBlockSplitsAtLineEnds)
{
    const std::string code = MakeNumberedLines(3000);
    const std::string md = "```cpp\n" + code + "```\n";
    const auto nodes = ParseNodes(md);
    ASSERT_GT(nodes.size(), 2u);
    EXPECT_TRUE(ParseInvariantsHold(md, nodes));
    EXPECT_EQ(JoinChunks(nodes, 0), code.substr(0, code.size() - 1));
    for (size_t i = 0; i < nodes.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(nodes[i].type, NodeType::CodeBlock);
        EXPECT_EQ(nodes[i].code_language, SyntaxLanguage::Cpp);
        EXPECT_EQ(nodes[i].newline_before_chunk(), i > 0);
        EXPECT_FALSE(nodes[i].GetText().ends_with('\n'));
        EXPECT_TRUE(StartsAtSourceOffset(md, nodes[i]));
    }
}

// 区切りを待つ改行の後が空行だけでブロックが終わっても、空のノードを作らずに元のテキストに戻る。
TEST(ParserChunks, TrailingBlankLinesNearBoundaryKeepCodeText)
{
    for (size_t lines = 650; lines < 800; ++lines) {
        SCOPED_TRACE(lines);
        const std::string content = MakeNumberedLines(lines) + "\n\n";
        const std::string md = "```\n" + content + "```\n";
        const auto nodes = ParseNodes(md);
        ASSERT_TRUE(ParseInvariantsHold(md, nodes));
        ASSERT_EQ(JoinChunks(nodes, 0), content.substr(0, content.size() - 1));
    }
}

// 区切りをまたぐ装飾とリンクは、続きのノードにも掛かる。
TEST(ParserChunks, SpansContinueAcrossChunks)
{
    std::string body = MakeNumberedLines(2000);
    body.pop_back();
    // 区切りの空白は装飾の中にあるので、各ノードのテキスト全体に 1 つの run が掛かる。
    const auto expect_whole_run = [](const std::string& md, auto&& check) {
        const auto nodes = ParseNodes(md);
        ASSERT_GT(nodes.size(), 1u);
        EXPECT_TRUE(ParseInvariantsHold(md, nodes));
        for (size_t i = 0; i < nodes.size(); ++i) {
            SCOPED_TRACE(i);
            const auto runs = nodes[i].runs();
            ASSERT_EQ(runs.size(), 1u);
            EXPECT_EQ(runs[0].length, nodes[i].GetText().size());
            EXPECT_TRUE(check(nodes[i], runs[0]));
        }
    };
    const std::string bold = "**" + body + "**";
    expect_whole_run(bold, [](const Node&, const TextRun& r) { return r.bold(); });
    const std::string link = "[" + body + "](https://e.com/)";
    expect_whole_run(link, [](const Node& n, const TextRun& r) {
        return r.has_link() && n.view_link_urls().size() == 1 && n.view_link_urls()[0] == "https://e.com/";
    });
}

// 引用は同じ引用グループのまま区切り、Alert の種類は続きにも伝わる。
TEST(ParserChunks, QuoteChunksStayInAlertGroup)
{
    const std::string md = "> [!NOTE]\n" + MakeNumberedLines(2000, "> ");
    const auto nodes = ParseNodes(md);
    ASSERT_GT(nodes.size(), 1u);
    EXPECT_TRUE(ParseInvariantsHold(md, nodes));
    EXPECT_GT(nodes[0].alert_label_length(), 0u);
    for (const auto& node : nodes) {
        EXPECT_EQ(node.type, NodeType::BlockQuote);
        EXPECT_EQ(node.alert_type, AlertType::Note);
        EXPECT_EQ(node.alert_label_length() > 0, !node.continues_prev());
    }
}

// 見出し・リスト項目・図・表は区切らない (記号や下線が増え、図は 1 つのコードから描く)。
TEST(ParserChunks, OtherBlocksAreNotSplit)
{
    std::string long_line;
    for (int i = 0; i < 6000; ++i) {
        std::format_to(std::back_inserter(long_line), "w{} ", i);
    }
    long_line.pop_back();
    const std::pair<std::string, NodeType> cases[] = {
        { "# " + long_line,                                 NodeType::Heading  },
        { "- " + long_line,                                 NodeType::ListItem },
        { "```mermaid\n" + MakeNumberedLines(2000) + "```", NodeType::Diagram  },
        { "| a |\n|---|\n| " + long_line + " |",            NodeType::Table    },
    };
    for (const auto& [md, type] : cases) {
        const auto nodes = ParseNodes(md);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_EQ(nodes[0].type, type);
        EXPECT_TRUE(ParseInvariantsHold(md, nodes));
    }
}

// 区切った段落の続きは、画像を含んでいても画像ノードにしない (前の部分だけが本文として残るため)。
TEST(ParserChunks, ContinuationIsNotPromotedToImage)
{
    const std::string md = MakeNumberedLines(2000) + "![alt](a.png)";
    const auto nodes = ParseNodes(md);
    ASSERT_GT(nodes.size(), 1u);
    EXPECT_TRUE(ParseInvariantsHold(md, nodes));
    for (const auto& node : nodes) {
        EXPECT_EQ(node.type, NodeType::Paragraph);
    }
}

// 区切りを待つ間に空行が目安の長さを超えて続いても、順序を保って区切る。
TEST(ParserChunks, LongRunOfBlankLinesKeepsOrder)
{
    const std::string content = MakeNumberedLines(800) + std::string(40000, '\n') + "tail\n";
    const std::string md = "```\n" + content + "```\n";
    const auto nodes = ParseNodes(md);
    ASSERT_GT(nodes.size(), 3u);
    EXPECT_TRUE(ParseInvariantsHold(md, nodes));
    EXPECT_EQ(JoinChunks(nodes, 0), content.substr(0, content.size() - 1));
    for (const auto& node : nodes) {
        EXPECT_LE(node.GetText().size(), kChunkMax);
    }
}

// コードブロックで目安を超える行は、その行がノード 1 つに収まる限り前の改行で区切り、行の途中では区切らない
// (折り返さないコードで、無い改行が入って見えないように)。
TEST(ParserChunks, CodeLineThatWouldOverflowStartsNextChunk)
{
    std::string code;
    while (code.size() < 13 * 1024) {
        code += "short line\n";
    }
    std::string long_line = "    ";
    while (long_line.size() < 6 * 1024) {
        long_line += "tok ";
    }
    code += long_line + "\nend\n";
    const std::string md = "```cpp\n" + code + "```\n";
    const auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 2u);
    EXPECT_TRUE(ParseInvariantsHold(md, nodes));
    EXPECT_TRUE(nodes[1].newline_before_chunk());
    EXPECT_TRUE(nodes[1].GetText().starts_with(long_line));
    EXPECT_EQ(JoinChunks(nodes, 0), code.substr(0, code.size() - 1));
    for (const auto& node : nodes) {
        EXPECT_EQ(node.line_count, std::ranges::count(node.GetText(), '\n'));
    }
    EXPECT_TRUE(StartsAtSourceOffset(md, nodes[1]));
}

// ノード 1 つに収まらない長い行だけは、行の途中で区切る。
TEST(ParserChunks, CodeLineLongerThanChunkSplitsInsideLine)
{
    std::string line;
    while (line.size() < 40 * 1024) {
        line += "tok ";
    }
    const std::string md = "```cpp\nhead\n" + line + "\n```\n";
    const auto nodes = ParseNodes(md);
    ASSERT_GT(nodes.size(), 2u);
    EXPECT_TRUE(ParseInvariantsHold(md, nodes));
    EXPECT_EQ(JoinChunks(nodes, 0), "head\n" + line);
    for (const auto& node : nodes) {
        EXPECT_LE(node.GetText().size(), kChunkMax);
    }
}

// 空白の無い和文は、目安の前後にある句読点の直後で区切る (語の途中で改行して見えないように)。
TEST(ParserChunks, LongJapaneseLineSplitsAfterPunctuation)
{
    std::string md;
    for (int i = 0; i < 1500; ++i) {
        md += "日本語の文章です、続きます。";
    }
    const auto nodes = ParseNodes(md);
    ASSERT_GT(nodes.size(), 2u);
    EXPECT_TRUE(ParseInvariantsHold(md, nodes));
    EXPECT_EQ(JoinChunks(nodes, 0), md);
    for (size_t i = 0; i + 1 < nodes.size(); ++i) {
        SCOPED_TRACE(i);
        const auto text = nodes[i].GetText();
        EXPECT_TRUE(text.ends_with("。") || text.ends_with("、"));
        EXPECT_LE(text.size(), kChunkMax);
    }
}
