#include <gtest/gtest.h>
#include "document.h"
#include "document_test_helpers.h"
#include "test_helpers.h"
#include "document_utils.h"
#include "example_files.h"
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

bool IsQuoteParagraph(const Node& node)
{
    return node.type == NodeType::Paragraph && node.quote_depth > 0;
}

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

TEST(Parser, EmptyOrWhitespaceInputReturnsNoNodes)
{
    for (const std::string_view md : { "", "   \n\n  " }) {
        SCOPED_TRACE(md);
        EXPECT_TRUE(ParseNodes(md).empty());
    }
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

// md4c は強調の入れ子に上限が無い。256 段を超えても、内側を閉じたあとの外側の斜体が切れない。
TEST(Parser, DeeplyNestedEmphasisKeepsStyle)
{
    constexpr int kDepth = 300;
    std::string md;
    for (int i = 0; i < kDepth; ++i) {
        md += "*a ";
    }
    md += 'z';
    for (int i = 0; i < kDepth; ++i) {
        md += " a*";
    }
    auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 1u);
    const auto runs = nodes[0].runs();
    ASSERT_FALSE(runs.empty());
    EXPECT_TRUE(std::ranges::all_of(runs, &TextRun::italic));
    // 斜体が途中で切れると、残りの文字は run を持たない。
    EXPECT_EQ(runs.back().start + runs.back().length, nodes[0].GetText().size());
}

// インライン数式を持つノード (表はどれかのセル) の番号は、全ノードを見て集めたものと一致し、文書の順に並ぶ。
// 数式の組 (InlineMathPlan) はこの番号のノードしか見ない。区切った長い段落の続きのノードも数える。
TEST(Parser, MathIndicesListEveryNodeWithMath)
{
    std::string long_para;
    for (int i = 0; i < 3000; ++i) {
        long_para += std::format("word{0} $x_{0}$ ", i);
    }
    const std::string crafted = "# $h$\n\n$a$ text\n\nplain\n\n> [!NOTE]\n> $q$\n\n- $l$\n\n| $t$ | b |\n|---|---|\n| c | d |\n\n"
                                "| no | math |\n|---|---|\n| x | y |\n\n```math\nx^2\n```\n\n" +
                                long_para + "\n\nnote[^1]\n\n[^1]: $f$\n";
    const std::string example = ReadExampleFileBytes("example/test.md");
    ASSERT_FALSE(example.empty());
    for (const std::string* md : { &crafted, &example }) {
        const auto storage = ParseMarkdown(*md);
        std::vector<size_t> expected;
        for (size_t i = 0; i < storage.nodes.size(); ++i) {
            const Node& n = storage.nodes[i];
            const auto* tbl = n.table_data();
            if (tbl ? std::ranges::any_of(tbl->all_runs, &TextRun::math) : std::ranges::any_of(n.runs(), &TextRun::math)) {
                expected.push_back(i);
            }
        }
        EXPECT_GT(expected.size(), 8u);
        EXPECT_EQ(storage.indices.math_indices, expected);
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
        { "before [link text](https://example.com) after", "before link text after", "https://example.com" },
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

// ---- 水平線 ----

TEST(Parser, HorizontalRuleWithAsterisks)
{
    auto nodes = ParseNodes("***");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::HorizontalRule);
}

// ---- リスト ----

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
// 上限ちょうどは全部残し、超えた分は末尾から捨てる。打ち切り後もテキストは残し、改行は行数に数える。
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
    EXPECT_TRUE(IsQuoteParagraph(nodes[2]));
    EXPECT_EQ(nodes[2].GetText(), "text");
    EXPECT_EQ(nodes[2].blockquote_group, nodes[1].blockquote_group);
}

// ---- バグ #10: ネストされた引用ブロック ----

// 引用の段落の quote_depth は引用のネストの深さ。内側の引用を出た後は外側の深さに戻る (内側の終了には空行 `>` が要る)。
// group はネストの内外を問わず最外側のものを共有する (PR #156)。引用は字下げし、マーカーが無ければ Alert にしない。
TEST(Parser, BlockquoteDepthAndGroup)
{
    using Paragraphs = std::vector<std::pair<std::string_view, int>>;
    struct Case {
        std::string_view md;
        Paragraphs paragraphs; // {テキスト, quote_depth}
    };
    const Case kCases[] = {
        { "> quoted text", { { "quoted text", 1 } } },
        { "> outer\n>\n> > inner\n>\n> still outer", { { "outer", 1 }, { "inner", 2 }, { "still outer", 1 } } },
        { "> outer\n> > inner\n> > > deep", { { "outer", 1 }, { "inner", 2 }, { "deep", 3 } } },
        { "> outer\n> > inner\n>\n> back to outer", { { "outer", 1 }, { "inner", 2 }, { "back to outer", 1 } } },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.md);
        const auto nodes = ParseNodes(c.md);
        ASSERT_EQ(nodes.size(), c.paragraphs.size());
        EXPECT_GE(nodes[0].blockquote_group, 0);
        for (size_t i = 0; i < nodes.size(); ++i) {
            SCOPED_TRACE(i);
            EXPECT_TRUE(IsQuoteParagraph(nodes[i]));
            EXPECT_EQ(nodes[i].GetText(), c.paragraphs[i].first);
            EXPECT_EQ(nodes[i].quote_depth, c.paragraphs[i].second);
            EXPECT_EQ(nodes[i].blockquote_group, nodes[0].blockquote_group);
            EXPECT_GT(nodes[i].indent_level, 0);
            EXPECT_EQ(nodes[i].alert_type, AlertType::None);
            EXPECT_EQ(nodes[i].alert_label_length(), 0u);
        }
    }
}

// ---- 文字参照 ----

// 文字参照は UTF-8 に解いて前後の文字とつなぐ。文字にならない数 (U+10FFFF 超など) は、CommonMark と GitHub と同じく
// U+FFFD にする。参照ごとの解き方は HtmlEntity のテストで見る。原文の NUL はパースの前に置き換える (NormalizeSource)。
TEST(Parser, CharacterReferencesAreDecoded)
{
    struct Case {
        std::string_view md;
        std::string_view text;
    };
    constexpr Case kCases[] = {
        { "&#x1F600;", "\xF0\x9F\x98\x80" }, // 追加面 (bug #11)
        { "&#128169;", "\xF0\x9F\x92\xA9" }, // 追加面の 10 進表記
        { "&#x110000;", "\xEF\xBF\xBD" },
        { "a&#0;b", "a\xEF\xBF\xBD" "b" },
        { "a&lt;b&gt;c", "a<b>c" },
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

// 画像の src の文字にならない数値参照も U+FFFD にする (リンク先には NUL を書けない)。
TEST(Parser, NonCharacterReferenceInImageSrcBecomesReplacement)
{
    const auto nodes = ParseNodes("![a](x&#0;y.png)");
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].runs().size(), 1u);
    EXPECT_EQ(nodes[0].image_src(nodes[0].runs()[0]), "x\xEF\xBF\xBDy.png");
}

// ---- リスト ----

// 記号と番号の書式はリストの段で決まる。番号付きと順序なしは混ぜて数え、引用は数えない (issue#365)。
// 項目の深さはリストの入れ子だけを数え、深い項目ほど字下げする。番号は箇条書きなら 0。
TEST(Parser, ListDepthCountsOnlyListNesting)
{
    struct Case {
        std::string_view md;
        std::vector<int> depths;
        std::vector<int> numbers;
        std::string_view kinds; // 項目ごとに o = 番号付き、u = 箇条書き。空なら見ない
        std::vector<std::string_view> texts = {};
    };
    const Case kCases[] = {
        { "- item1\n- item2\n- item3", { 1, 1, 1 }, { 0, 0, 0 }, "uuu", { "item1", "item2", "item3" } },
        { "1. first\n2. second\n3. third", { 1, 1, 1 }, { 1, 2, 3 }, "ooo", { "first", "second", "third" } },
        { "5. five\n6. six", { 1, 1 }, { 5, 6 }, "oo" },
        // 番号が 0 でも番号付き (PR#249: 番号 > 0 で判定すると箇条書きと誤認する)。
        { "0. zero\n1. one", { 1, 1 }, { 0, 1 }, "oo" },
        { "- a\n  - b\n    - c\n      - d", { 1, 2, 3, 4 }, { 0, 0, 0, 0 } },
        { "1. a\n   1. b\n      1. c", { 1, 2, 3 }, { 1, 1, 1 } },
        { "1. a\n   - b\n     1. c", { 1, 2, 3 }, { 1, 0, 1 } },
        { "1. ordered\n   - unordered\n   - unordered2", { 1, 2, 2 }, { 1, 0, 0 } },
        { "> - a\n>   - b", { 1, 2 }, { 0, 0 } },
        { "- a\n\n  > - b", { 1, 2 }, { 0, 0 } },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.md);
        std::vector<int> depths;
        std::vector<int> numbers;
        std::vector<int> indents;
        std::string kinds;
        std::vector<std::string_view> texts;
        const auto nodes = ParseNodes(c.md);
        for (const auto& node : nodes) {
            if (node.type == NodeType::ListItem) {
                depths.push_back(node.list_depth());
                numbers.push_back(node.list_number());
                indents.push_back(node.indent_level);
                kinds += node.list_ordered() ? 'o' : 'u';
                texts.push_back(node.GetText());
            }
        }
        EXPECT_EQ(depths, c.depths);
        EXPECT_EQ(numbers, c.numbers);
        if (!c.kinds.empty()) {
            EXPECT_EQ(kinds, c.kinds);
        }
        if (!c.texts.empty()) {
            EXPECT_EQ(texts, c.texts);
        }
        ASSERT_FALSE(indents.empty());
        EXPECT_GT(indents[0], 0);
        for (size_t i = 1; i < indents.size(); ++i) {
            if (depths[i] > depths[i - 1]) {
                EXPECT_LT(indents[i - 1], indents[i]) << "i=" << i;
            }
        }
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
    EXPECT_TRUE(tbl->IsHeaderRow(0));
    EXPECT_FALSE(tbl->IsHeaderRow(1));
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

TEST(Parser, TableAlignment)
{
    auto nodes = ParseNodes(
                     "| L | C | R |\n"
                     "|:--|:--:|--:|\n"
                     "| a | b | c |");
    ASSERT_EQ(nodes.size(), 1u);
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

TEST(Parser, NodeTypesInComplexDocument)
{
    auto nodes = ParseNodes(
                     "# H\n\nP\n\n- L\n\n---\n\n```\nC\n```\n");
    for (const auto type : { NodeType::Heading, NodeType::Paragraph, NodeType::ListItem,
                             NodeType::HorizontalRule, NodeType::CodeBlock }) {
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

TEST(Parser, TableCellLinks)
{
    struct Case {
        std::string_view cell;
        std::string_view url;
        bool bold;
    };
    constexpr Case kCases[] = {
        { "[bar](https://example.com)", "https://example.com", false },
        { "[intro](#introduction)", "#introduction", false },
        { "[**bold**](https://example.com)", "https://example.com", true },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.cell);
        const std::string md = std::format("| Name | Link |\n|---|---|\n| foo | {} |", c.cell);
        auto nodes = ParseNodes(md);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_EQ(CellLinkUrl(nodes[0], 1, 1), c.url);
        EXPECT_EQ(CellLinkUrl(nodes[0], 1, 0), std::nullopt);
        EXPECT_EQ(std::ranges::any_of(nodes[0].table_data()->GetCellRuns(1, 1), &TextRun::bold), c.bold);
    }
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

// ---- Mermaid言語のコードブロック ----

TEST(Parser, MermaidCodeBlock)
{
    auto nodes = ParseNodes("```mermaid\ngraph TD;\n  A-->B;\n```");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Diagram);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Mermaid);
}

// ---- LaTeX display math ($$...$$) ----

// $$...$$ だけの段落は図にせず、段落いっぱいの kMathDoubleDollar の run を持つブロック数式にする。
TEST(Parser, LatexDisplayMathOnlyParagraphIsBlockMath)
{
    auto result = ParseMarkdown("$$E = mc^2$$");
    ASSERT_EQ(result.nodes.size(), 1u);
    const auto& node = result.nodes[0];
    EXPECT_EQ(node.type, NodeType::Paragraph);
    EXPECT_EQ(node.GetText(), "$$E = mc^2$$");
    ASSERT_EQ(node.runs().size(), 1u);
    EXPECT_TRUE(node.runs()[0].double_dollar_math());
    EXPECT_TRUE(IsBlockMath(node));
    EXPECT_TRUE(result.indices.diagram_indices.empty());
}

// 数式以外のテキストの種別ごとに、$$..$$ だけの段落かを判定する。HTML は表示しないので数えない。
// $$..$$ が複数ある段落もブロック数式にしない。
TEST(Parser, LatexBlockMathDependsOnOtherTextType)
{
    struct Case {
        std::string_view md;
        bool block;
    };
    for (const auto& c : {
             Case{ "before $$x+y$$ after", false },
             Case{ "$$a$$ $$b$$", false },
             Case{ "$$x$$ &amp;", false },
             Case{ "$$x$$&amp;", false },
             Case{ "$$x$$  \nnext", false },
             Case{ "$$x$$\nnext", false },
             Case{ "$$x$$ `c`", false },
             Case{ "$$x$$<!-- c -->", true },
         }) {
        SCOPED_TRACE(c.md);
        auto nodes = ParseNodes(c.md);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
        EXPECT_EQ(IsBlockMath(nodes[0]), c.block);
    }
}

// 長くて区切った $$..$$ はブロック数式にせず、本文のまま続きのノードに並べる。
TEST(Parser, LatexDisplayMathSplitIntoChunksIsNotBlockMath)
{
    std::string md = RepeatTo("x + ", kTextChunkTarget * 2, "$$");
    md += "y$$";
    auto nodes = ParseNodes(md);
    ASSERT_GT(nodes.size(), 1u);
    for (const auto& n : nodes) {
        EXPECT_EQ(n.type, NodeType::Paragraph);
        EXPECT_FALSE(IsBlockMath(n));
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
    std::string md = RepeatTo("word ", kTextChunkTarget + kTextChunkWindow - 64);
    md += "$";
    md = RepeatTo("x + ", kTextChunkTarget + kTextChunkWindow * 3, std::move(md));
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

TEST(Parser, LatexDisplayMathInBlockquoteIsBlockMath)
{
    auto nodes = ParseNodes("> $$y=x$$");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_TRUE(IsQuoteParagraph(nodes[0]));
    EXPECT_EQ(MathRunTexts(nodes[0]), (std::vector<std::string_view>{ "$$y=x$$" }));
    EXPECT_TRUE(IsBlockMath(nodes[0]));
}

// 本文と混ざった $$...$$ は、前後の $$ を含む範囲に kMathDoubleDollar の run を付ける。
TEST(Parser, LatexDisplayMathInTextMarksDisplayRun)
{
    auto nodes = ParseNodes("a $$x+y$$ b $z$");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_EQ(MathRunTexts(nodes[0]), (std::vector<std::string_view>{ "$$x+y$$", "$z$" }));
    EXPECT_TRUE(nodes[0].runs()[0].double_dollar_math());
    EXPECT_FALSE(nodes[0].runs()[1].double_dollar_math());
}

TEST(Parser, PlainDollarSignsNotMisdetectedAsMath)
{
    // "The price is $5 or $10." のような金額表記は LaTeX ではなくテキストとして扱う。
    // md4c は `$` の直後に空白・数字・記号が続く場合などインライン数式として解釈しないため、
    // フラグ有効化で既存のドル記号テキストが壊れないことを確認する。
    auto nodes = ParseNodes("The price is $5 or $10.");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_TRUE(MathRunTexts(nodes[0]).empty());
    EXPECT_TRUE(nodes[0].GetText().contains("$5"));
    EXPECT_TRUE(nodes[0].GetText().contains("$10"));
}

TEST(Parser, LatexBlockMathLineCountMatchesNewlines)
{
    const std::string_view sources[] = {
        "$$E = mc^2$$",
        "$$\nE = mc^2\n$$",
        "$$\na\nb\nc\n$$",
        "```math\na\nb\n```",
    };
    for (const auto src : sources) {
        SCOPED_TRACE(std::string{ src });
        auto nodes = ParseNodes(src);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
        ASSERT_TRUE(IsBlockMath(nodes[0]));
        const auto text = nodes[0].GetText();
        EXPECT_EQ(nodes[0].line_count, static_cast<int>(std::ranges::count(text, '\n')));
    }
}

TEST(Parser, LatexDisplayMathSurroundingParagraphs)
{
    auto result = ParseMarkdown("before\n\n$$E=mc^2$$\n\nafter");
    ASSERT_EQ(result.nodes.size(), 3u);
    EXPECT_FALSE(IsBlockMath(result.nodes[0]));
    EXPECT_TRUE(IsBlockMath(result.nodes[1]));
    EXPECT_EQ(result.nodes[1].GetText(), "$$E=mc^2$$");
    EXPECT_FALSE(IsBlockMath(result.nodes[2]));
    EXPECT_TRUE(result.indices.diagram_indices.empty());
}

// GitHub / GitLab 互換の ```math も $$...$$ だけの段落と同じブロック数式にする。中身の改行は保ち、前後を $$ で囲む。
TEST(Parser, MathFencedCodeBlockIsBlockMath)
{
    auto result = ParseMarkdown("before\n\n```math\n\\frac{a}{b}\n\\sqrt{x}\n```");
    ASSERT_EQ(result.nodes.size(), 2u);
    const auto& node = result.nodes[1];
    EXPECT_EQ(node.type, NodeType::Paragraph);
    EXPECT_EQ(node.code_language, SyntaxLanguage::None);
    EXPECT_EQ(node.GetText(), "$$\\frac{a}{b}\n\\sqrt{x}$$");
    EXPECT_EQ(node.line_count, 1);
    EXPECT_TRUE(IsBlockMath(node));
    EXPECT_TRUE(result.indices.diagram_indices.empty());
}

// 引用の中の ```math は引用の段落と同じ種別にする。空の ```math は数式が無いのでコードブロックのまま。
TEST(Parser, MathFencedCodeBlockInQuoteAndEmpty)
{
    auto quoted = ParseNodes("> ```math\n> x\n> ```");
    ASSERT_EQ(quoted.size(), 1u);
    EXPECT_TRUE(IsQuoteParagraph(quoted[0]));
    EXPECT_TRUE(IsBlockMath(quoted[0]));

    auto empty = ParseNodes("```math\n```");
    ASSERT_EQ(empty.size(), 1u);
    EXPECT_EQ(empty[0].type, NodeType::CodeBlock);
    EXPECT_TRUE(empty[0].runs().empty());
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

// 引用の先頭行がマーカーだけなら、マーカーはアイコンとラベルの行に置き換わり、本文はその次の行に続く。種別名の大小は問わない。
// Alert でなければマーカーも本文として残り、ソフト改行は従来どおり空白になる。
TEST(Parser, AlertMarkerOnFirstLine)
{
    struct Case {
        std::string_view md;
        AlertType type;
        std::string_view text; // Alert ならラベル行の後ろの本文、そうでなければ最後のノードのテキスト全体
    };
    constexpr Case kCases[] = {
        { "> [!NOTE]\n> This is a note", AlertType::Note, "This is a note" },
        { "> [!TIP]\n> Helpful advice", AlertType::Tip, "Helpful advice" },
        { "> [!IMPORTANT]\n> Key info", AlertType::Important, "Key info" },
        { "> [!WARNING]\n> Be careful", AlertType::Warning, "Be careful" },
        { "> [!CAUTION]\n> Don't do this", AlertType::Caution, "Don't do this" },
        { "> [!note]\n> lower case", AlertType::Note, "lower case" },
        { "> [!Note]\n> mixed case", AlertType::Note, "mixed case" },
        { "> [!NOTE]", AlertType::Note, "" },
        // マーカー行の後ろが空白や強制改行でも、本文が次の行なら Alert になる。
        { "> [!NOTE] \n> body", AlertType::Note, "body" },
        { "> [!NOTE]  \n> body", AlertType::Note, "body" },
        { "> [!NOTE]\\\n> body", AlertType::Note, "body" },
        // GitHub はマーカーと本文が同じ行にあると Alert にせず、通常の引用としてマーカーも表示する。
        { "> [!NOTE] same line", AlertType::None, "[!NOTE] same line" },
        { "> [!NOTE] same line\n> next", AlertType::None, "[!NOTE] same line next" },
        { "> [!NOTE] x\n> body", AlertType::None, "[!NOTE] x body" },
        { "> [!FOO]\n> body", AlertType::None, "[!FOO] body" },
        { "> [!NOTX]\n> body", AlertType::None, "[!NOTX] body" },
        { "> [!]\n> body", AlertType::None, "[!] body" },
        { "> [NOTE]\n> body", AlertType::None, "[NOTE] body" },
        { "> intro\n>\n> [!NOTE]\n> body", AlertType::None, "[!NOTE] body" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.md);
        const auto nodes = ParseNodes(c.md);
        ASSERT_FALSE(nodes.empty());
        const auto& node = nodes.back();
        EXPECT_TRUE(IsQuoteParagraph(node));
        EXPECT_EQ(node.alert_type, c.type);
        EXPECT_TRUE(ParseInvariantsHold(c.md, nodes));
        if (c.type == AlertType::None) {
            EXPECT_EQ(node.GetText(), c.text);
            EXPECT_EQ(node.alert_label_length(), 0u);
            continue;
        }
        ASSERT_EQ(nodes.size(), 1u);
        const std::string label{ AlertHeadText(c.type) };
        EXPECT_EQ(node.GetText(), c.text.empty() ? label : label + "\n" + std::string{ c.text });
        EXPECT_EQ(node.alert_label_length(), label.size());
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

// 複数段落の Alert は全ノードに種類を伝え、ラベルは最初のノードだけに付ける。
TEST(Parser, AlertMultiParagraphGrouping)
{
    auto nodes = ParseNodes("> [!NOTE]\n> First para\n>\n> Second para");
    EXPECT_GE(std::ranges::count_if(nodes, [](const Node& node) static { return IsQuoteParagraph(node) && node.alert_type == AlertType::Note; }), 2);
    EXPECT_EQ(std::ranges::count_if(nodes, [](const Node& node) static { return node.alert_label_length() > 0; }), 1);
}

// ノードを作らない引用は Alert の検出を素通りし、後ろの段落を引用の先頭と取り違えない。
TEST(Parser, AlertIgnoresQuoteWithoutNodes)
{
    for (const char* md : { ">\n\n[!NOTE]", "> <!-- c -->\n\n[!NOTE]", "- >\n- [!NOTE]" }) {
        SCOPED_TRACE(md);
        const auto nodes = ParseNodes(md);
        ASSERT_FALSE(nodes.empty());
        EXPECT_EQ(nodes.back().GetText(), "[!NOTE]");
        EXPECT_EQ(nodes.back().alert_type, AlertType::None);
        EXPECT_TRUE(ParseInvariantsHold(md, nodes));
    }
}

TEST(Parser, AlertFollowedByRegularBlockquote)
{
    auto nodes = ParseNodes("> [!NOTE]\n> Alert text\n\n> Normal quote");
    // Alert と通常の blockquote が混在
    auto quotes = nodes | std::views::filter([](const Node& node) static { return IsQuoteParagraph(node); });
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

namespace {

// 画像の run (alt の装飾で分かれた run は最初のものだけ) の src。
std::vector<std::string_view> ImageSrcsOf(const Node& node)
{
    std::vector<std::string_view> srcs;
    for (const auto& r : node.runs()) {
        if (r.image_start()) {
            srcs.push_back(node.image_src(r));
        }
    }
    return srcs;
}

} // namespace

// マーカー直後の画像は md4c では同じ段落になる。画像は段落の中の run なので、マーカーの段落がそのまま Alert になり、
// 画像はラベルの後ろに残る (GitHub と同じ)。別の段落の画像は Alert を引き継ぐ。
TEST(Parser, AlertMarkerFollowedByImageStaysAlert)
{
    for (const char* md : { "> [!NOTE]\n> ![img](p.png)", "> [!NOTE]\n>\n> ![img](p.png)" }) {
        SCOPED_TRACE(md);
        const auto nodes = ParseNodes(md);
        ASSERT_FALSE(nodes.empty());
        EXPECT_TRUE(IsQuoteParagraph(nodes[0]));
        EXPECT_TRUE(nodes[0].GetText().starts_with(AlertHeadText(AlertType::Note)));
        for (const auto& node : nodes) {
            EXPECT_EQ(node.alert_type, AlertType::Note);
            EXPECT_EQ(node.blockquote_group, nodes[0].blockquote_group);
        }
        EXPECT_EQ(ImageSrcsOf(nodes.back()), std::vector<std::string_view>{ "p.png" });
        EXPECT_TRUE(ParseInvariantsHold(md, nodes));
    }
}

// マーカーは先頭段落のものだけ。Alert でない引用や、先頭段落でないマーカーの画像は普通の引用の中の画像。
TEST(Parser, ImageInQuoteWithoutLeadingAlertMarkerIsPlainQuote)
{
    for (const char* md : { "> ![img](p.png)", "> [!NOTX]\n> ![img](p.png)", "> note ![img](p.png)", "> intro\n>\n> [!NOTE]\n> ![img](p.png)", "> [!NOTE] ![img](p.png)" }) {
        SCOPED_TRACE(md);
        const auto nodes = ParseNodes(md);
        ASSERT_FALSE(nodes.empty());
        EXPECT_EQ(ImageSrcsOf(nodes.back()), std::vector<std::string_view>{ "p.png" });
        for (const auto& node : nodes) {
            EXPECT_EQ(node.alert_type, AlertType::None);
        }
    }
}

// リンク付き画像も、Alert のラベルを付けた後でリンク URL と src を引ける。
TEST(Parser, AlertMarkerFollowedByLinkedImageKeepsLinkOnImage)
{
    const char* md = "> [!NOTE]\n> [![badge](b.svg)](https://x.example)";
    const auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].alert_type, AlertType::Note);
    const auto urls = nodes[0].view_link_urls();
    bool has_image = false;
    for (const auto& run : nodes[0].runs()) {
        if (run.image()) {
            has_image = true;
            ASSERT_TRUE(run.has_link());
            ASSERT_LT(static_cast<size_t>(run.link_url_index), urls.size());
            EXPECT_EQ(urls[static_cast<size_t>(run.link_url_index)], "https://x.example");
            EXPECT_EQ(nodes[0].image_src(run), "b.svg");
        }
    }
    EXPECT_TRUE(has_image);
    EXPECT_TRUE(ParseInvariantsHold(md, nodes));
}

// src が空の画像は alt を文字のまま Alert の本文に残す。
TEST(Parser, AlertMarkerFollowedByImageWithoutSrcStaysOneAlertNode)
{
    const auto nodes = ParseNodes("> [!NOTE]\n> ![alt]()");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_TRUE(IsQuoteParagraph(nodes[0]));
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
    EXPECT_TRUE(nodes[0].GetText().contains("bold"));
    EXPECT_TRUE(nodes[0].GetText().contains("code"));
    // ラベル以外の太字と code のランがあるべき
    EXPECT_TRUE(std::ranges::any_of(nodes[0].runs(), [](const TextRun& run) static { return run.bold() && run.start > 0; }));
    EXPECT_TRUE(std::ranges::any_of(nodes[0].runs(), &TextRun::code));
}

// ---- ソースオフセット ----

// source_offset はノードのテキストが始まる原文の UTF-8 byte 位置。テキストを持たない HR は前のノードの位置を受け継ぐ。
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
        { "---", { 0 } },
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

// 色々なブロックが混ざっても、source_offset は単調非減少で原文の中に収まる。
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
    EXPECT_EQ(nodes[3].SourceOffset(), nodes[2].SourceOffset()) << "末尾に並ぶ定義はソース位置の単調増加を崩さないよう前のノードの位置を受け継ぐ";
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
    ASSERT_EQ(nodes[0].runs().size(), 1u);
    EXPECT_EQ(nodes[0].image_src(nodes[0].runs()[0]), "img/a&b.png");
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
    return md.substr(node.SourceOffset()).starts_with(word);
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
    const std::string all = MakeNumberedLines(800);
    for (size_t lines = 650; lines < 800; ++lines) {
        SCOPED_TRACE(lines);
        // 先頭の lines 行 (MakeNumberedLines(lines) と同じ)。
        const std::string content = all.substr(0, all.find(std::format("line {} ", lines))) + "\n\n";
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
        EXPECT_TRUE(IsQuoteParagraph(node));
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
    std::string code = RepeatTo("short line\n", 13 * 1024);
    std::string long_line = RepeatTo("tok ", 6 * 1024, "    ");
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
    std::string line = RepeatTo("tok ", 40 * 1024);
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

// 装飾の無い段落やコードは原文を指す (コピーしない)。
// 字下げは md4c から静的な空白で届くが、原文も空白なら 1 行目が字下げされたコードも原文を指す。
// タブの字下げは空白に置き換わるのでコピーになる。
TEST(ParserRawView, PlainTextPointsIntoRaw)
{
    struct Case {
        const char* md;
        NodeType type;
        bool viewed;
    };
    constexpr Case kCases[] = {
        { "Just a plain paragraph without any markup.", NodeType::Paragraph, true },
        { "```cpp\nint x = 0;\nint y = 1;\n```", NodeType::CodeBlock, true },
        { "```\n    return x;\nend\n```\n", NodeType::CodeBlock, true },
        { "text\n\n```cpp\n  a\n    b\n```\n", NodeType::CodeBlock, true },
        { "```\n\tx\n```\n", NodeType::CodeBlock, false },
    };
    for (const auto& [md, type, viewed] : kCases) {
        auto doc = Document::FromMarkdown(md, L"test.md");
        const auto it = std::ranges::find(doc.GetNodes(), type, &Node::type);
        ASSERT_NE(it, doc.GetNodes().end()) << md;
        EXPECT_EQ(TextPointsIntoRaw(*it, doc.GetRawText()), viewed) << md;
        if (viewed) {
            EXPECT_EQ(doc.GetRawText().substr(it->SourceOffset(), it->GetText().size()), it->GetText()) << md;
        }
    }
}
