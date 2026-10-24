#include <gtest/gtest.h>
#include <algorithm>
#include <format>
#include <iterator>
#include <limits>
#include <memory_resource>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "document_utils.h"
#include "document_test_helpers.h"
#include "layout_computer.h"
#include "test_helpers.h"
#include "parser.h"
#include "selection_html.h"
#include "syntax.h"
#include "theme.h"
#include "theme_palette.h"
#include "utf8_fuzz_helpers.h"
#include "test_nodes.h"

using layout::CalcScrollYForDiff;

namespace {

TextSelection SelectAll(const std::pmr::vector<Node>& nodes)
{
    const int last = static_cast<int>(nodes.size()) - 1;
    return TextSelection::MakeOrdered(0, 0, last, static_cast<uint32_t>(nodes[last].GetText().size()));
}

std::pmr::string CopyAllAsHtml(std::string_view md, bool dark_mode = false)
{
    const auto nodes = ParseNodes(md);
    return ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), dark_mode);
}

TextRun MakeRun(uint32_t start, uint32_t length, int16_t link_url_index = -1)
{
    TextRun r;
    r.start = start;
    r.length = length;
    r.link_url_index = link_url_index;
    return r;
}

} // namespace

// ============================================================
// ExtractSelectedText
// ============================================================

// 全選択で倍々成長の再確保をしないよう、正確な長さで 1 回だけ確保する。
TEST(ExtractSelectedText, ReservesExactLength)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode("first paragraph"));
    nodes.push_back(MakeTextNode("second"));
    nodes.push_back(MakeTextNode("third one"));
    TextSelection sel = TextSelection::MakeOrdered(0, 6, 2, 5);
    sel.active = true;
    const auto text = ExtractSelectedText(nodes, sel);
    EXPECT_EQ(text, "paragraph\r\nsecond\r\nthird");
    // reserve は確保粒度 (16 byte) までの切り上げのみ許容する。
    EXPECT_LT(text.capacity(), text.size() + 16);
}

// CF_UNICODETEXT は CRLF が慣例なので、ノード内の改行 (コード行・表の行・BR) も CRLF にそろえる。
TEST(ExtractSelectedText, ConvertsNewlinesInsideNodesToCrlf)
{
    auto nodes = ParseNodes("```\na\nb\n```\n\n| h |\n|---|\n| c |\n\nx  \ny");
    ASSERT_EQ(nodes.size(), 3u);
    auto sel = TextSelection::MakeOrdered(0, 0, 2, std::numeric_limits<uint32_t>::max());
    sel.active = true;
    const auto text = ExtractSelectedText(nodes, sel);
    EXPECT_TRUE(text.starts_with("a\r\nb\r\n")) << text;
    EXPECT_TRUE(text.contains("h\r\nc")) << text;
    EXPECT_TRUE(text.ends_with("\r\nx\r\ny")) << text;
    EXPECT_LT(text.capacity(), text.size() + 16);
}

TEST(ExtractSelectedText, InactiveSelectionReturnsEmpty)
{
    auto nodes = ParseNodes("Hello world");
    TextSelection sel;
    sel.active = false;
    EXPECT_TRUE(ExtractSelectedText(nodes, sel).empty());
}

// [start, end) を切り出し、end はテキスト長でクランプする。位置は UTF-8 byte。
TEST(ExtractSelectedText, SingleNodeRange)
{
    struct Case {
        std::string_view md;
        uint32_t start;
        uint32_t end;
        std::string_view expected;
    };
    constexpr Case kCases[] = {
        { "Hello world", 0, 11, "Hello world" },
        { "Hello world", 0, 5, "Hello" },
        { "Hello world", 6, 11, "world" },
        { "Short", 0, 1000, "Short" },
        { "日本語テスト", 0, 18, "日本語テスト" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::Message() << c.md << " [" << c.start << ", " << c.end << ")");
        const auto nodes = ParseNodes(c.md);
        EXPECT_EQ(ExtractSelectedText(nodes, TextSelection::MakeOrdered(0, c.start, 0, c.end)), c.expected);
    }
}

// ノード間は \r\n で区切り、端のノードは選択範囲だけを出す。
TEST(ExtractSelectedText, MultipleNodesJoinedWithCrlf)
{
    auto nodes = ParseNodes("First\n\nSecond\n\nThird");
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(ExtractSelectedText(nodes, SelectAll(nodes)), "First\r\nSecond\r\nThird");
    EXPECT_EQ(ExtractSelectedText(nodes, TextSelection::MakeOrdered(0, 2, 2, 3)), "rst\r\nSecond\r\nThi");
}

TEST(ExtractSelectedText, EmptyNodes)
{
    std::pmr::vector<Node> nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, 5);
    // 範囲外のノード - クラッシュしないこと
    EXPECT_TRUE(ExtractSelectedText(nodes, sel).empty());
}

// ============================================================
// ExtractSelectedTextAsHtml
// ============================================================

TEST(ExtractSelectedTextAsHtml, InactiveSelectionReturnsEmpty)
{
    auto nodes = ParseNodes("Hello");
    TextSelection sel;
    sel.active = false;
    EXPECT_TRUE(ExtractSelectedTextAsHtml(nodes, sel, false).empty());
}

TEST(ExtractSelectedTextAsHtml, ParagraphWrapsInPTag)
{
    EXPECT_EQ(CopyAllAsHtml("Hello world"), "<p>Hello world</p>");
}

TEST(ExtractSelectedTextAsHtml, HeadingLevels)
{
    EXPECT_EQ(CopyAllAsHtml("## Section"), "<h2>Section</h2>");
}

TEST(ExtractSelectedTextAsHtml, BoldAndItalic)
{
    const auto html = CopyAllAsHtml("**bold** and *italic*");
    EXPECT_TRUE(html.contains("<strong>bold</strong>"));
    EXPECT_TRUE(html.contains("<em>italic</em>"));
}

TEST(ExtractSelectedTextAsHtml, InlineCode)
{
    EXPECT_TRUE(CopyAllAsHtml("text `code` more").contains("<code>code</code>"));
}

TEST(ExtractSelectedTextAsHtml, EscapesSpecialChars)
{
    std::pmr::vector<Node> nodes;
    nodes.emplace_back(MakeTextNode("a<b&c>d\"e'f"));
    EXPECT_EQ(ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), false), "<p>a&lt;b&amp;c&gt;d&quot;e&#39;f</p>");
}

TEST(ExtractSelectedTextAsHtml, CodeBlockIsEscapedInPreCode)
{
    auto nodes = ParseNodes("```\nint x = 1 < 2;\n```");
    ASSERT_EQ(nodes[0].type, NodeType::CodeBlock);
    auto html = ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), false);
    EXPECT_TRUE(html.contains("<pre"));
    EXPECT_TRUE(html.contains("<code>"));
    EXPECT_TRUE(html.contains("&lt;"));
    EXPECT_TRUE(html.contains("</code></pre>"));
    // コードブロック内ではインラインタグ化しない
    EXPECT_FALSE(html.contains("<strong>"));
    EXPECT_FALSE(html.contains("<em>"));
}

TEST(ExtractSelectedTextAsHtml, CodeBlockWithoutTokensHasNoSyntaxSpans)
{
    // 言語指定なし -> トークンが無い -> span タグは付かない
    auto nodes = ParseNodes("```\nplain text\n```");
    ASSERT_EQ(nodes[0].type, NodeType::CodeBlock);
    auto html = ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), false);
    EXPECT_FALSE(html.contains("<span"));
    EXPECT_TRUE(html.contains("plain text"));
}

TEST(ExtractSelectedTextAsHtml, CodeBlockIsHighlighted)
{
    auto nodes = ParseNodes("```cpp\nint x = 42;\n```");
    ASSERT_EQ(nodes[0].type, NodeType::CodeBlock);
    auto html = ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), false);
    EXPECT_TRUE(html.contains("<span style=\"color:#267f99\">int</span>"));
    EXPECT_TRUE(html.contains("<span style=\"color:#098658\">42</span>"));
    // その他の Plain 区間は素のテキスト
    EXPECT_TRUE(html.contains(" x = "));
}

TEST(ExtractSelectedTextAsHtml, CodeBlockSpanEscapesSpecialChars)
{
    const auto html = CopyAllAsHtml("```cpp\n\"a<b\"\n```");
    // span 内のテキストも HTML エスケープされる
    EXPECT_TRUE(html.contains("<span style=\"color:#a31515\">"));
    EXPECT_TRUE(html.contains("&lt;"));
    EXPECT_FALSE(html.contains("a<b"));
}

TEST(ExtractSelectedTextAsHtml, CodeBlockDarkModeUsesDarkColors)
{
    const auto html = CopyAllAsHtml("```cpp\nint x = 42;\n```", /*dark_mode=*/true);
    // ダーク用の色（VS Code Dark+ 相当）が使われる
    EXPECT_TRUE(html.contains("<span style=\"color:#4ec9b0\">int</span>"));
    EXPECT_TRUE(html.contains("<span style=\"color:#b5cea8\">42</span>"));
    // ライト用の色は混ざらない
    EXPECT_FALSE(html.contains("#267f99"));
    EXPECT_FALSE(html.contains("#098658"));
    // コードブロック背景がダーク色
    EXPECT_TRUE(html.contains("background-color:#2d2d2d"));
    EXPECT_TRUE(html.contains("color:#d4d4d4"));
}

TEST(ExtractSelectedTextAsHtml, TableRendersAsTableStructure)
{
    auto nodes = ParseNodes(
                     "| A | B |\n"
                     "|---|---|\n"
                     "| 1 | 2 |");
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].type, NodeType::Table);
    auto html = ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), false);
    EXPECT_TRUE(html.contains("<table"));
    EXPECT_TRUE(html.contains("<thead>"));
    EXPECT_TRUE(html.contains("<th"));
    EXPECT_TRUE(html.contains(">A</th>"));
    EXPECT_TRUE(html.contains(">B</th>"));
    EXPECT_TRUE(html.contains("</thead>"));
    EXPECT_TRUE(html.contains("<tbody>"));
    EXPECT_TRUE(html.contains("<td"));
    EXPECT_TRUE(html.contains(">1</td>"));
    EXPECT_TRUE(html.contains(">2</td>"));
    EXPECT_TRUE(html.contains("</tbody>"));
    EXPECT_TRUE(html.contains("</table>"));
    // フォールバックの <pre> は使われないこと
    EXPECT_FALSE(html.contains("<pre>"));
}

TEST(ExtractSelectedTextAsHtml, TableAlignmentAppliedAsTextAlign)
{
    auto nodes = ParseNodes(
                     "| L | C | R |\n"
                     "|:--|:--:|--:|\n"
                     "| a | b | c |");
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].type, NodeType::Table);
    auto html = ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), false);
    EXPECT_TRUE(html.contains("text-align:center;"));
    EXPECT_TRUE(html.contains("text-align:right;"));
}

TEST(ExtractSelectedTextAsHtml, TablePreservesInlineFormatting)
{
    auto nodes = ParseNodes(
                     "| A | B |\n"
                     "|---|---|\n"
                     "| **bold** | [link](https://example.com) |");
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].type, NodeType::Table);
    auto html = ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), false);
    EXPECT_TRUE(html.contains("<strong>bold</strong>"));
    EXPECT_TRUE(html.contains("<a href=\"https://example.com\">link</a>"));
}

// 表の部分選択はプレーンテキストと同じく読み順の範囲で切り取り、範囲にかかる行だけを出す。
// 範囲外のセルは表の形を保つため空セルになる。
TEST(ExtractSelectedTextAsHtml, TablePartialSelectionEmitsOnlySelectedRowsAndText)
{
    auto nodes = ParseNodes(
                     "| A | B |\n"
                     "|---|---|\n"
                     "| 1 | 2 |\n"
                     "| 3 | 4 |");
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].GetText(), "A\tB\n1\t2\n3\t4");
    const auto html = ExtractSelectedTextAsHtml(nodes, TextSelection::MakeOrdered(0, 6, 0, 9), false);
    EXPECT_FALSE(html.contains("<thead>"));
    EXPECT_FALSE(html.contains(">A</th>"));
    EXPECT_FALSE(html.contains(">1</td>"));
    EXPECT_FALSE(html.contains(">4</td>"));
    EXPECT_TRUE(html.contains(">2</td>"));
    EXPECT_TRUE(html.contains(">3</td>"));
    EXPECT_EQ(CountOccurrences(html, "<tr>"), 2u);
    EXPECT_EQ(CountOccurrences(html, "<td"), 4u);
}

TEST(ExtractSelectedTextAsHtml, TableSelectionInsideOneCellEmitsThatSubstring)
{
    auto nodes = ParseNodes(
                     "| hello | world |\n"
                     "|---|---|\n"
                     "| a | b |");
    ASSERT_EQ(nodes[0].GetText(), "hello\tworld\na\tb");
    const auto html = ExtractSelectedTextAsHtml(nodes, TextSelection::MakeOrdered(0, 1, 0, 4), false);
    EXPECT_TRUE(html.contains(">ell</th>"));
    EXPECT_FALSE(html.contains("world"));
    EXPECT_FALSE(html.contains("<tbody>"));
}

// 区切り文字 (セル間のタブ) だけを選んだ場合は、文字を含むセルが無いので表を出さない。
TEST(ExtractSelectedTextAsHtml, TableSelectionOfSeparatorOnlyEmitsNothing)
{
    auto nodes = ParseNodes(
                     "| A | B |\n"
                     "|---|---|\n"
                     "| 1 | 2 |");
    ASSERT_EQ(nodes[0].GetText(), "A\tB\n1\t2");
    EXPECT_EQ(ExtractSelectedTextAsHtml(nodes, TextSelection::MakeOrdered(0, 1, 0, 2), false), "");
}

// 空セルだけの行 (見出し無し表の空ヘッダ行など) も、選択範囲に丸ごと含まれれば出す。
TEST(ExtractSelectedTextAsHtml, TableFullSelectionKeepsEmptyHeaderRow)
{
    auto nodes = ParseNodes(
                     "| | |\n"
                     "|---|---|\n"
                     "| a | b |");
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].type, NodeType::Table);
    const auto html = ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), false);
    EXPECT_TRUE(html.contains("<thead>"));
    EXPECT_EQ(CountOccurrences(html, "<tr>"), 2u);
}

// 列数 0 の表 (md4c が TABLE detail を渡さない場合) でも範囲外を読まない。
TEST(ExtractSelectedTextAsHtml, TableWithoutColumnsDoesNotReadOutOfRange)
{
    std::pmr::vector<Node> nodes(1);
    nodes[0].type = NodeType::Table;
    auto* tbl = AttachTestTable(nodes[0]);
    tbl->row_count = 1;
    tbl->col_count = 0;
    tbl->cell_text_starts.push_back(0);
    tbl->cell_run_starts.push_back(0);
    tbl->is_header_row.push_back(false);
    const auto html = ExtractSelectedTextAsHtml(nodes, TextSelection::MakeOrdered(0, 0, 0, 1), false);
    EXPECT_FALSE(html.contains("<table"));
}

TEST(ExtractSelectedTextAsHtml, TableDarkModeUsesDarkBorder)
{
    auto nodes = ParseNodes(
                     "| A | B |\n"
                     "|---|---|\n"
                     "| 1 | 2 |");
    ASSERT_EQ(nodes[0].type, NodeType::Table);
    auto html = ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), /*dark_mode=*/true);
    EXPECT_TRUE(html.contains("border:1px solid #3c3c3c"));
    EXPECT_FALSE(html.contains("#d0d7de"));
}

TEST(ExtractSelectedTextAsHtml, TableWithoutDataFallsBackToPre)
{
    // table_data が空のノードに対しては <pre> フォールバックで出力される。
    Node n;
    n.type = NodeType::Table;
    n.SetText("fallback", 0);
    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(n));
    auto html = ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), false);
    EXPECT_FALSE(html.contains("<table"));
    EXPECT_TRUE(html.contains("<pre>fallback</pre>"));
}

TEST(ExtractSelectedTextAsHtml, ImageRendersAsImgTag)
{
    auto nodes = ParseNodes("![alt text](https://example.com/img.png)");
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].type, NodeType::Image);
    auto html = ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), false);
    EXPECT_TRUE(html.contains("<img src=\"https://example.com/img.png\""));
    EXPECT_TRUE(html.contains("alt=\"alt text\""));
    EXPECT_FALSE(html.contains("<pre>"));
}

TEST(ExtractSelectedTextAsHtml, ImageEscapesAttributesSafely)
{
    // alt と src の両方で & " がエスケープされること。
    auto nodes = ParseNodes("![Q&A \"x\"](path?a=1&b=2)");
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].type, NodeType::Image);
    auto html = ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), false);
    EXPECT_TRUE(html.contains("src=\"path?a=1&amp;b=2\""));
    EXPECT_TRUE(html.contains("Q&amp;A"));
    EXPECT_TRUE(html.contains("&quot;x&quot;"));
}

// 貼り付け先で読み込まれる <img src> は、リンクと同じく外部アクセスの経路になるスキームを外す。
TEST(ExtractSelectedTextAsHtml, ImageSrcDropsUnsafeSchemes)
{
    struct Case {
        std::string_view md;
        bool keeps_src;
    };
    constexpr Case kCases[] = {
        { "![x](https://example.com/a.png)", true },
        { "![x](HTTP://example.com/a.png)", true },
        { "![x](img/a.png)", true },
        { "![x](./a:b.png)", true },
        { "![x](/root/a.png)", true },
        { "![x](a.png?v=1)", true },
        { "![x](javascript:alert(1))", false },
        { "![x](JavaScript:alert(1))", false },
        { "![x](file:///C:/a.png)", false },
        { "![x](data:image/png;base64,AAAA)", false },
        { "![x](C:\\img\\a.png)", false },
        { "![x](//attacker/a.png)", false },
        // Markdown のエスケープで "\\\\" が "\\" になる。
        { "![x](\\\\\\\\attacker\\share\\a.png)", false },
        { "![x](/\\attacker/a.png)", false },
        { "![x](<java\tscript:alert(1)>)", false },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.md);
        auto nodes = ParseNodes(c.md);
        ASSERT_EQ(nodes.size(), 1u);
        ASSERT_EQ(nodes[0].type, NodeType::Image);
        const auto html = ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), false);
        EXPECT_EQ(html.contains(" src=\""), c.keeps_src) << html;
        EXPECT_TRUE(html.contains("alt=\"x\"")) << html;
    }
}

TEST(ExtractSelectedTextAsHtml, ListsWrapInUlAndOl)
{
    EXPECT_EQ(CopyAllAsHtml("- one\n- two"), "<ul><li>one</li><li>two</li></ul>");
    EXPECT_EQ(CopyAllAsHtml("1. first\n2. second"), "<ol><li>first</li><li>second</li></ol>");
    EXPECT_EQ(CopyAllAsHtml("1. [ ] first\n2. [x] second"),
              "<ol><li><input type=\"checkbox\" disabled> first</li><li><input type=\"checkbox\" checked disabled> second</li></ol>");
}

// loose list は LI が空の枠で本文が後ろの段落ノードになるが、段落を同じ <li> に入れてリストを保つ。
TEST(ExtractSelectedTextAsHtml, LooseListKeepsParagraphsInsideItems)
{
    auto nodes = ParseNodes("1. a\n\n2. b");
    EXPECT_EQ(ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), false), "<ol><li><p>a</p></li><li><p>b</p></li></ol>");
}

TEST(ExtractSelectedTextAsHtml, ListItemKeepsFollowingCodeBlock)
{
    auto nodes = ParseNodes("- a\n\n  ```\n  x\n  ```\n- b\n\nafter");
    const auto html = ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), false);
    EXPECT_TRUE(html.starts_with("<ul><li><p>a</p><pre")) << html;
    EXPECT_TRUE(html.ends_with("</li><li><p>b</p></li></ul><p>after</p>")) << html;
}

TEST(ExtractSelectedTextAsHtml, OrderedListKeepsStartNumber)
{
    auto nodes = ParseNodes("3. c\n4. d");
    EXPECT_EQ(ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), false), "<ol start=\"3\"><li>c</li><li>d</li></ol>");
}

// start は選択の先頭項目の番号。1 のときだけ省き、0 は省かない。種類が変わればリストを分ける。
TEST(ExtractSelectedTextAsHtml, OrderedListStartBoundaries)
{
    EXPECT_EQ(CopyAllAsHtml("0. a\n1. b"), "<ol start=\"0\"><li>a</li><li>b</li></ol>");
    EXPECT_EQ(CopyAllAsHtml("1. a\n- x"), "<ol><li>a</li></ol><ul><li>x</li></ul>");
    EXPECT_EQ(CopyAllAsHtml("3. [ ] t\n4. [x] u"),
              "<ol start=\"3\"><li><input type=\"checkbox\" disabled> t</li><li><input type=\"checkbox\" checked disabled> u</li></ol>");

    auto mid = ParseNodes("1. a\n2. b\n3. c");
    ASSERT_EQ(mid.size(), 3u);
    const auto from_second = TextSelection::MakeOrdered(1, 0, 2, 1);
    EXPECT_EQ(ExtractSelectedTextAsHtml(mid, from_second, false), "<ol start=\"2\"><li>b</li><li>c</li></ol>");
}

// loose list の段落の途中から選んでも、後続の項目の番号は start に出る。
TEST(ExtractSelectedTextAsHtml, LooseOrderedListFromParagraphKeepsNextStart)
{
    auto nodes = ParseNodes("1. a\n\n2. b");
    ASSERT_EQ(nodes.size(), 4u);
    const auto sel = TextSelection::MakeOrdered(1, 0, 3, 1);
    EXPECT_EQ(ExtractSelectedTextAsHtml(nodes, sel, false), "<p>a</p><ol start=\"2\"><li><p>b</p></li></ol>");
}

// 一度も描画していないコードブロックはコピー時にトークン化する。diff の追加/削除色も対応表から引けること
// (switch に default が無いので、対応が抜けると unreachable に落ちる)。
TEST(ExtractSelectedTextAsHtml, DiffCodeBlockUsesInsertedAndDeletedColors)
{
    const auto color = [](uint32_t rgb) static { return std::format("<span style=\"color:#{:06x}\">", rgb); };
    for (const bool dark : { false, true }) {
        SCOPED_TRACE(dark);
        const auto& palette = dark ? theme_palette::kDark : theme_palette::kLight;
        const auto html = CopyAllAsHtml("```diff\n+added\n-removed\n```", dark);
        EXPECT_TRUE(html.contains(color(palette.syntax_inserted) + "+added</span>")) << html;
        EXPECT_TRUE(html.contains(color(palette.syntax_deleted) + "-removed</span>")) << html;
    }
}

// トークンの途中で切れる選択は、選択内の部分だけをそのトークンの色で包む。
TEST(ExtractSelectedTextAsHtml, PartialCodeBlockSelectionSplitsToken)
{
    auto nodes = ParseNodes("```cpp\nint x = 42;\n```");
    const auto sel = TextSelection::MakeOrdered(0, 1, 0, 6);
    const auto html = ExtractSelectedTextAsHtml(nodes, sel, false);
    EXPECT_TRUE(html.contains("<code><span style=\"color:#267f99\">nt</span> x </code>")) << html;
    EXPECT_FALSE(html.contains(">int<")) << html;
    EXPECT_FALSE(html.contains("42")) << html;
}

// 図 (Mermaid / LaTeX) は書式付きコピーでは元のコードを <pre> で出す。項目内の図はその <li> に入る。
TEST(ExtractSelectedTextAsHtml, DiagramCopiesSourceAsPre)
{
    for (const std::string_view md : { std::string_view{ "$$x^2$$" }, std::string_view{ "```mermaid\ngraph TD\n```" } }) {
        SCOPED_TRACE(md);
        auto nodes = ParseNodes(md);
        ASSERT_EQ(nodes.size(), 1u);
        ASSERT_EQ(nodes[0].type, NodeType::Diagram);
        const auto html = ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), false);
        EXPECT_TRUE(html.starts_with("<pre")) << html;
        EXPECT_TRUE(html.contains(std::string(nodes[0].GetText()) + "</code></pre>")) << html;
    }
    const auto html = CopyAllAsHtml("- a\n\n  ```mermaid\n  g\n  ```\n- b");
    EXPECT_TRUE(html.starts_with("<ul><li><p>a</p><pre")) << html;
    EXPECT_TRUE(html.contains("g</code></pre></li><li>")) << html;
}

// 表のリンクの安全判定はセル間で共有する表を引く。添字がずれると危険な URL が残るか安全な URL が消える。
TEST(ExtractSelectedTextAsHtml, TableLinkSafetyIsPerUrl)
{
    auto nodes = ParseNodes("| [a](https://a.example) | [b](javascript:alert(1)) |\n|---|---|\n"
                            "| [c](javascript:alert(2)) | [d](https://d.example) |");
    ASSERT_EQ(nodes.size(), 1u);
    const auto html = ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), false);
    EXPECT_TRUE(html.contains("<a href=\"https://a.example\">a</a>")) << html;
    EXPECT_TRUE(html.contains("<a href=\"https://d.example\">d</a>")) << html;
    EXPECT_FALSE(html.contains("javascript")) << html;
    EXPECT_TRUE(html.contains(">b</th>")) << html;
    EXPECT_TRUE(html.contains(">c</td>")) << html;
}

// 判定結果の一時領域 (スタック 32 バイト) を超える数のリンクでも URL ごとの判定がずれない。
TEST(ExtractSelectedTextAsHtml, ManyLinksKeepPerUrlSafety)
{
    std::string md;
    for (int i = 0; i < 40; i++) {
        md += i % 2 == 0 ? std::format("[s{}](https://e.example/{}) ", i, i) : std::format("[u{}](javascript:x{}) ", i, i);
    }
    auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 1u);
    const auto html = ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), false);
    for (int i = 0; i < 40; i++) {
        SCOPED_TRACE(i);
        EXPECT_EQ(html.contains(std::format("<a href=\"https://e.example/{}\">s{}</a>", i, i)), i % 2 == 0);
    }
    EXPECT_FALSE(html.contains("javascript")) << html;
}

// 入れ子のリストは親の <li> の中に入れ、内側を出た後の親の本文や次の項目は親のリストに戻る。
TEST(ExtractSelectedTextAsHtml, NestedListsStayInsideParentItem)
{
    EXPECT_EQ(CopyAllAsHtml("- a\n  - b\n- c"), "<ul><li>a<ul><li>b</li></ul></li><li>c</li></ul>");
    EXPECT_EQ(CopyAllAsHtml("- a\n\n  - b\n\n  c\n- d"), "<ul><li><p>a</p><ul><li>b</li></ul><p>c</p></li><li><p>d</p></li></ul>");
    EXPECT_EQ(CopyAllAsHtml("- a\n  - b\n    - c\n- d"), "<ul><li>a<ul><li>b<ul><li>c</li></ul></li></ul></li><li>d</li></ul>");
    // 内側の番号付きリストは別の <ol> なので、外側の番号が内側の項目数だけずれない。
    EXPECT_EQ(CopyAllAsHtml("5. a\n   1. x\n6. b"), "<ol start=\"5\"><li>a<ol><li>x</li></ol></li><li>b</li></ol>");
}

// 区切り記号が違う番号付きリストは別のリスト。1 つにまとめると貼り付け先で番号が振り直される。
TEST(ExtractSelectedTextAsHtml, OrderedListsWithDifferentDelimitersStaySeparate)
{
    EXPECT_EQ(CopyAllAsHtml("1. a\n1) b"), "<ol><li>a</li></ol><ol><li>b</li></ol>");
}

// 引用の中の項目のさらに中の引用は、その項目に入る。項目の外の入れ子の引用はリストの外。
TEST(ExtractSelectedTextAsHtml, QuoteInsideItemInsideQuoteStaysInItem)
{
    const auto inside = CopyAllAsHtml("> - a\n>\n>   > q");
    EXPECT_TRUE(inside.ends_with("<blockquote>q</blockquote></li></ul>")) << inside;
    const auto outside = CopyAllAsHtml("> - a\n>\n> > q");
    EXPECT_TRUE(outside.ends_with("</li></ul><blockquote>q</blockquote>")) << outside;
}

TEST(ExtractSelectedTextAsHtml, QuoteAfterListIsOutsideList)
{
    auto nodes = ParseNodes("- a\n\n> q");
    EXPECT_EQ(ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), false), "<ul><li>a</li></ul><blockquote>q</blockquote>");
}

TEST(ExtractSelectedTextAsHtml, BlocksJoinWithoutSeparators)
{
    EXPECT_EQ(CopyAllAsHtml("> quoted text"), "<blockquote>quoted text</blockquote>");
    EXPECT_EQ(CopyAllAsHtml("before\n\n---\n\nafter"), "<p>before</p><hr><p>after</p>");
    EXPECT_EQ(CopyAllAsHtml("First\n\nSecond"), "<p>First</p><p>Second</p>");
}

TEST(ExtractSelectedTextAsHtml, PartialSelectionInParagraph)
{
    auto nodes = ParseNodes("Hello world");
    auto sel = TextSelection::MakeOrdered(0, 6, 0, 11);
    EXPECT_EQ(ExtractSelectedTextAsHtml(nodes, sel, false), "<p>world</p>");
}

// IsSafeUrlScheme を通らないリンクは外し、文字だけ残す。
TEST(ExtractSelectedTextAsHtml, LinkKeepsOnlySafeSchemes)
{
    struct Case {
        std::string_view md;
        std::string_view expected;
    };
    constexpr Case kCases[] = {
        { "[click](https://example.com)", "<p><a href=\"https://example.com\">click</a></p>" },
        { "[mail](mailto:user@example.com)", "<p><a href=\"mailto:user@example.com\">mail</a></p>" },
        { "[click](javascript:alert(1))", "<p>click</p>" },
        { "[open](file:///C:/secret.txt)", "<p>open</p>" },
        { "[sec](#section)", "<p>sec</p>" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.md);
        EXPECT_EQ(CopyAllAsHtml(c.md), c.expected);
    }
}

// ============================================================
// FindLinkAtPosition
// ============================================================

TEST(FindLinkAtPosition, ParsedParagraph)
{
    struct Case {
        std::string_view md;
        uint32_t pos;
        std::optional<std::string_view> expected;
    };
    const Case kCases[] = {
        { "plain text", 0, std::nullopt },
        { "[click](https://example.com)", 0, "https://example.com" },
        { "before [link](https://example.com) after", 0, std::nullopt },
        { "[section](#my-section)", 0, "#my-section" },
        // リンク文字列の最後の文字
        { "[link](https://example.com)", 3, "https://example.com" },
        { "[link](https://example.com)", 9999, std::nullopt },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::Message() << c.md << " pos=" << c.pos);
        const auto nodes = ParseNodes(c.md);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_EQ(FindLinkAtPosition(nodes[0], c.pos), c.expected);
    }
}

TEST(FindLinkAtPosition, EmptyNode)
{
    Node node;
    auto result = FindLinkAtPosition(node, 0);
    EXPECT_FALSE(result.has_value());
}

// ============================================================
// FindWordBoundaries
// ============================================================

namespace {

template <class SV>
struct WordCase {
    const char* name;
    SV text;
    uint32_t pos;
    bool found;
    uint32_t start;
    uint32_t end;
};

// 見つからない場合は {0, 0, false}
template <class SV, size_t N>
void ExpectWordBoundaries(const WordCase<SV> (&cases)[N])
{
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        const auto result = FindWordBoundaries(c.text, c.pos);
        EXPECT_EQ(result.found, c.found);
        EXPECT_EQ(result.start, c.start);
        EXPECT_EQ(result.end, c.end);
    }
}

} // namespace

// pos は UTF-8 byte 単位 (かな・漢字・全角は 3 byte、BMP 外は 4 byte)。
TEST(FindWordBoundaries, Utf8)
{
    constexpr WordCase<std::string_view> kCases[] = {
        { "EmptyText", "", 0, false, 0, 0 },
        { "SingleWord", "hello", 2, true, 0, 5 },
        { "WordAtStart", "hello world", 0, true, 0, 5 },
        { "WordAtEnd", "hello world", 6, true, 6, 11 },
        { "PositionOnSpace", "hello world", 5, false, 0, 0 },
        { "PositionOnPunctuation", "hello, world", 5, false, 0, 0 },
        { "WordWithUnderscore", "my_variable = 1", 3, true, 0, 11 },
        { "WordWithNumbers", "var123 = x", 3, true, 0, 6 },
        // 最後の文字にクランプされる
        { "PositionBeyondEnd", "hello", 100, true, 0, 5 },
        { "SingleCharWord", "a", 0, true, 0, 1 },
        { "AllSpaces", "   ", 1, false, 0, 0 },
        { "MixedPunctuationAndWords", "(hello)", 3, true, 1, 6 },
        // 同一文字種の連続区間を一塊にし、別カテゴリで止まる
        { "KatakanaSequence", "テスト test", 0, true, 0, 9 },
        { "AsciiWordAfterCjk", "テスト test", 10, true, 10, 14 },
        { "HiraganaSequence", "これはテスト", 0, true, 0, 9 },
        { "HanSequence", "日本語です", 3, true, 0, 9 },
        { "HiraganaAfterHan", "日本語です", 9, true, 9, 15 },
        // 長音「ー」(U+30FC) はカタカナと同カテゴリ
        { "KatakanaWithLongVowel", "コーヒー", 3, true, 0, 12 },
        { "FullwidthAlnumSequence", "ＡＢＣ１２３", 6, true, 0, 18 },
        // ハングルは空白で区切った語節を 1 単語にし、漢字とは別カテゴリ
        { "HangulSequence", "한국어 문장", 3, true, 0, 9 },
        { "HangulAfterSpace", "한국어 문장", 10, true, 10, 16 },
        { "HangulAfterHan", "韓國한국", 6, true, 6, 12 },
        // 英数字に続くハングル (助詞・助数詞) は同じ語
        { "HangulParticleAfterAscii", "API를 쓴다", 4, true, 0, 6 },
        { "HangulCounterAfterDigit", "3개", 0, true, 0, 4 },
        // 互換字母 (U+314B) もハングル
        { "HangulCompatibilityJamo", "ㅋㅋㅋ", 3, true, 0, 9 },
        // アクセント付きのラテン文字・ギリシャ文字・キリル文字も英数字と同じ語
        { "LatinAccentFromAscii", "café au lait", 0, true, 0, 5 },
        { "LatinAccentOnAccent", "café au lait", 3, true, 0, 5 },
        { "LatinSharpS", "Straße", 0, true, 0, 7 },
        { "LatinExtendedAdditional", "Việt Nam", 0, true, 0, 6 },
        { "LatinCombiningMark", "cafe\xCC\x81 noir", 0, true, 0, 6 },
        { "Cyrillic", "Привет мир", 4, true, 0, 12 },
        { "Greek", "Ελληνικά", 0, true, 0, 16 },
        // ×(U+00D7) は記号
        { "MultiplicationSignSplitsWord", "2×3", 0, true, 0, 1 },
        { "MultiplicationSignNotSelected", "2×3", 1, false, 0, 0 },
        // 全角句読点「、」(U+3001) は Other
        { "FullwidthSymbolNotSelected", "、", 0, false, 0, 0 },
        // 「々」(U+3005) は Han
        { "HanRepetitionMark", "人々", 0, true, 0, 6 },
        // 𠮷 = U+20BB7 (4 byte) と 田 (3 byte) はどちらも Han
        { "HanInSupplementaryPlane", "𠮷田", 0, true, 0, 7 },
        // マルチバイトの途中を指しても先頭バイトにスナップする
        { "PosOnUtf8ContinuationByte", "テスト", 1, true, 0, 9 },
        { "PosOnUtf8FourByteContinuation", "𠮷田", 2, true, 0, 7 },
        // 孤立した継続バイトは直前の文字に吸収せず単独の U+FFFD (単語外) として扱う。
        // 旧実装は後方走査で start がラップし範囲外を読んでいた。
        { "StrayContinuationBetweenHiragana", "あ\x82" "あ", 4, true, 4, 7 },
        { "StrayContinuationAfterHiragana", "あ\x82", 3, false, 0, 0 },
        { "StrayContinuationRunAfterAscii", "ab\x80\x80\x80\x80", 5, false, 0, 0 },
    };
    ExpectWordBoundaries(kCases);
}

// 不正 UTF-8 でも、前方 decode で UTF-16 化したテキスト上の単語範囲と一致する。
// UTF-16 側は後方走査がサロゲート 1 個分で自明なので、UTF-8 側の後方走査の oracle になる。
TEST(FindWordBoundaries, MalformedUtf8MatchesUtf16Conversion)
{
    static constexpr std::string_view kPieces[] = { "a", "_", "9", " ", "あ", "ア", "漢", "Ａ", "𠮷", "\xE3\x81", "\xF0\x9F\x98" };
    for (const uint32_t seed : utf8_fuzz::kFuzzSeeds) {
        std::mt19937 rng{ seed };
        for (int iter = 0; iter < 200; ++iter) {
            const std::string text = utf8_fuzz::RandomPiecesWithMalformed(rng, kPieces, 1, 10);
            SCOPED_TRACE(std::format("seed={} iter={} text={}", seed, iter, utf8_fuzz::HexEscape(text)));
            const std::string_view sv{ text };

            const auto bounds = utf8_fuzz::ForwardDecodeBoundaries(sv);
            std::wstring wide;
            std::vector<uint32_t> wide_at; // bounds[k] に対応する UTF-16 offset
            for (size_t k = 0; k + 1 < bounds.size(); ++k) {
                wide_at.push_back(static_cast<uint32_t>(wide.size()));
                const uint32_t cp = utf8_codec::DecodeAt(sv, bounds[k]).cp;
                if (cp >= 0x10000u) {
                    wide.push_back(static_cast<wchar_t>(0xD800u + ((cp - 0x10000u) >> 10)));
                    wide.push_back(static_cast<wchar_t>(0xDC00u + ((cp - 0x10000u) & 0x3FFu)));
                }
                else {
                    wide.push_back(static_cast<wchar_t>(cp));
                }
            }
            wide_at.push_back(static_cast<uint32_t>(wide.size()));
            const auto index_of = [&](uint32_t byte_pos) {
                return static_cast<size_t>(std::ranges::lower_bound(bounds, byte_pos) - bounds.begin());
            };

            // size 以上はクランプ経路
            for (uint32_t pos = 0; pos <= sv.size(); ++pos) {
                const auto r = FindWordBoundaries(sv, pos);
                const uint32_t clamped = std::min<uint32_t>(pos, static_cast<uint32_t>(sv.size()) - 1);
                const size_t k = static_cast<size_t>(std::ranges::upper_bound(bounds, clamped) - bounds.begin()) - 1;
                const auto rw = FindWordBoundaries(std::wstring_view{ wide }, wide_at[k]);
                ASSERT_EQ(r.found, rw.found) << "pos=" << pos;
                if (!r.found) {
                    continue;
                }
                ASSERT_LE(r.start, bounds[k]) << "pos=" << pos;
                ASSERT_LT(bounds[k], r.end) << "pos=" << pos;
                ASSERT_LE(r.end, sv.size()) << "pos=" << pos;
                const size_t si = index_of(r.start);
                const size_t ei = index_of(r.end);
                ASSERT_TRUE(si < bounds.size() && bounds[si] == r.start) << "start が文字境界にない pos=" << pos;
                ASSERT_TRUE(ei < bounds.size() && bounds[ei] == r.end) << "end が文字境界にない pos=" << pos;
                ASSERT_EQ(wide_at[si], rw.start) << "pos=" << pos;
                ASSERT_EQ(wide_at[ei], rw.end) << "pos=" << pos;
            }
        }
    }
}

// pos は UTF-16 コード単位 (BMP 外はサロゲートペアで 2 単位)。
TEST(FindWordBoundariesW, Utf16)
{
    constexpr WordCase<std::wstring_view> kCases[] = {
        { "SingleWord", L"hello world", 2, true, 0, 5 },
        { "PositionOnSpace", L"hello world", 5, false, 0, 0 },
        { "AsciiWordAfterCjk", L"テスト test", 4, true, 4, 8 },
        { "KatakanaSequence", L"テスト", 0, true, 0, 3 },
        { "HiraganaSequence", L"これはテスト", 0, true, 0, 3 },
        { "HanAndHiraganaBoundary", L"日本語です", 3, true, 3, 5 },
        { "HangulSequence", L"한국어 문장", 4, true, 4, 6 },
        { "LatinAccent", L"café au lait", 2, true, 0, 4 },
        { "HanSurrogatePair", L"𠮷田", 0, true, 0, 3 },
        // low surrogate を指しても high surrogate にスナップする
        { "PosOnLowSurrogate", L"𠮷田", 1, true, 0, 3 },
    };
    ExpectWordBoundaries(kCases);
}

// ============================================================
// FilenameView
// ============================================================

TEST(FilenameView, ReturnsLastPathComponent)
{
    struct Case {
        std::wstring_view path;
        std::wstring_view expected;
    };
    constexpr Case kCases[] = {
        { L"", L"" },
        { L"file.md", L"file.md" },
        { L"C:\\Users\\test\\file.md", L"file.md" },
        { L"C:/Users/test/file.md", L"file.md" },
        { L"C:\\dir/subdir\\file.md", L"file.md" },
        { L"\\\\server\\share\\file.md", L"file.md" },
        { L"C:\\dir\\", L"" },
        { L"C:\\ドキュメント\\ファイル.md", L"ファイル.md" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ c.path }));
        EXPECT_EQ(FilenameView(c.path), c.expected);
    }
}

// ============================================================
// BuildTitleString
// ============================================================

TEST(BuildTitleString, PrefixesFilenameToAppName)
{
    struct Case {
        std::wstring_view path;
        std::wstring_view expected;
    };
    constexpr Case kCases[] = {
        { L"", L"mendo" },
        { L"C:\\dir\\test.md", L"test.md - mendo" },
        { L"readme.md", L"readme.md - mendo" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ c.path }));
        EXPECT_EQ(BuildTitleString(c.path, 0), c.expected);
    }
}

// ============================================================
// 追加のエッジケース
// ============================================================

// ---- ExtractSelectedText 追加テスト ----

TEST(ExtractSelectedText, SelectionSpanningTableNode)
{
    // テーブル型ノード（線形化テキストを持つ）でのテスト
    Node table_node;
    table_node.type = NodeType::Table;
    table_node.SetText("A\tB\n1\t2", 1);

    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(table_node));
    EXPECT_EQ(ExtractSelectedText(nodes, TextSelection::MakeOrdered(0, 0, 0, 3)), "A\tB");
}

// issue #200: parser 経由で生成されたテーブル（線形化テキストは table_data->concat_text）に対し、selection が concat_text 内 offset を指しているとき、
// ExtractSelectedText がセル内テキストを返すこと。
TEST(ExtractSelectedText, ParsedTableCellWordSelection)
{
    auto nodes = ParseNodes(
                     "| Name | Value |\n"
                     "|------|-------|\n"
                     "| foo  | bar   |");
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].type, NodeType::Table);
    const auto* tbl = nodes[0].table_data();
    ASSERT_NE(tbl, nullptr);

    const std::string_view ct = tbl->concat_text;
    const auto pos = ct.find("foo");
    ASSERT_NE(pos, std::string_view::npos);

    auto sel = TextSelection::MakeOrdered(0, static_cast<uint32_t>(pos),
                                          0, static_cast<uint32_t>(pos + 3));
    EXPECT_EQ(ExtractSelectedText(nodes, sel), "foo");
}

TEST(ExtractSelectedText, ParsedTableFullSelectionPreservesSeparators)
{
    auto nodes = ParseNodes(
                     "| A | B |\n"
                     "|---|---|\n"
                     "| 1 | 2 |");
    ASSERT_EQ(nodes[0].type, NodeType::Table);
    ASSERT_EQ(nodes[0].GetText(), nodes[0].table_data()->concat_text);
    EXPECT_EQ(ExtractSelectedText(nodes, SelectAll(nodes)), "A\tB\r\n1\t2");
}

// 範囲外のノード番号は飛ばし、範囲内のノードだけを出す。
TEST(ExtractSelectedText, NodeIndexOutOfRange)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode("hello"));
    EXPECT_EQ(ExtractSelectedText(nodes, { .start_node = -5, .start_pos = 0, .end_node = 0, .end_pos = 5, .active = true }), "hello");
    EXPECT_TRUE(ExtractSelectedText(nodes, { .start_node = 0, .start_pos = 0, .end_node = 100, .end_pos = 5, .active = true }).starts_with("hello"));
}

// ---- FindLinkAtPosition 追加テスト ----

TEST(FindLinkHitAtPosition, ReturnsUrlAndIndex)
{
    Node node;
    node.SetText("link1 link2", 0);
    AddTestLinkUrl(node, "https://a.com");
    AddTestLinkUrl(node, "https://b.com");
    const TextRun runs[]{ MakeRun(0, 5, 0), MakeRun(6, 5, 1) };
    node.SetRuns(runs);

    const auto first = FindLinkHitAtPosition(node, 2);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->url, "https://a.com");
    EXPECT_EQ(first->index, 0u);
    const auto second = FindLinkHitAtPosition(node, 8);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->url, "https://b.com");
    EXPECT_EQ(second->index, 1u);
    // 2 つのリンクの間
    EXPECT_FALSE(FindLinkHitAtPosition(node, 5).has_value());
}

// run の二分探索の境界: run の終端は含まず、直後の run が始まる位置はその run に当たる。
// リンク以外の装飾 run を挟んでも、隣接するリンク run を取り違えない。
TEST(FindLinkHitAtPosition, BinarySearchBoundaries)
{
    Node node;
    SetTestText(node, "abcdefg", 0);
    const int16_t l0 = AddTestLinkUrl(node, "https://a.com");
    const int16_t l1 = AddTestLinkUrl(node, "https://b.com");
    const int16_t l2 = AddTestLinkUrl(node, "https://c.com");
    const auto run = [](uint32_t start, uint32_t length, int16_t link) static {
        TextRun r = MakeRun(start, length, link);
        r.set_bold(link < 0);
        return r;
    };
    const TextRun runs[]{ run(0, 2, l0), run(2, 1, -1), run(3, 2, l1), run(5, 1, l2) };
    SetTestRuns(node, runs);

    const auto index_at = [&](uint32_t pos) -> std::optional<uint32_t> {
        const auto hit = FindLinkHitAtPosition(node, pos);
        return hit ? std::optional{ hit->index } : std::nullopt;
    };
    EXPECT_EQ(index_at(0), 0u);
    EXPECT_EQ(index_at(1), 0u);
    EXPECT_EQ(index_at(2), std::nullopt);
    EXPECT_EQ(index_at(3), 1u);
    EXPECT_EQ(index_at(4), 1u);
    EXPECT_EQ(index_at(5), 2u);
    EXPECT_EQ(index_at(6), std::nullopt);
    EXPECT_EQ(index_at(100), std::nullopt);
}

// ---- FindLinkAtPosition: テーブルセル内のリンク ----

TEST(FindLinkAtPosition, TableCellLinkFound)
{
    // セル(1, 1)にリンクを持つテーブルノードを構築:
    // | Name | URL     |
    // | foo  | [bar](https://example.com) |
    // 線形化テキスト: "Name\tURL\nfoo\tbar"
    Node node;
    node.type = NodeType::Table;
    auto* tbl = AttachTestTable(node);
    const int16_t link = AddTestLinkUrl(node, "https://example.com");

    // concat 化: "Name\tURL\nfoo\tbar", cell_text_starts: [0, 5, 9, 13, 16]
    tbl->row_count = 2;
    tbl->col_count = 2;
    tbl->concat_text = "Name\tURL\nfoo\tbar";
    tbl->cell_text_starts = { 0u, 5u, 9u, 13u, 16u };
    tbl->all_runs = { MakeRun(0, 4), MakeRun(0, 3), MakeRun(0, 3), MakeRun(0, 3, link) };
    tbl->cell_run_starts = { 0u, 1u, 2u, 3u, 4u };
    tbl->aligns = { TableAlign::Default, TableAlign::Default };
    tbl->is_header_row = { true, false };

    // "bar"内の位置（オフセット13）でリンクが見つかるべき
    auto result = FindLinkAtPosition(node, 13);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "https://example.com");

    // "Name"内の位置（オフセット1）ではリンクが見つからないべき
    auto no_link = FindLinkAtPosition(node, 1);
    EXPECT_FALSE(no_link.has_value());

    // "foo"内の位置（オフセット9）ではリンクが見つからないべき
    auto no_link2 = FindLinkAtPosition(node, 9);
    EXPECT_FALSE(no_link2.has_value());
}

TEST(FindLinkAtPosition, TableCellLinkFromParsedMarkdown)
{
    auto nodes = ParseNodes(
                     "| Text | Link |\n"
                     "|------|------|\n"
                     "| hello | [click](https://example.com) |");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Table);

    // セル内にリンクのrunが存在することを確認
    const auto* tbl = nodes[0].table_data();
    ASSERT_GE(tbl->row_count, 2u);
    ASSERT_GE(tbl->col_count, 2u);
    bool has_link = false;
    for (const auto& run : tbl->GetCellRuns(1, 1)) {
        if (run.has_link()) {
            EXPECT_EQ(nodes[0].view_link_urls()[run.link_url_index], "https://example.com");
            has_link = true;
        }
    }
    EXPECT_TRUE(has_link);
}

TEST(FindLinkAtPosition, TableCellInternalLink)
{
    Node node;
    node.type = NodeType::Table;
    auto* tbl = AttachTestTable(node);
    const int16_t link = AddTestLinkUrl(node, "#my-section");

    // concat 化: 1 行 1 列の "section"
    tbl->row_count = 1;
    tbl->col_count = 1;
    tbl->concat_text = "section";
    tbl->cell_text_starts = { 0u, 7u };
    tbl->all_runs = { MakeRun(0, 7, link) };
    tbl->cell_run_starts = { 0u, 1u };
    tbl->aligns = { TableAlign::Default };
    tbl->is_header_row = { false };

    auto result = FindLinkAtPosition(node, 3);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "#my-section");
}

TEST(FindLinkAtPosition, TablePositionOnSeparator)
{
    // タブ/改行区切り上の位置ではリンクを返さないべき
    Node node;
    node.type = NodeType::Table;
    auto* tbl = AttachTestTable(node);
    const int16_t link = AddTestLinkUrl(node, "https://b.com");

    // concat 化: 1 行 2 列の "A\tB"
    tbl->row_count = 1;
    tbl->col_count = 2;
    tbl->concat_text = "A\tB";
    tbl->cell_text_starts = { 0u, 2u, 3u };
    tbl->all_runs = { MakeRun(0, 1), MakeRun(0, 1, link) };
    tbl->cell_run_starts = { 0u, 1u, 2u };
    tbl->aligns = { TableAlign::Default, TableAlign::Default };
    tbl->is_header_row = { false };

    // タブ区切り（オフセット1）はどのセルにもマッチしないべき
    EXPECT_FALSE(FindLinkAtPosition(node, 1).has_value());
    // "B"（オフセット2）でリンクが見つかるべき
    EXPECT_EQ(FindLinkAtPosition(node, 2), "https://b.com");
}

// ---- GetCellText: 末尾行が col_count 未満で padding された場合 ----

TEST(NodeTableData, GetCellTextShortLastRow)
{
    // 3 行 3 列のうち、最後の行が 1 セルしか持たないケース。OnLeaveBlock の padding で
    // 末尾の cell_text_starts が concat_text.size() に揃えられる。座標ベースの last_cell
    // 判定では (2,0) が end-start-1 = サイズ -1、(2,1) が underflow して SIZE_MAX を
    // 返してしまっていた。データ駆動 (end == concat_text.size()) で 0 を返すこと。
    Node node;
    node.type = NodeType::Table;
    auto* tbl = AttachTestTable(node);
    tbl->row_count = 3;
    tbl->col_count = 3;
    tbl->concat_text = "a\tb\tc\nd\te\tf\ng";
    // 実セル 7 個 + padding 2 個 + 番兵 1 = サイズ 10。padding と番兵は concat 末尾を指す。
    tbl->cell_text_starts = { 0u, 2u, 4u, 6u, 8u, 10u, 12u, 13u, 13u, 13u };
    tbl->cell_run_starts = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    tbl->aligns = { TableAlign::Default, TableAlign::Default, TableAlign::Default };
    tbl->is_header_row = { true, false, false };

    EXPECT_EQ(tbl->GetCellText(0, 0), "a");
    EXPECT_EQ(tbl->GetCellText(0, 2), "c");
    EXPECT_EQ(tbl->GetCellText(1, 2), "f");
    EXPECT_EQ(tbl->GetCellText(2, 0), "g");
    EXPECT_EQ(tbl->GetCellText(2, 1).size(), 0u);
    EXPECT_EQ(tbl->GetCellText(2, 2).size(), 0u);
}

// ============================================================
// FindFirstDifference (UTF-16 コード単位の差分検出)
// ============================================================

TEST(FindFirstDifference, ReturnsFirstDifferingByte)
{
    struct Case {
        std::string_view old_text;
        std::string_view new_text;
        size_t expected;
    };
    constexpr Case kCases[] = {
        { "hello", "hello", std::string_view::npos },
        { "", "", std::string_view::npos },
        { "abc", "xbc", 0 },
        { "abcdef", "abcXef", 3 },
        { "abc", "abX", 2 },
        { "abc", "abcdef", 3 },
        { "abcdef", "abc", 3 },
        { "", "new", 0 },
        { "old", "", 0 },
        // う E3 81 86 と え E3 81 88 は 3 byte 目で分かれるので 6 + 2
        { "あいう", "あいえ", 8 },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(c.old_text) + " -> " + ::testing::PrintToString(c.new_text));
        EXPECT_EQ(FindFirstDifference(c.old_text, c.new_text), c.expected);
    }
}

// 粗いチャンク比較 → 細かいチャンク → バイト単位の各境界で差分位置が正確に出る。
TEST(FindFirstDifference, LargeInputAcrossChunkBoundaries)
{
    const std::string base(200 * 1024, 'a');
    for (const size_t pos : { size_t{ 0 }, size_t{ 63 }, size_t{ 64 }, size_t{ 65535 }, size_t{ 65536 }, size_t{ 131071 }, base.size() - 1 }) {
        std::string changed = base;
        changed[pos] = 'b';
        EXPECT_EQ(FindFirstDifference(base, changed), pos) << "pos=" << pos;
    }
    EXPECT_EQ(FindFirstDifference(base, base), std::string_view::npos);
    EXPECT_EQ(FindFirstDifference(base, base + "x"), base.size());
}

// 末尾を削っただけの保存は何度読み直しても prefix shrink なので、上限で defer をやめる。
TEST(PrefixShrinkRetry, StopsDeferringAfterLimit)
{
    PrefixShrinkRetry retry;
    for (int i = 0; i < PrefixShrinkRetry::kMaxDefers; ++i) {
        EXPECT_FALSE(retry.Exhausted());
        EXPECT_TRUE(retry.TryDefer());
    }
    EXPECT_TRUE(retry.Exhausted());
    EXPECT_FALSE(retry.TryDefer());

    retry.Reset();
    EXPECT_FALSE(retry.Exhausted());
    EXPECT_TRUE(retry.TryDefer());
}

// ============================================================
// AnalyzeReloadDiff
// ============================================================

// diff_pos は UTF-8 byte。NoChange のときのみ npos。
TEST(AnalyzeReloadDiff, ClassifiesEdit)
{
    struct Case {
        std::string_view old_text;
        std::string_view new_text;
        ReloadOp op;
        size_t diff_pos;
    };
    constexpr Case kCases[] = {
        { "hello world", "hello world", ReloadOp::NoChange, std::string_view::npos },
        { "", "", ReloadOp::NoChange, std::string_view::npos },
        // 末尾追記も差し替えて差分位置へスクロールする
        { "abc", "abcdef", ReloadOp::Replace, 3 },
        { "", "new content", ReloadOp::Replace, 0 },
        { "あいう", "あいうえお", ReloadOp::Replace, 9 },
        // truncate→rewrite の前半の可能性があるため defer
        { "abcdef", "abc", ReloadOp::DeferPrefixShrink, 3 },
        { "old content", "", ReloadOp::DeferPrefixShrink, 0 },
        { "abcdef", "abcXef", ReloadOp::Replace, 3 },
        { "abc", "Xbc", ReloadOp::Replace, 0 },
        { "abcdef", "abcYYYz", ReloadOp::Replace, 3 },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(c.old_text) + " -> " + ::testing::PrintToString(c.new_text));
        const auto d = AnalyzeReloadDiff(c.old_text, c.new_text);
        EXPECT_EQ(d.op, c.op);
        EXPECT_EQ(d.diff_pos, c.diff_pos);
    }
}

// ============================================================
// FindNodeBySourceOffset
// ============================================================

TEST(FindNodeBySourceOffset, EmptyNodes)
{
    std::pmr::vector<Node> nodes;
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 0), -1);
}

TEST(FindNodeBySourceOffset, SingleNode)
{
    std::pmr::vector<Node> nodes(1);
    nodes[0].SetSourceOffset(0);
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 0), 0);
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 100), 0);
}

TEST(FindNodeBySourceOffset, OffsetBeforeAllNodes)
{
    std::pmr::vector<Node> nodes(2);
    nodes[0].SetSourceOffset(10);
    nodes[1].SetSourceOffset(20);
    // diff_offset=5 は最初のノード(offset=10)よりも前 → 該当なし
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 5), -1);
}

TEST(FindNodeBySourceOffset, ExactMatch)
{
    std::pmr::vector<Node> nodes(3);
    nodes[0].SetSourceOffset(0);
    nodes[1].SetSourceOffset(10);
    nodes[2].SetSourceOffset(25);
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 10), 1);
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 25), 2);
}

TEST(FindNodeBySourceOffset, BetweenNodes)
{
    std::pmr::vector<Node> nodes(3);
    nodes[0].SetSourceOffset(0);
    nodes[1].SetSourceOffset(10);
    nodes[2].SetSourceOffset(25);
    // offset=15 はノード1(10)とノード2(25)の間 → ノード1を返す
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 15), 1);
}

TEST(FindNodeBySourceOffset, SkipsUnsetOffsets)
{
    std::pmr::vector<Node> nodes(3);
    nodes[0].SetSourceOffset(0);
    // nodes[1] は SetSourceOffset を呼ばないため未設定
    nodes[2].SetSourceOffset(20);
    // 未設定値は常に diff_offset 以上にならない（kUnsetSourceOffset <= diff_offset は通常 false）
    // → ノード0を返す
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 10), 0);
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 20), 2);
}

TEST(FindNodeBySourceOffset, ParsedMarkdown)
{
    std::string_view md = "# Title\n\nParagraph\n\n## Section";
    auto nodes = ParseNodes(md);
    ASSERT_GE(nodes.size(), 3u);
    // 各ノードが有効な source_offset を持つ
    for (const auto& n : nodes) {
        EXPECT_NE(n.SourceOffset(), kUnsetSourceOffset);
    }
    // 最初のノードの offset は "# " の後 = 2
    EXPECT_EQ(nodes[0].SourceOffset(), 2u);
    // "Paragraph" は "# Title\n\n" = 9バイト目から
    EXPECT_EQ(nodes[1].SourceOffset(), 9u);
}

TEST(FindNodeBySourceOffset, LastNodeForLargeOffset)
{
    std::pmr::vector<Node> nodes(3);
    nodes[0].SetSourceOffset(0);
    nodes[1].SetSourceOffset(100);
    nodes[2].SetSourceOffset(200);
    // ソース末尾を超えるオフセット → 最後のノードを返す
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 999), 2);
}

TEST(FindNodeBySourceOffset, AllUnsetOffsets)
{
    // 全ノードが未設定（テキストなし）→ 該当なし
    std::pmr::vector<Node> nodes(3);
    // 何も設定しないため未設定
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 50), -1);
}

TEST(FindNodeBySourceOffset, MixedWithHorizontalRules)
{
    // パース結果で HorizontalRule が混在するケース
    std::string_view md = "AAA\n\n---\n\nBBB";
    auto nodes = ParseNodes(md);
    ASSERT_GE(nodes.size(), 3u);
    // "AAA" offset=0, "---" は未設定, "BBB" offset=10
    EXPECT_EQ(nodes[0].SourceOffset(), 0u);
    EXPECT_EQ(nodes[1].SourceOffset(), kUnsetSourceOffset);

    // offset=5（"---"のソース位置付近）→ AAA(offset=0)を返す（HRはスキップ）
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 5), 0);
    // offset=10（BBBのソース位置）→ BBBを返す
    int bbb_idx = FindNodeBySourceOffset(nodes, 10);
    EXPECT_EQ(bbb_idx, 2);
}

// ============================================================
// 統合テスト: diff検出 → ノード特定
// ============================================================

// ヘルパー: old→new の編集をシミュレートし、変更箇所のノードを特定する。
static int SimulateEditAndFindNode(std::string_view old_md, std::string_view new_md)
{
    size_t diff_pos = FindFirstDifference(old_md, new_md);
    if (diff_pos == std::string_view::npos) {
        return -1;
    }
    auto nodes = ParseNodes(new_md);
    if (nodes.empty()) {
        return -1;
    }
    return FindNodeBySourceOffset(nodes, diff_pos);
}

TEST(DiffToNode, EditMiddleParagraph)
{
    // 2番目の段落を編集
    std::string old_md = "First\n\nSecond\n\nThird";
    std::string new_md = "First\n\nModified\n\nThird";
    int node = SimulateEditAndFindNode(old_md, new_md);
    auto nodes = ParseNodes(new_md);
    EXPECT_EQ(node, 1); // 2番目の段落
    EXPECT_EQ(nodes[node].GetText(), "Modified");
}

TEST(DiffToNode, EditFirstParagraph)
{
    std::string old_md = "Hello\n\nWorld";
    std::string new_md = "Changed\n\nWorld";
    int node = SimulateEditAndFindNode(old_md, new_md);
    EXPECT_EQ(node, 0);
}

TEST(DiffToNode, EditLastParagraph)
{
    std::string old_md = "First\n\nSecond\n\nThird";
    std::string new_md = "First\n\nSecond\n\nChanged";
    int node = SimulateEditAndFindNode(old_md, new_md);
    EXPECT_EQ(node, 2); // 最後の段落
}

TEST(DiffToNode, InsertNewParagraph)
{
    // 段落を挿入
    std::string old_md = "Before\n\nAfter";
    std::string new_md = "Before\n\nInserted\n\nAfter";
    int node = SimulateEditAndFindNode(old_md, new_md);
    auto nodes = ParseNodes(new_md);
    ASSERT_GE(node, 0);
    // 挿入位置のノード（"Inserted" または "Before"の次）
    EXPECT_EQ(nodes[node].GetText(), "Inserted");
}

TEST(DiffToNode, DeleteParagraph)
{
    // 段落を削除
    std::string old_md = "First\n\nRemoveMe\n\nLast";
    std::string new_md = "First\n\nLast";
    int node = SimulateEditAndFindNode(old_md, new_md);
    ASSERT_GE(node, 0);
    // diff_pos=7（"RemoveMe" vs "Last"の開始位置）→ "Last"(offset=7)か"First"
    EXPECT_LE(node, 1); // "First" or "Last"
}

TEST(DiffToNode, AppendToEnd)
{
    std::string old_md = "Existing";
    std::string new_md = "Existing\n\nAppended";
    int node = SimulateEditAndFindNode(old_md, new_md);
    ASSERT_GE(node, 0);
    // diff_pos=8（old の末尾）→ "Existing"(offset=0)を返す
    // "Appended" の offset=10 > 8 なので "Existing" がマッチ
    EXPECT_LE(node, 1);
}

TEST(DiffToNode, EditInCodeBlock)
{
    // コードブロック内の編集
    std::string old_md = "text\n\n```\nold code\n```\n\nend";
    std::string new_md = "text\n\n```\nnew code\n```\n\nend";
    int node = SimulateEditAndFindNode(old_md, new_md);
    auto nodes = ParseNodes(new_md);
    ASSERT_GE(node, 0);
    EXPECT_EQ(nodes[node].type, NodeType::CodeBlock);
}

TEST(DiffToNode, EditInListItem)
{
    // リストアイテムの編集
    std::string old_md = "- first\n- second\n- third";
    std::string new_md = "- first\n- changed\n- third";
    int node = SimulateEditAndFindNode(old_md, new_md);
    auto nodes = ParseNodes(new_md);
    ASSERT_GE(node, 0);
    EXPECT_EQ(nodes[node].GetText(), "changed");
}

TEST(DiffToNode, EditHeading)
{
    // 見出しテキストの編集
    std::string old_md = "# Old Title\n\nBody";
    std::string new_md = "# New Title\n\nBody";
    int node = SimulateEditAndFindNode(old_md, new_md);
    EXPECT_EQ(node, 0); // 見出しノード
}

TEST(DiffToNode, NoChange)
{
    std::string md = "Same content";
    EXPECT_EQ(SimulateEditAndFindNode(md, md), -1);
}

TEST(DiffToNode, EditInBlockQuote)
{
    std::string old_md = "normal\n\n> old quote\n\nafter";
    std::string new_md = "normal\n\n> new quote\n\nafter";
    int node = SimulateEditAndFindNode(old_md, new_md);
    auto nodes = ParseNodes(new_md);
    ASSERT_GE(node, 0);
    EXPECT_EQ(nodes[node].type, NodeType::BlockQuote);
}

TEST(DiffToNode, EditWithJapanese)
{
    // 日本語テキストの編集
    std::string old_md = "# はじめに\n\n旧テキスト\n\nおわり";
    std::string new_md = "# はじめに\n\n新テキスト\n\nおわり";
    int node = SimulateEditAndFindNode(old_md, new_md);
    auto nodes = ParseNodes(new_md);
    ASSERT_GE(node, 0);
    // "# はじめに\n\n" = 2 + 15 + 2 = 19バイト
    // diff_pos は "新" vs "旧" の位置
    EXPECT_EQ(node, 1); // 2番目の段落
}

TEST(DiffToNode, EditInTable)
{
    std::string old_md =
        "| A | B |\n"
        "|---|---|\n"
        "| 1 | 2 |";
    std::string new_md =
        "| A | B |\n"
        "|---|---|\n"
        "| X | 2 |";
    int node = SimulateEditAndFindNode(old_md, new_md);
    auto nodes = ParseNodes(new_md);
    ASSERT_GE(node, 0);
    EXPECT_EQ(nodes[node].type, NodeType::Table);
}

TEST(DiffToNode, LargeDocumentMiddleEdit)
{
    // 多数のノードを持つ文書の中間を編集
    std::string old_md, new_md;
    for (int i = 0; i < 100; ++i) {
        old_md += "Paragraph " + std::to_string(i) + "\n\n";
        if (i == 50) {
            new_md += "CHANGED paragraph 50\n\n";
        }
        else {
            new_md += "Paragraph " + std::to_string(i) + "\n\n";
        }
    }
    int node = SimulateEditAndFindNode(old_md, new_md);
    auto nodes = ParseNodes(new_md);
    ASSERT_GE(node, 0);
    EXPECT_EQ(nodes[node].GetText(), "CHANGED paragraph 50");
}

// ============================================================
// CalcScrollYForDiff
// ============================================================

// ヘルパー: source_offset を等間隔に設定したノード列を構築する。
static std::pmr::vector<Node> MakeNodes(int count, size_t offset_step = 100)
{
    std::pmr::vector<Node> nodes(count);
    for (int i = 0; i < count; ++i) {
        nodes[i].SetSourceOffset(i * offset_step);
    }
    return nodes;
}

TEST(CalcScrollYForDiff, FallbackWhenNoNodes)
{
    std::pmr::vector<Node> nodes;
    LayoutCache cache;
    EXPECT_FLOAT_EQ(CalcScrollYForDiff(nodes, cache, "content", 0, 500.0f, 42.0f), 42.0f);
}

TEST(CalcScrollYForDiff, FallbackWhenDiffPosIsNpos)
{
    // npos = 差分位置なし (改行コード変換のみのリロード等)。現在位置を維持する。
    std::string content(300, 'x');
    auto nodes = MakeNodes(3, 100);
    auto cache = MakeUniformCache(3, 100.0f);
    EXPECT_FLOAT_EQ(
        CalcScrollYForDiff(nodes, cache, content, std::string_view::npos, 500.0f, 123.0f), 123.0f);
}

TEST(CalcScrollYForDiff, FallbackWhenNodeNotFound)
{
    // すべてのノードの source_offset が diff_pos より大きい。
    // content は MakeNodes が埋める最大 offset (200) + SetSourceOffset の上書き (50) を
    // string_view{base + offset, 0} で構築できるサイズを確保する必要がある。
    std::string content(300, 'x');
    auto nodes = MakeNodes(3, 100);
    nodes[0].SetSourceOffset(50);
    auto cache = MakeUniformCache(3);
    // diff_pos=10 < 全ノードの最小 offset(50) → -1 → fallback
    EXPECT_FLOAT_EQ(CalcScrollYForDiff(nodes, cache, content, 10, 500.0f, 99.0f), 99.0f);
}

TEST(CalcScrollYForDiff, ScrollsToNodeStartWithMargin)
{
    // 3ノード: offset=0,100,200 / y=0,100,200 / height=100
    std::string content(300, 'x');
    auto nodes = MakeNodes(3, 100);
    auto cache = MakeUniformCache(3, 100.0f);

    // diff_pos=0 → node 0, y=0, margin=500*0.2=100 → max(0, 0-100)=0
    EXPECT_FLOAT_EQ(CalcScrollYForDiff(nodes, cache, content, 0, 500.0f, 0.0f), 0.0f);

    // diff_pos=100 → node 1, y=100, margin=100 → max(0, 100-100)=0
    EXPECT_FLOAT_EQ(CalcScrollYForDiff(nodes, cache, content, 100, 500.0f, 0.0f), 0.0f);

    // diff_pos=200 → node 2, y=200, margin=100 → max(0, 200-100)=100
    EXPECT_FLOAT_EQ(CalcScrollYForDiff(nodes, cache, content, 200, 500.0f, 0.0f), 100.0f);
}

TEST(CalcScrollYForDiff, IntraNodeFractionInterpolation)
{
    // 2ノード: offset=0,100 / y=0,1000 / height=1000
    std::string content(200, 'x');
    auto nodes = MakeNodes(2, 100);
    auto cache = MakeUniformCache(2, 1000.0f);

    // diff_pos=50 → node 0 (offset=0), next_start=100
    // fraction = (50-0)/(100-0) = 0.5
    // node_y = 0 + 1000*0.5 = 500
    // margin = 100*0.2 = 20
    // result = max(0, 500-20) = 480
    EXPECT_FLOAT_EQ(CalcScrollYForDiff(nodes, cache, content, 50, 100.0f, 0.0f), 480.0f);
}

TEST(CalcScrollYForDiff, FractionClampsToOne)
{
    // diff_pos がノード範囲を超える場合でも fraction は 1.0 でクランプ
    std::string content(200, 'x');
    auto nodes = MakeNodes(2, 100);
    auto cache = MakeUniformCache(2, 1000.0f);

    // diff_pos=99 → node 0, fraction=99/100=0.99
    // y = 0 + 1000*0.99 = 990, scroll = 990-20 = 970
    EXPECT_FLOAT_EQ(CalcScrollYForDiff(nodes, cache, content, 99, 100.0f, 0.0f), 970.0f);
    // diff_pos=100 → node 1 (exact match), y=1000, scroll = 1000-20 = 980
    EXPECT_FLOAT_EQ(CalcScrollYForDiff(nodes, cache, content, 100, 100.0f, 0.0f), 980.0f);
}

TEST(CalcScrollYForDiff, SkipsUnsetSourceOffsets)
{
    // ノード1の source_offset が未設定の場合、ノード2を next_start として使う
    std::string content(300, 'x');
    std::pmr::vector<Node> nodes(3);
    nodes[0].SetSourceOffset(0);
    // nodes[1] は未設定（HorizontalRule等）
    nodes[2].SetSourceOffset(200);
    auto cache = MakeUniformCache(3, 100.0f);

    // diff_pos=100 → node 0 (offset=0), next valid = node 2 (offset=200)
    // fraction = (100-0)/(200-0) = 0.5
    // node_y = 0 + 100*0.5 = 50
    // margin = 500*0.2 = 100
    // result = max(0, 50-100) = 0
    EXPECT_FLOAT_EQ(CalcScrollYForDiff(nodes, cache, content, 100, 500.0f, 0.0f), 0.0f);
}

TEST(CalcScrollYForDiff, LastNodeUsesContentSizeAsNextStart)
{
    // 最後のノードでは content.size() が next_start として使われる
    std::string content(100, 'x');
    auto nodes = MakeNodes(1, 0);
    nodes[0].SetSourceOffset(0);
    auto cache = MakeUniformCache(1, 1000.0f);

    // diff_pos=50, next_start=content.size()=100
    // fraction = 50/100 = 0.5
    // node_y = 0 + 1000*0.5 = 500
    // margin = 200*0.2 = 40
    // result = max(0, 500-40) = 460
    EXPECT_FLOAT_EQ(CalcScrollYForDiff(nodes, cache, content, 50, 200.0f, 0.0f), 460.0f);
}

TEST(CalcScrollYForDiff, ParsedMarkdownIntegration)
{
    // 実際のMarkdownをパースして差分スクロール位置を計算する統合テスト
    std::string md = "# Title\n\nFirst paragraph\n\nSecond paragraph\n\nThird paragraph";
    auto nodes = ParseNodes(std::string_view{ md });
    ASSERT_GE(nodes.size(), 4u);

    // 等間隔 cache (text_top=0,50,100,...) を構築。spacing/Heading 個別寸法は無視し、
    // CalcScrollYForDiff が cache.Top(i) をどう参照するかだけを見る統合テスト。
    auto cache = MakeUniformCache(static_cast<int>(nodes.size()), 50.0f);

    // "Second paragraph" の先頭で diff
    size_t diff_pos = static_cast<size_t>(md.find("Second"));
    ASSERT_NE(diff_pos, std::string::npos);

    float result = CalcScrollYForDiff(nodes, cache, md, diff_pos, 500.0f, 0.0f);
    // スクロール位置は 0 以上で、fallback(0) とは異なる値が期待される
    EXPECT_GE(result, 0.0f);
}

// issue#146 回帰テスト:
// 末尾追記 (旧 PrefixGrowth) で旧コードが old_scroll を保持していたため、
// ユーザの編集場所にスクロールしなかった。AnalyzeReloadDiff と
// CalcScrollYForDiff の組合せで「fallback ではなく diff_pos に対応する
// 位置を返す」ことを担保する。
TEST(CalcScrollYForDiff, PrefixGrowthScrollsTowardAppendedTail)
{
    // 旧 doc: 3 段落
    const std::string old_md = "para_one\n\npara_two\n\npara_three";
    // 新 doc: 末尾に 1 段落追記
    const std::string new_md = old_md + "\n\npara_four_added";

    // AnalyzeReloadDiff で Replace と判定されること
    const auto decision = AnalyzeReloadDiff(old_md, new_md);
    ASSERT_EQ(decision.op, ReloadOp::Replace);
    ASSERT_EQ(decision.diff_pos, old_md.size());

    // 新 doc を pipeline で扱うイメージで cache を構築
    auto nodes = ParseNodes(std::string_view{ new_md });
    ASSERT_GE(nodes.size(), 4u);

    auto cache = MakeUniformCache(static_cast<int>(nodes.size()), 100.0f);
    const float last_old_node_y = cache.Top(nodes.size() - 2);
    const float appended_node_y = cache.Top(nodes.size() - 1);

    // ユーザが先頭付近 (scroll_y=0) を見ている状態で末尾追記が起きたシナリオ。
    // 旧コード (is_prefix_only ? old_scroll : ...) では desired_scroll が
    // current_scroll(=0) のまま fallback されていた。これが issue#146 の症状。
    constexpr float viewport_height = 500.0f;
    constexpr float current_scroll = 0.0f;
    const float result = CalcScrollYForDiff(nodes, cache, new_md,
                                            decision.diff_pos, viewport_height, current_scroll);

    // 修正後: 追記境界 (旧 doc 末尾) 近辺にスクロール。current_scroll とは
    // 顕著に異なる値が返ること。旧コードでは current_scroll(=0) がそのまま
    // 返っていたので、「margin 分以上」上回ることで回帰を検出できる。
    EXPECT_GT(result, current_scroll + viewport_height * 0.2f);
    // 着地点は最後の旧ノード〜追記ノードの境界付近 (margin で多少上)。
    EXPECT_GE(result, last_old_node_y - viewport_height * 0.2f);
    EXPECT_LE(result, appended_node_y);
}

// ============================================================
// ToLowerAsciiCopy
// ============================================================

TEST(ToLowerAsciiCopy, LowersOnlyAsciiLetters)
{
    struct Case {
        std::string_view in;
        std::string_view expected;
    };
    constexpr Case kCases[] = {
        { "", "" },
        { "HELLO", "hello" },
        { "hello", "hello" },
        { "HeLLo WoRLd", "hello world" },
        { "日本語", "日本語" },
        { "ABC-123_XYZ", "abc-123_xyz" },
        // A(0x41) の直前 @(0x40) と Z(0x5A) の直後 [(0x5B) は変換しない
        { "@A[Z", "@a[z" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(c.in));
        EXPECT_EQ(ToLowerAsciiCopy(c.in), c.expected);
    }
}

// ============================================================
// IsMarkdownFile
// ============================================================

TEST(IsMarkdownFile, MatchesMarkdownExtensionsCaseInsensitively)
{
    struct Case {
        std::wstring_view path;
        bool expected;
    };
    constexpr Case kCases[] = {
        { L"readme.md", true },
        { L"doc.markdown", true },
        { L"notes.mkd", true },
        { L"README.MD", true },
        { L"test.Markdown", true },
        { L"C:\\Users\\user\\Documents\\file.md", true },
        { L"C:\\my.project\\docs\\readme.md", true },
        { L"test.txt", false },
        { L"readme", false },
        { L"", false },
        { L".", false },
        { L"file.mdd", false },
        { L"page.html", false },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ c.path }));
        EXPECT_EQ(IsMarkdownFile(c.path), c.expected);
    }
}

// ============================================================
// AppendInlineHtml の LF バッチ化境界ケース
// ============================================================

// LF ごとに <br> を 1 個出し、前後のテキストはそのまま続ける。
TEST(ExtractSelectedTextAsHtml, EachLfBecomesBr)
{
    struct Case {
        const char* text;
        std::string_view expected;
    };
    constexpr Case kCases[] = {
        { "line1\nline2", "<p>line1<br>line2</p>" },
        { "\nafter", "<p><br>after</p>" },
        { "before\n", "<p>before<br></p>" },
        { "a\n\nb", "<p>a<br><br>b</p>" },
        { "\n", "<p><br></p>" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::string_view{ c.text }));
        std::pmr::vector<Node> nodes;
        nodes.push_back(MakeTextNode(c.text));
        EXPECT_EQ(ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), false), c.expected);
    }
}

// ============================================================
// ExtensionView / ParentDirectory
// ============================================================

TEST(ExtensionView, ReturnsDotExtensionOfLastComponent)
{
    struct Case {
        std::wstring_view path;
        std::wstring_view expected;
    };
    constexpr Case kCases[] = {
        { L"", L"" },
        { L"readme", L"" },
        { L"readme.md", L".md" },
        { L"archive.tar.gz", L".gz" },
        { L"C:\\docs\\readme.MD", L".MD" },
        // ディレクトリ名のドットは拡張子ではない
        { L"C:\\v1.2\\readme", L"" },
        { L"C:/v1.2/readme", L"" },
        { L".gitignore", L".gitignore" },
        { L"name.", L"." },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ c.path }));
        EXPECT_EQ(ExtensionView(c.path), c.expected);
    }
}

TEST(ParentDirectory, ReturnsDirectoryPart)
{
    struct Case {
        std::wstring_view path;
        std::wstring_view expected;
    };
    constexpr Case kCases[] = {
        { L"", L"" },
        { L"readme.md", L"" },
        { L"C:\\docs\\readme.md", L"C:\\docs" },
        { L"C:/docs/readme.md", L"C:/docs" },
        { L"C:\\readme.md", L"C:\\" },
        { L"\\\\server\\share\\readme.md", L"\\\\server\\share" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ c.path }));
        EXPECT_EQ(ParentDirectory(c.path), c.expected);
    }
}

// ============================================================
// コードブロックの HTML (コピーボタン)
// ============================================================

TEST(CodeBlockHtml, WrapsEscapedCodeInPre)
{
    const auto nodes = ParseNodes("```\na < b && c > d\n```");
    ASSERT_EQ(nodes.size(), 1u);
    const auto html = ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), false);
    EXPECT_TRUE(html.starts_with("<pre "));
    EXPECT_TRUE(html.ends_with("</code></pre>"));
    EXPECT_TRUE(html.contains("a &lt; b &amp;&amp; c &gt; d"));
    EXPECT_FALSE(html.contains("a < b"));
}

// ダーク時は貼り付け先の既定 (黒文字) で読めなくならないよう文字色を明示する。
TEST(CodeBlockHtml, DarkModeSpecifiesTextColor)
{
    const auto nodes = ParseNodes("```\ncode\n```");
    ASSERT_EQ(nodes.size(), 1u);
    const auto light = ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), false);
    const auto dark = ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), true);
    EXPECT_FALSE(light.contains(";color:"));
    EXPECT_TRUE(dark.contains(";color:"));
}

// ============================================================
// 区切ったテキストのコピー
// ============================================================

// 区切ったコードブロックは、区切る前と同じテキストと 1 つの <pre> としてコピーする。
TEST(ChunkCopy, SplitCodeCopiesAsOneBlock)
{
    const std::string code = MakeNumberedLines(3000);
    const std::string md = "```\n" + code + "```";
    const auto nodes = ParseNodes(md);
    ASSERT_GT(nodes.size(), 2u);

    std::string expected;
    for (const char c : std::string_view{ code }.substr(0, code.size() - 1)) {
        expected += c == '\n' ? std::string_view{ "\r\n" } : std::string_view{ &c, 1 };
    }
    EXPECT_EQ(ExtractSelectedText(nodes, SelectAll(nodes)), std::pmr::string{ expected });

    const auto html = ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), false);
    EXPECT_EQ(CountOccurrences(html, "<pre "), 1u);
    EXPECT_EQ(CountOccurrences(html, "</pre>"), 1u);
    EXPECT_EQ(CountOccurrences(html, "\n"), 2999u);
}

// 区切った段落は改行を挟まず、1 つの <p> としてコピーする。
TEST(ChunkCopy, SplitParagraphCopiesAsOneParagraph)
{
    const std::string md = MakeNumberedLines(4000);
    const auto nodes = ParseNodes(md);
    ASSERT_GT(nodes.size(), 2u);

    std::string expected = md.substr(0, md.size() - 1);
    std::ranges::replace(expected, '\n', ' ');
    EXPECT_EQ(ExtractSelectedText(nodes, SelectAll(nodes)), std::pmr::string{ expected });

    const auto html = ExtractSelectedTextAsHtml(nodes, SelectAll(nodes), false);
    EXPECT_EQ(CountOccurrences(html, "<p>"), 1u);
    EXPECT_EQ(CountOccurrences(html, "</p>"), 1u);
}

// 並びの途中だけを選んでも、選択の端で要素を開いて閉じる。
TEST(ChunkCopy, PartialSelectionInsideRunIsClosed)
{
    const std::string md = MakeNumberedLines(4000);
    const auto nodes = ParseNodes(md);
    ASSERT_GT(nodes.size(), 2u);
    const auto html = ExtractSelectedTextAsHtml(nodes, TextSelection::MakeOrdered(1, 0, 1, 10), false);
    EXPECT_TRUE(html.starts_with("<p>"));
    EXPECT_TRUE(html.ends_with("</p>"));
}

// 区切ったコードの続きは前のノードからの続きとしてトークン化するので、継ぎ目をまたぐブロックコメントも色が崩れない。
TEST(ChunkCopy, ContinuedCodeTokenizesFromPreviousChunk)
{
    const std::string md = "```cpp\n/*\n" + MakeNumberedLines(1000) + "*/\nint x;\n```";
    const auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 2u);
    ASSERT_TRUE(nodes[1].continues_prev());

    const auto tokens = TokenizeCodeNode(nodes[1]);
    ASSERT_FALSE(tokens.empty());
    EXPECT_EQ(tokens[0].start, 0u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
    const auto text = nodes[1].GetText();
    const auto keyword = text.find("int x;");
    ASSERT_NE(keyword, std::string_view::npos);
    EXPECT_TRUE(std::ranges::any_of(tokens, [&](const SyntaxToken& t) {
        return t.start == keyword && t.type != SyntaxTokenType::Comment;
    }));
    for (const auto& t : tokens) {
        EXPECT_LE(t.start + t.length, text.size());
    }
}
