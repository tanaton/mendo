#include <gtest/gtest.h>
#include <algorithm>
#include <format>
#include <iterator>
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
#include "reload.h"
#include "selection_html.h"
#include "syntax.h"
#include "theme.h"
#include "utf8_fuzz_helpers.h"

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

TEST(ExtractSelectedText, InactiveSelectionReturnsEmpty)
{
    auto nodes = ParseMarkdown("Hello world").nodes;
    TextSelection sel;
    sel.active = false;
    EXPECT_TRUE(ExtractSelectedText(nodes, sel).empty());
}

TEST(ExtractSelectedText, SingleNodeFullSelection)
{
    auto nodes = ParseMarkdown("Hello world").nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    EXPECT_EQ(ExtractSelectedText(nodes, sel), "Hello world");
}

TEST(ExtractSelectedText, SingleNodePartialSelection)
{
    auto nodes = ParseMarkdown("Hello world").nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, 5);
    EXPECT_EQ(ExtractSelectedText(nodes, sel), "Hello");
}

TEST(ExtractSelectedText, SingleNodeMiddleSelection)
{
    auto nodes = ParseMarkdown("Hello world").nodes;
    auto sel = TextSelection::MakeOrdered(0, 6, 0, 11);
    EXPECT_EQ(ExtractSelectedText(nodes, sel), "world");
}

TEST(ExtractSelectedText, MultipleNodesFullSelection)
{
    auto nodes = ParseMarkdown("First\n\nSecond\n\nThird").nodes;
    ASSERT_EQ(nodes.size(), 3u);
    auto sel = TextSelection::MakeOrdered(
        0, 0, 2, static_cast<uint32_t>(nodes[2].GetText().size()));
    auto result = ExtractSelectedText(nodes, sel);
    EXPECT_NE(result.find("First"), std::string::npos);
    EXPECT_NE(result.find("Second"), std::string::npos);
    EXPECT_NE(result.find("Third"), std::string::npos);
}

TEST(ExtractSelectedText, MultipleNodesPartialSelection)
{
    auto nodes = ParseMarkdown("First\n\nSecond\n\nThird").nodes;
    ASSERT_EQ(nodes.size(), 3u);
    // "First"の途中から"Third"の途中まで選択
    auto sel = TextSelection::MakeOrdered(0, 2, 2, 3);
    auto result = ExtractSelectedText(nodes, sel);
    EXPECT_EQ(result.substr(0, 3), "rst");
    EXPECT_NE(result.find("Second"), std::string::npos);
    EXPECT_NE(result.find("Thi"), std::string::npos);
}

TEST(ExtractSelectedText, NewlineBetweenNodes)
{
    auto nodes = ParseMarkdown("A\n\nB").nodes;
    ASSERT_EQ(nodes.size(), 2u);
    auto sel = TextSelection::MakeOrdered(
        0, 0, 1, static_cast<uint32_t>(nodes[1].GetText().size()));
    auto result = ExtractSelectedText(nodes, sel);
    // ノード間に\r\nが含まれるべき
    EXPECT_NE(result.find("\r\n"), std::string::npos);
}

TEST(ExtractSelectedText, EmptyNodes)
{
    std::pmr::vector<Node> nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, 5);
    // 範囲外のノード - クラッシュしないこと
    EXPECT_TRUE(ExtractSelectedText(nodes, sel).empty());
}

TEST(ExtractSelectedText, EndBeyondTextSize)
{
    auto nodes = ParseMarkdown("Short").nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, 1000);
    // end_posがテキストサイズを超える場合はクランプされるべき
    EXPECT_EQ(ExtractSelectedText(nodes, sel), "Short");
}

TEST(ExtractSelectedText, JapaneseText)
{
    auto nodes = ParseMarkdown("日本語テスト").nodes;
    auto sel = TextSelection::MakeOrdered(
        0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    EXPECT_EQ(ExtractSelectedText(nodes, sel), "日本語テスト");
}

// ============================================================
// ExtractSelectedTextAsHtml
// ============================================================

TEST(ExtractSelectedTextAsHtml, InactiveSelectionReturnsEmpty)
{
    auto nodes = ParseMarkdown("Hello").nodes;
    TextSelection sel;
    sel.active = false;
    EXPECT_TRUE(ExtractSelectedTextAsHtml(nodes, sel).empty());
}

TEST(ExtractSelectedTextAsHtml, ParagraphWrapsInPTag)
{
    auto nodes = ParseMarkdown("Hello world").nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    EXPECT_EQ(ExtractSelectedTextAsHtml(nodes, sel), "<p>Hello world</p>");
}

TEST(ExtractSelectedTextAsHtml, HeadingLevels)
{
    auto nodes = ParseMarkdown("## Section").nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    EXPECT_EQ(ExtractSelectedTextAsHtml(nodes, sel), "<h2>Section</h2>");
}

TEST(ExtractSelectedTextAsHtml, BoldAndItalic)
{
    auto nodes = ParseMarkdown("**bold** and *italic*").nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find("<strong>bold</strong>"), std::string::npos);
    EXPECT_NE(html.find("<em>italic</em>"), std::string::npos);
}

TEST(ExtractSelectedTextAsHtml, InlineCode)
{
    auto nodes = ParseMarkdown("text `code` more").nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find("<code>code</code>"), std::string::npos);
}

TEST(ExtractSelectedTextAsHtml, LinkProducesAnchor)
{
    auto nodes = ParseMarkdown("[click](https://example.com)").nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find("<a href=\"https://example.com\">click</a>"), std::string::npos);
}

TEST(ExtractSelectedTextAsHtml, EscapesSpecialChars)
{
    std::pmr::vector<Node> nodes;
    nodes.emplace_back(MakeTextNode("a<b&c>d\"e'f"));
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    EXPECT_EQ(ExtractSelectedTextAsHtml(nodes, sel),
              "<p>a&lt;b&amp;c&gt;d&quot;e&#39;f</p>");
}

TEST(ExtractSelectedTextAsHtml, CodeBlockIsEscapedInPreCode)
{
    auto nodes = ParseMarkdown("```\nint x = 1 < 2;\n```").nodes;
    ASSERT_EQ(nodes[0].type, NodeType::CodeBlock);
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find("<pre"), std::string::npos);
    EXPECT_NE(html.find("<code>"), std::string::npos);
    EXPECT_NE(html.find("&lt;"), std::string::npos);
    EXPECT_NE(html.find("</code></pre>"), std::string::npos);
    // コードブロック内ではインラインタグ化しない
    EXPECT_EQ(html.find("<strong>"), std::string::npos);
    EXPECT_EQ(html.find("<em>"), std::string::npos);
}

TEST(ExtractSelectedTextAsHtml, CodeBlockWithoutTokensHasNoSyntaxSpans)
{
    // 言語指定なし -> syntax_tokens が空 -> span タグは付かない
    auto nodes = ParseMarkdown("```\nplain text\n```").nodes;
    ASSERT_EQ(nodes[0].type, NodeType::CodeBlock);
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_EQ(html.find("<span"), std::string::npos);
    EXPECT_NE(html.find("plain text"), std::string::npos);
}

TEST(ExtractSelectedTextAsHtml, CodeBlockWithSyntaxTokensWrapsInSpans)
{
    // syntax_tokens を手動でセットし、span による色付けが行われることを確認
    auto nodes = ParseMarkdown("```cpp\nint x = 42;\n```").nodes;
    ASSERT_EQ(nodes[0].type, NodeType::CodeBlock);
    auto& n = nodes[0];
    const std::string_view text = n.GetText();
    ASSERT_FALSE(text.empty());
    // "int" を Keyword, "42" を Number としてマーク
    auto& tokens = n.syntax_tokens_mut();
    tokens.clear();
    const auto int_pos = static_cast<uint32_t>(text.find("int"));
    const auto num_pos = static_cast<uint32_t>(text.find("42"));
    ASSERT_NE(int_pos, static_cast<uint32_t>(std::string::npos));
    ASSERT_NE(num_pos, static_cast<uint32_t>(std::string::npos));
    tokens.push_back(SyntaxToken{ int_pos, 3u, SyntaxTokenType::Keyword });
    tokens.push_back(SyntaxToken{ num_pos, 2u, SyntaxTokenType::Number });

    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(text.size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find("<span style=\"color:#af00db\">int</span>"), std::string::npos);
    EXPECT_NE(html.find("<span style=\"color:#098658\">42</span>"), std::string::npos);
    // その他の Plain 区間は素のテキスト
    EXPECT_NE(html.find(" x = "), std::string::npos);
}

TEST(ExtractSelectedTextAsHtml, CodeBlockSpanEscapesSpecialChars)
{
    auto nodes = ParseMarkdown("```cpp\na<b\n```").nodes;
    auto& n = nodes[0];
    const std::string_view text = n.GetText();
    auto& tokens = n.syntax_tokens_mut();
    tokens.clear();
    // 全体を文字列トークンとしてマーク
    tokens.push_back(SyntaxToken{ 0u, static_cast<uint32_t>(text.size()),
                                  SyntaxTokenType::String });

    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(text.size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    // span 内のテキストも HTML エスケープされる
    EXPECT_NE(html.find("&lt;"), std::string::npos);
    EXPECT_EQ(html.find("a<b"), std::string::npos);
}

TEST(ExtractSelectedTextAsHtml, CodeBlockDarkModeUsesDarkColors)
{
    auto nodes = ParseMarkdown("```cpp\nint x = 42;\n```").nodes;
    auto& n = nodes[0];
    const std::string_view text = n.GetText();
    auto& tokens = n.syntax_tokens_mut();
    tokens.clear();
    const auto int_pos = static_cast<uint32_t>(text.find("int"));
    const auto num_pos = static_cast<uint32_t>(text.find("42"));
    tokens.push_back(SyntaxToken{ int_pos, 3u, SyntaxTokenType::Keyword });
    tokens.push_back(SyntaxToken{ num_pos, 2u, SyntaxTokenType::Number });

    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(text.size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel, /*dark_mode=*/true);
    // ダーク用の色（VS Code Dark+ 相当）が使われる
    EXPECT_NE(html.find("<span style=\"color:#c586c0\">int</span>"), std::string::npos);
    EXPECT_NE(html.find("<span style=\"color:#b5cea8\">42</span>"), std::string::npos);
    // ライト用の色は混ざらない
    EXPECT_EQ(html.find("#af00db"), std::string::npos);
    EXPECT_EQ(html.find("#098658"), std::string::npos);
    // コードブロック背景がダーク色
    EXPECT_NE(html.find("background-color:#2d2d2d"), std::string::npos);
    EXPECT_NE(html.find("color:#d4d4d4"), std::string::npos);
}

// 表の選択座標は LinearizedText (concat_text) の offset。
static TextSelection MakeTableFullSelection(const Node& table)
{
    return TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(table.LinearizedText().size()));
}

TEST(ExtractSelectedTextAsHtml, TableRendersAsTableStructure)
{
    auto nodes = ParseMarkdown(
                     "| A | B |\n"
                     "|---|---|\n"
                     "| 1 | 2 |")
                     .nodes;
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].type, NodeType::Table);
    auto sel = MakeTableFullSelection(nodes[0]);
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find("<table"), std::string::npos);
    EXPECT_NE(html.find("<thead>"), std::string::npos);
    EXPECT_NE(html.find("<th"), std::string::npos);
    EXPECT_NE(html.find(">A</th>"), std::string::npos);
    EXPECT_NE(html.find(">B</th>"), std::string::npos);
    EXPECT_NE(html.find("</thead>"), std::string::npos);
    EXPECT_NE(html.find("<tbody>"), std::string::npos);
    EXPECT_NE(html.find("<td"), std::string::npos);
    EXPECT_NE(html.find(">1</td>"), std::string::npos);
    EXPECT_NE(html.find(">2</td>"), std::string::npos);
    EXPECT_NE(html.find("</tbody>"), std::string::npos);
    EXPECT_NE(html.find("</table>"), std::string::npos);
    // フォールバックの <pre> は使われないこと
    EXPECT_EQ(html.find("<pre>"), std::string::npos);
}

TEST(ExtractSelectedTextAsHtml, TableAlignmentAppliedAsTextAlign)
{
    auto nodes = ParseMarkdown(
                     "| L | C | R |\n"
                     "|:--|:--:|--:|\n"
                     "| a | b | c |")
                     .nodes;
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].type, NodeType::Table);
    auto sel = MakeTableFullSelection(nodes[0]);
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find("text-align:center;"), std::string::npos);
    EXPECT_NE(html.find("text-align:right;"), std::string::npos);
}

TEST(ExtractSelectedTextAsHtml, TablePreservesInlineFormatting)
{
    auto nodes = ParseMarkdown(
                     "| A | B |\n"
                     "|---|---|\n"
                     "| **bold** | [link](https://example.com) |")
                     .nodes;
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].type, NodeType::Table);
    auto sel = MakeTableFullSelection(nodes[0]);
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find("<strong>bold</strong>"), std::string::npos);
    EXPECT_NE(html.find("<a href=\"https://example.com\">link</a>"), std::string::npos);
}

// 表の部分選択はプレーンテキストと同じく読み順の範囲で切り取り、範囲にかかる行だけを出す。
// 範囲外のセルは表の形を保つため空セルになる。
TEST(ExtractSelectedTextAsHtml, TablePartialSelectionEmitsOnlySelectedRowsAndText)
{
    auto nodes = ParseMarkdown(
                     "| A | B |\n"
                     "|---|---|\n"
                     "| 1 | 2 |\n"
                     "| 3 | 4 |")
                     .nodes;
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].LinearizedText(), "A\tB\n1\t2\n3\t4");
    const auto html = ExtractSelectedTextAsHtml(nodes, TextSelection::MakeOrdered(0, 6, 0, 9));
    EXPECT_EQ(html.find("<thead>"), std::string::npos);
    EXPECT_EQ(html.find(">A</th>"), std::string::npos);
    EXPECT_EQ(html.find(">1</td>"), std::string::npos);
    EXPECT_EQ(html.find(">4</td>"), std::string::npos);
    EXPECT_NE(html.find(">2</td>"), std::string::npos);
    EXPECT_NE(html.find(">3</td>"), std::string::npos);
    EXPECT_EQ(CountOccurrences(html, "<tr>"), 2u);
    EXPECT_EQ(CountOccurrences(html, "<td"), 4u);
}

TEST(ExtractSelectedTextAsHtml, TableSelectionInsideOneCellEmitsThatSubstring)
{
    auto nodes = ParseMarkdown(
                     "| hello | world |\n"
                     "|---|---|\n"
                     "| a | b |")
                     .nodes;
    ASSERT_EQ(nodes[0].LinearizedText(), "hello\tworld\na\tb");
    const auto html = ExtractSelectedTextAsHtml(nodes, TextSelection::MakeOrdered(0, 1, 0, 4));
    EXPECT_NE(html.find(">ell</th>"), std::string::npos);
    EXPECT_EQ(html.find("world"), std::string::npos);
    EXPECT_EQ(html.find("<tbody>"), std::string::npos);
}

// 区切り文字 (セル間のタブ) だけを選んだ場合は、文字を含むセルが無いので表を出さない。
TEST(ExtractSelectedTextAsHtml, TableSelectionOfSeparatorOnlyEmitsNothing)
{
    auto nodes = ParseMarkdown(
                     "| A | B |\n"
                     "|---|---|\n"
                     "| 1 | 2 |")
                     .nodes;
    ASSERT_EQ(nodes[0].LinearizedText(), "A\tB\n1\t2");
    EXPECT_EQ(ExtractSelectedTextAsHtml(nodes, TextSelection::MakeOrdered(0, 1, 0, 2)), "");
}

// 空セルだけの行 (見出し無し表の空ヘッダ行など) も、選択範囲に丸ごと含まれれば出す。
TEST(ExtractSelectedTextAsHtml, TableFullSelectionKeepsEmptyHeaderRow)
{
    auto nodes = ParseMarkdown(
                     "| | |\n"
                     "|---|---|\n"
                     "| a | b |")
                     .nodes;
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].type, NodeType::Table);
    const auto html = ExtractSelectedTextAsHtml(nodes, MakeTableFullSelection(nodes[0]));
    EXPECT_NE(html.find("<thead>"), std::string::npos);
    EXPECT_EQ(CountOccurrences(html, "<tr>"), 2u);
}

// 列数 0 の表 (md4c が TABLE detail を渡さない場合) でも範囲外を読まない。
TEST(ExtractSelectedTextAsHtml, TableWithoutColumnsDoesNotReadOutOfRange)
{
    std::pmr::vector<Node> nodes(1);
    nodes[0].type = NodeType::Table;
    auto* tbl = nodes[0].ensure_table();
    tbl->row_count = 1;
    tbl->col_count = 0;
    tbl->cell_text_starts.push_back(0);
    tbl->cell_run_starts.push_back(0);
    tbl->is_header_row.push_back(false);
    const auto html = ExtractSelectedTextAsHtml(nodes, TextSelection::MakeOrdered(0, 0, 0, 1));
    EXPECT_EQ(html.find("<table"), std::string::npos);
}

TEST(ExtractSelectedTextAsHtml, TableDarkModeUsesDarkBorder)
{
    auto nodes = ParseMarkdown(
                     "| A | B |\n"
                     "|---|---|\n"
                     "| 1 | 2 |")
                     .nodes;
    ASSERT_EQ(nodes[0].type, NodeType::Table);
    auto sel = MakeTableFullSelection(nodes[0]);
    auto html = ExtractSelectedTextAsHtml(nodes, sel, /*dark_mode=*/true);
    EXPECT_NE(html.find("border:1px solid #3c3c3c"), std::string::npos);
    EXPECT_EQ(html.find("#d0d7de"), std::string::npos);
}

TEST(ExtractSelectedTextAsHtml, TableWithoutDataFallsBackToPre)
{
    // table_data が空のノードに対しては <pre> フォールバックで出力される。
    Node n;
    n.type = NodeType::Table;
    n.SetTextWithLineCount(std::string_view{ "fallback" }, 0);
    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(n));
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_EQ(html.find("<table"), std::string::npos);
    EXPECT_NE(html.find("<pre>fallback</pre>"), std::string::npos);
}

TEST(ExtractSelectedTextAsHtml, ImageRendersAsImgTag)
{
    auto nodes = ParseMarkdown("![alt text](https://example.com/img.png)").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].type, NodeType::Image);
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find("<img src=\"https://example.com/img.png\""), std::string::npos);
    EXPECT_NE(html.find("alt=\"alt text\""), std::string::npos);
    EXPECT_EQ(html.find("<pre>"), std::string::npos);
}

TEST(ExtractSelectedTextAsHtml, ImageEscapesAttributesSafely)
{
    // alt と src の両方で & " がエスケープされること。
    auto nodes = ParseMarkdown("![Q&A \"x\"](path?a=1&b=2)").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].type, NodeType::Image);
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find("src=\"path?a=1&amp;b=2\""), std::string::npos);
    EXPECT_NE(html.find("Q&amp;A"), std::string::npos);
    EXPECT_NE(html.find("&quot;x&quot;"), std::string::npos);
}

TEST(ExtractSelectedTextAsHtml, UnorderedListWrapsInUl)
{
    auto nodes = ParseMarkdown("- one\n- two").nodes;
    ASSERT_GE(nodes.size(), 2u);
    auto sel = TextSelection::MakeOrdered(0, 0, 1, static_cast<uint32_t>(nodes[1].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find("<ul>"), std::string::npos);
    EXPECT_NE(html.find("<li>one</li>"), std::string::npos);
    EXPECT_NE(html.find("<li>two</li>"), std::string::npos);
    EXPECT_NE(html.find("</ul>"), std::string::npos);
}

TEST(ExtractSelectedTextAsHtml, OrderedListWrapsInOl)
{
    auto nodes = ParseMarkdown("1. first\n2. second").nodes;
    ASSERT_GE(nodes.size(), 2u);
    auto sel = TextSelection::MakeOrdered(0, 0, 1, static_cast<uint32_t>(nodes[1].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find("<ol>"), std::string::npos);
    EXPECT_NE(html.find("<li>first</li>"), std::string::npos);
    EXPECT_NE(html.find("<li>second</li>"), std::string::npos);
    EXPECT_NE(html.find("</ol>"), std::string::npos);
}

TEST(ExtractSelectedTextAsHtml, BlockQuote)
{
    auto nodes = ParseMarkdown("> quoted text").nodes;
    ASSERT_EQ(nodes[0].type, NodeType::BlockQuote);
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find("<blockquote>"), std::string::npos);
    EXPECT_NE(html.find("</blockquote>"), std::string::npos);
    EXPECT_NE(html.find("quoted text"), std::string::npos);
}

TEST(ExtractSelectedTextAsHtml, HorizontalRule)
{
    auto nodes = ParseMarkdown("before\n\n---\n\nafter").nodes;
    ASSERT_GE(nodes.size(), 3u);
    ASSERT_EQ(nodes[1].type, NodeType::HorizontalRule);
    auto sel = TextSelection::MakeOrdered(0, 0, 2, static_cast<uint32_t>(nodes[2].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find("<hr>"), std::string::npos);
}

TEST(ExtractSelectedTextAsHtml, MultiParagraph)
{
    auto nodes = ParseMarkdown("First\n\nSecond").nodes;
    ASSERT_EQ(nodes.size(), 2u);
    auto sel = TextSelection::MakeOrdered(0, 0, 1, static_cast<uint32_t>(nodes[1].GetText().size()));
    EXPECT_EQ(ExtractSelectedTextAsHtml(nodes, sel),
              "<p>First</p><p>Second</p>");
}

TEST(ExtractSelectedTextAsHtml, PartialSelectionInParagraph)
{
    auto nodes = ParseMarkdown("Hello world").nodes;
    auto sel = TextSelection::MakeOrdered(0, 6, 0, 11);
    EXPECT_EQ(ExtractSelectedTextAsHtml(nodes, sel), "<p>world</p>");
}

TEST(ExtractSelectedTextAsHtml, OrderedTaskListWrapsInOl)
{
    auto nodes = ParseMarkdown("1. [ ] first\n2. [x] second").nodes;
    ASSERT_GE(nodes.size(), 2u);
    ASSERT_EQ(nodes[0].type, NodeType::TaskListItem);
    ASSERT_EQ(nodes[1].type, NodeType::TaskListItem);
    auto sel = TextSelection::MakeOrdered(0, 0, 1, static_cast<uint32_t>(nodes[1].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find("<ol>"), std::string::npos);
    EXPECT_NE(html.find("</ol>"), std::string::npos);
    EXPECT_EQ(html.find("<ul>"), std::string::npos);
}

TEST(ExtractSelectedTextAsHtml, UnsafeSchemeLinkIsStripped)
{
    auto nodes = ParseMarkdown("[click](javascript:alert(1))").nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_EQ(html.find("<a href="), std::string::npos);
    EXPECT_EQ(html.find("javascript"), std::string::npos);
    EXPECT_NE(html.find("click"), std::string::npos);
}

TEST(ExtractSelectedTextAsHtml, FileSchemeLinkIsStripped)
{
    auto nodes = ParseMarkdown("[open](file:///C:/secret.txt)").nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_EQ(html.find("<a href="), std::string::npos);
    EXPECT_NE(html.find("open"), std::string::npos);
}

TEST(ExtractSelectedTextAsHtml, MailtoLinkIsKept)
{
    auto nodes = ParseMarkdown("[mail](mailto:user@example.com)").nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find("<a href=\"mailto:user@example.com\">mail</a>"), std::string::npos);
}

TEST(ExtractSelectedTextAsHtml, InternalAnchorLinkIsStripped)
{
    auto nodes = ParseMarkdown("[sec](#section)").nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_EQ(html.find("<a href="), std::string::npos);
    EXPECT_NE(html.find("sec"), std::string::npos);
}

// ============================================================
// FindLinkAtPosition
// ============================================================

TEST(FindLinkAtPosition, NoLinks)
{
    auto nodes = ParseMarkdown("plain text").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    auto result = FindLinkAtPosition(nodes[0], 0);
    EXPECT_FALSE(result.has_value());
}

TEST(FindLinkAtPosition, LinkFound)
{
    auto nodes = ParseMarkdown("[click](https://example.com)").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    // リンクテキスト内の位置
    auto result = FindLinkAtPosition(nodes[0], 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "https://example.com");
}

TEST(FindLinkAtPosition, PositionOutsideLink)
{
    auto nodes = ParseMarkdown("before [link](https://example.com) after").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    // "before"テキスト内の位置（リンクではないはず）
    auto result = FindLinkAtPosition(nodes[0], 0);
    EXPECT_FALSE(result.has_value());
}

TEST(FindLinkAtPosition, InternalLink)
{
    auto nodes = ParseMarkdown("[section](#my-section)").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    auto result = FindLinkAtPosition(nodes[0], 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "#my-section");
}

TEST(FindLinkAtPosition, PositionAtLinkBoundary)
{
    auto nodes = ParseMarkdown("[link](https://example.com)").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    // リンクの最後の文字の位置
    uint32_t last_pos = static_cast<uint32_t>(nodes[0].GetText().size()) - 1;
    auto result = FindLinkAtPosition(nodes[0], last_pos);
    ASSERT_TRUE(result.has_value());
}

TEST(FindLinkAtPosition, PositionBeyondText)
{
    auto nodes = ParseMarkdown("[link](https://example.com)").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    auto result = FindLinkAtPosition(nodes[0], 9999);
    EXPECT_FALSE(result.has_value());
}

TEST(FindLinkAtPosition, EmptyNode)
{
    Node node;
    auto result = FindLinkAtPosition(node, 0);
    EXPECT_FALSE(result.has_value());
}

// ============================================================
// FindAnchorNodeIndex
// ============================================================

TEST(FindAnchorNodeIndex, EmptyNodes)
{
    std::pmr::vector<Node> nodes;
    EXPECT_EQ(FindAnchorNodeIndexLinear(nodes, "test"), -1);
}

TEST(FindAnchorNodeIndex, EmptyAnchor)
{
    auto nodes = ParseMarkdown("# Title").nodes;
    EXPECT_EQ(FindAnchorNodeIndexLinear(nodes, ""), -1);
}

TEST(FindAnchorNodeIndex, FindExistingAnchor)
{
    auto nodes = ParseMarkdown("# Title\n\nParagraph\n\n## Section").nodes;
    ASSERT_GE(nodes.size(), 3u);
    int idx = FindAnchorNodeIndexLinear(nodes, "title");
    EXPECT_EQ(idx, 0);
}

TEST(FindAnchorNodeIndex, FindSecondHeading)
{
    auto nodes = ParseMarkdown("# First\n\nParagraph\n\n## Second").nodes;
    int idx = FindAnchorNodeIndexLinear(nodes, "second");
    EXPECT_GE(idx, 0);
    EXPECT_EQ(nodes[idx].GetText(), "Second");
}

TEST(FindAnchorNodeIndex, CaseInsensitiveSearch)
{
    auto nodes = ParseMarkdown("# Hello World").nodes;
    // アンカーは"hello-world"、大文字で検索
    int idx = FindAnchorNodeIndexLinear(nodes, "Hello-World");
    EXPECT_EQ(idx, 0);
}

TEST(FindAnchorNodeIndex, NotFound)
{
    auto nodes = ParseMarkdown("# Title").nodes;
    EXPECT_EQ(FindAnchorNodeIndexLinear(nodes, "nonexistent"), -1);
}

TEST(FindAnchorNodeIndex, CjkAnchor)
{
    auto nodes = ParseMarkdown("## コードブロック").nodes;
    int idx = FindAnchorNodeIndexLinear(nodes, "コードブロック");
    EXPECT_EQ(idx, 0);
}

TEST(FindAnchorNodeIndex, SkipsNonHeadings)
{
    auto nodes = ParseMarkdown("Paragraph\n\n# Heading").nodes;
    int idx = FindAnchorNodeIndexLinear(nodes, "heading");
    EXPECT_GE(idx, 0);
    EXPECT_EQ(nodes[idx].type, NodeType::Heading);
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
        { "HanSurrogatePair", L"𠮷田", 0, true, 0, 3 },
        // low surrogate を指しても high surrogate にスナップする
        { "PosOnLowSurrogate", L"𠮷田", 1, true, 0, 3 },
    };
    ExpectWordBoundaries(kCases);
}

// ============================================================
// ExtractFilename
// ============================================================

TEST(ExtractFilename, ReturnsLastPathComponent)
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
        EXPECT_EQ(ExtractFilename(c.path), c.expected);
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
        EXPECT_EQ(BuildTitleString(c.path), c.expected);
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
    table_node.SetTextWithLineCount(std::string_view{ "A\tB\n1\t2" }, 1);

    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(table_node));
    TextSelection sel;
    sel.start_node = 0;
    sel.start_pos = 0;
    sel.end_node = 0;
    sel.end_pos = 3;
    sel.active = true;

    auto result = ExtractSelectedText(nodes, sel);
    EXPECT_EQ(result, "A\tB");
}

// issue #200: parser 経由で生成されたテーブル（owned_text_ が空で table_data->concat_text に
// 線形化テキストが入る）に対し、selection が concat_text 内 offset を指しているとき、
// ExtractSelectedText がセル内テキストを返すこと。
TEST(ExtractSelectedText, ParsedTableCellWordSelection)
{
    auto nodes = ParseMarkdown(
                     "| Name | Value |\n"
                     "|------|-------|\n"
                     "| foo  | bar   |")
                     .nodes;
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
    auto nodes = ParseMarkdown(
                     "| A | B |\n"
                     "|---|---|\n"
                     "| 1 | 2 |")
                     .nodes;
    ASSERT_EQ(nodes[0].type, NodeType::Table);
    const auto* tbl = nodes[0].table_data();
    ASSERT_NE(tbl, nullptr);

    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(tbl->concat_text.size()));
    auto result = ExtractSelectedText(nodes, sel);
    EXPECT_FALSE(result.empty());
    // セル区切り '\t' / 行区切り '\n' を含み、各セルテキストが含まれること。
    EXPECT_NE(result.find('\t'), std::string::npos);
    EXPECT_NE(result.find('\n'), std::string::npos);
    EXPECT_NE(result.find("A"), std::string::npos);
    EXPECT_NE(result.find("B"), std::string::npos);
    EXPECT_NE(result.find("1"), std::string::npos);
    EXPECT_NE(result.find("2"), std::string::npos);
}

TEST(ExtractSelectedText, StartNodeOutOfRange)
{
    std::pmr::vector<Node> nodes;
    Node n;
    n.SetTextWithLineCount(std::string_view{ "hello" }, 0);
    nodes.emplace_back(std::move(n));

    TextSelection sel;
    sel.start_node = -5;
    sel.start_pos = 0;
    sel.end_node = 0;
    sel.end_pos = 5;
    sel.active = true;

    // クラッシュせず、無効なノードをスキップするべき
    auto result = ExtractSelectedText(nodes, sel);
    EXPECT_FALSE(result.empty());
}

TEST(ExtractSelectedText, EndNodeOutOfRange)
{
    std::pmr::vector<Node> nodes;
    Node n;
    n.SetTextWithLineCount(std::string_view{ "hello" }, 0);
    nodes.emplace_back(std::move(n));

    TextSelection sel;
    sel.start_node = 0;
    sel.start_pos = 0;
    sel.end_node = 100;
    sel.end_pos = 5;
    sel.active = true;

    auto result = ExtractSelectedText(nodes, sel);
    // 少なくとも最初のノードのテキストが含まれるべき
    EXPECT_FALSE(result.empty());
}

// ---- FindLinkAtPosition 追加テスト ----

TEST(FindLinkAtPosition, MultipleLinkRuns)
{
    Node node;
    node.SetTextWithLineCount(std::string_view{ "link1 link2" }, 0);

    node.ensure_link_urls().emplace_back("https://a.com");
    node.ensure_link_urls().emplace_back("https://b.com");

    TextRun r1;
    r1.start = 0;
    r1.length = 5;
    r1.link_url_index = 0;

    TextRun r2;
    r2.start = 6;
    r2.length = 5;
    r2.link_url_index = 1;

    node.runs = { r1, r2 };

    auto result1 = FindLinkAtPosition(node, 2);
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(*result1, "https://a.com");

    auto result2 = FindLinkAtPosition(node, 8);
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(*result2, "https://b.com");

    // 2つのリンクの間
    auto gap = FindLinkAtPosition(node, 5);
    EXPECT_FALSE(gap.has_value());
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
    node.ensure_table();
    auto* tbl = node.table_data();

    TextRun h0r;
    h0r.start = 0;
    h0r.length = 4;
    TextRun h1r;
    h1r.start = 0;
    h1r.length = 3;
    TextRun d0r;
    d0r.start = 0;
    d0r.length = 3;
    TextRun d1r;
    d1r.start = 0;
    d1r.length = 3;
    d1r.link_url_index = static_cast<int16_t>(node.view_link_urls().size());
    node.ensure_link_urls().emplace_back("https://example.com");

    // concat 化: "Name\tURL\nfoo\tbar", cell_text_starts: [0, 5, 9, 13, 16]
    tbl->row_count = 2;
    tbl->col_count = 2;
    tbl->concat_text = "Name\tURL\nfoo\tbar";
    tbl->cell_text_starts = { 0u, 5u, 9u, 13u, 16u };
    tbl->all_runs.push_back(h0r);
    tbl->all_runs.push_back(h1r);
    tbl->all_runs.push_back(d0r);
    tbl->all_runs.push_back(d1r);
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
    auto nodes = ParseMarkdown(
                     "| Text | Link |\n"
                     "|------|------|\n"
                     "| hello | [click](https://example.com) |")
                     .nodes;
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
    node.ensure_table();
    auto* tbl = node.table_data();

    TextRun r;
    r.start = 0;
    r.length = 7;
    r.link_url_index = static_cast<int16_t>(node.view_link_urls().size());
    node.ensure_link_urls().emplace_back("#my-section");

    // concat 化: 1 行 1 列の "section"
    tbl->row_count = 1;
    tbl->col_count = 1;
    tbl->concat_text = "section";
    tbl->cell_text_starts = { 0u, 7u };
    tbl->all_runs.push_back(r);
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
    node.ensure_table();
    auto* tbl = node.table_data();

    TextRun r0;
    r0.start = 0;
    r0.length = 1;
    TextRun r1;
    r1.start = 0;
    r1.length = 1;
    r1.link_url_index = static_cast<int16_t>(node.view_link_urls().size());
    node.ensure_link_urls().emplace_back("https://b.com");

    // concat 化: 1 行 2 列の "A\tB"
    tbl->row_count = 1;
    tbl->col_count = 2;
    tbl->concat_text = "A\tB";
    tbl->cell_text_starts = { 0u, 2u, 3u };
    tbl->all_runs.push_back(r0);
    tbl->all_runs.push_back(r1);
    tbl->cell_run_starts = { 0u, 1u, 2u };
    tbl->aligns = { TableAlign::Default, TableAlign::Default };
    tbl->is_header_row = { false };

    // タブ区切り（オフセット1）はどのセルにもマッチしないべき
    auto result = FindLinkAtPosition(node, 1);
    EXPECT_FALSE(result.has_value());

    // "B"（オフセット2）でリンクが見つかるべき
    auto link = FindLinkAtPosition(node, 2);
    ASSERT_TRUE(link.has_value());
    EXPECT_EQ(*link, "https://b.com");
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
    node.ensure_table();
    auto* tbl = node.table_data();
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

// ---- FindAnchorNodeIndex 追加テスト ----

TEST(FindAnchorNodeIndex, DuplicateAnchors)
{
    std::pmr::vector<Node> nodes;

    Node h1;
    h1.type = NodeType::Heading;
    h1.ensure_anchor_id_mut() = "title";
    nodes.emplace_back(std::move(h1));

    Node h2;
    h2.type = NodeType::Heading;
    h2.ensure_anchor_id_mut() = "title-1";
    nodes.emplace_back(std::move(h2));

    // 最初のマッチが優先される
    EXPECT_EQ(FindAnchorNodeIndexLinear(nodes, "title"), 0);
    EXPECT_EQ(FindAnchorNodeIndexLinear(nodes, "title-1"), 1);
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
        // 末尾追記はスクロールを維持する prefix-only growth
        { "abc", "abcdef", ReloadOp::PrefixGrowth, 3 },
        { "", "new content", ReloadOp::PrefixGrowth, 0 },
        { "あいう", "あいうえお", ReloadOp::PrefixGrowth, 9 },
        // truncate→rewrite の前半の可能性があるため defer
        { "abcdef", "abc", ReloadOp::DeferPrefixShrink, 3 },
        { "old content", "", ReloadOp::DeferPrefixShrink, 0 },
        { "abcdef", "abcXef", ReloadOp::FullReload, 3 },
        { "abc", "Xbc", ReloadOp::FullReload, 0 },
        { "abcdef", "abcYYYz", ReloadOp::FullReload, 3 },
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
    EXPECT_EQ(FindNodeBySourceOffset(nodes, SourceOffsetTestBase(), 0), -1);
}

TEST(FindNodeBySourceOffset, SingleNode)
{
    std::pmr::vector<Node> nodes(1);
    nodes[0].SetSourceOffset(SourceOffsetTestBase(), 0);
    EXPECT_EQ(FindNodeBySourceOffset(nodes, SourceOffsetTestBase(), 0), 0);
    EXPECT_EQ(FindNodeBySourceOffset(nodes, SourceOffsetTestBase(), 100), 0);
}

TEST(FindNodeBySourceOffset, OffsetBeforeAllNodes)
{
    std::pmr::vector<Node> nodes(2);
    nodes[0].SetSourceOffset(SourceOffsetTestBase(), 10);
    nodes[1].SetSourceOffset(SourceOffsetTestBase(), 20);
    // diff_offset=5 は最初のノード(offset=10)よりも前 → 該当なし
    EXPECT_EQ(FindNodeBySourceOffset(nodes, SourceOffsetTestBase(), 5), -1);
}

TEST(FindNodeBySourceOffset, ExactMatch)
{
    std::pmr::vector<Node> nodes(3);
    nodes[0].SetSourceOffset(SourceOffsetTestBase(), 0);
    nodes[1].SetSourceOffset(SourceOffsetTestBase(), 10);
    nodes[2].SetSourceOffset(SourceOffsetTestBase(), 25);
    EXPECT_EQ(FindNodeBySourceOffset(nodes, SourceOffsetTestBase(), 10), 1);
    EXPECT_EQ(FindNodeBySourceOffset(nodes, SourceOffsetTestBase(), 25), 2);
}

TEST(FindNodeBySourceOffset, BetweenNodes)
{
    std::pmr::vector<Node> nodes(3);
    nodes[0].SetSourceOffset(SourceOffsetTestBase(), 0);
    nodes[1].SetSourceOffset(SourceOffsetTestBase(), 10);
    nodes[2].SetSourceOffset(SourceOffsetTestBase(), 25);
    // offset=15 はノード1(10)とノード2(25)の間 → ノード1を返す
    EXPECT_EQ(FindNodeBySourceOffset(nodes, SourceOffsetTestBase(), 15), 1);
}

TEST(FindNodeBySourceOffset, SkipsUnsetOffsets)
{
    std::pmr::vector<Node> nodes(3);
    nodes[0].SetSourceOffset(SourceOffsetTestBase(), 0);
    // nodes[1] は SetSourceOffset を呼ばないため view_.data() == nullptr (= 未設定)
    nodes[2].SetSourceOffset(SourceOffsetTestBase(), 20);
    // 未設定値は常に diff_offset 以上にならない（kUnsetSourceOffset <= diff_offset は通常 false）
    // → ノード0を返す
    EXPECT_EQ(FindNodeBySourceOffset(nodes, SourceOffsetTestBase(), 10), 0);
    EXPECT_EQ(FindNodeBySourceOffset(nodes, SourceOffsetTestBase(), 20), 2);
}

TEST(FindNodeBySourceOffset, ParsedMarkdown)
{
    std::string_view md = "# Title\n\nParagraph\n\n## Section";
    auto nodes = ParseMarkdown(md).nodes;
    ASSERT_GE(nodes.size(), 3u);
    // 各ノードが有効な source_offset を持つ
    for (const auto& n : nodes) {
        EXPECT_NE(n.SourceOffsetFrom(md.data()), kUnsetSourceOffset);
    }
    // 最初のノードの offset は "# " の後 = 2
    EXPECT_EQ(nodes[0].SourceOffsetFrom(md.data()), 2u);
    // "Paragraph" は "# Title\n\n" = 9バイト目から
    EXPECT_EQ(nodes[1].SourceOffsetFrom(md.data()), 9u);
}

TEST(FindNodeBySourceOffset, LastNodeForLargeOffset)
{
    std::pmr::vector<Node> nodes(3);
    nodes[0].SetSourceOffset(SourceOffsetTestBase(), 0);
    nodes[1].SetSourceOffset(SourceOffsetTestBase(), 100);
    nodes[2].SetSourceOffset(SourceOffsetTestBase(), 200);
    // ソース末尾を超えるオフセット → 最後のノードを返す
    EXPECT_EQ(FindNodeBySourceOffset(nodes, SourceOffsetTestBase(), 999), 2);
}

TEST(FindNodeBySourceOffset, AllUnsetOffsets)
{
    // 全ノードが未設定（テキストなし）→ 該当なし
    std::pmr::vector<Node> nodes(3);
    // 何も設定しないため view_.data() == nullptr (= 未設定)
    EXPECT_EQ(FindNodeBySourceOffset(nodes, SourceOffsetTestBase(), 50), -1);
}

TEST(FindNodeBySourceOffset, MixedWithHorizontalRules)
{
    // パース結果で HorizontalRule が混在するケース
    std::string_view md = "AAA\n\n---\n\nBBB";
    auto nodes = ParseMarkdown(md).nodes;
    ASSERT_GE(nodes.size(), 3u);
    // "AAA" offset=0, "---" は未設定, "BBB" offset=10
    EXPECT_EQ(nodes[0].SourceOffsetFrom(md.data()), 0u);
    EXPECT_EQ(nodes[1].SourceOffsetFrom(md.data()), kUnsetSourceOffset);

    // offset=5（"---"のソース位置付近）→ AAA(offset=0)を返す（HRはスキップ）
    EXPECT_EQ(FindNodeBySourceOffset(nodes, md.data(), 5), 0);
    // offset=10（BBBのソース位置）→ BBBを返す
    int bbb_idx = FindNodeBySourceOffset(nodes, md.data(), 10);
    EXPECT_EQ(bbb_idx, 2);
}

// ============================================================
// 統合テスト: diff検出 → ノード特定
// ============================================================

// ヘルパー: old→new の編集をシミュレートし、変更箇所のノードを特定する。
// ParseMarkdown は引数 string_view の data() を view_ ベースとして埋め込むため、
// FindNodeBySourceOffset にも同じ new_md.data() を base として渡す。
static int SimulateEditAndFindNode(std::string_view old_md, std::string_view new_md)
{
    size_t diff_pos = FindFirstDifference(old_md, new_md);
    if (diff_pos == std::string_view::npos) {
        return -1;
    }
    auto nodes = ParseMarkdown(new_md).nodes;
    if (nodes.empty()) {
        return -1;
    }
    return FindNodeBySourceOffset(nodes, new_md.data(), diff_pos);
}

TEST(DiffToNode, EditMiddleParagraph)
{
    // 2番目の段落を編集
    std::string old_md = "First\n\nSecond\n\nThird";
    std::string new_md = "First\n\nModified\n\nThird";
    int node = SimulateEditAndFindNode(old_md, new_md);
    auto nodes = ParseMarkdown(new_md).nodes;
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
    auto nodes = ParseMarkdown(new_md).nodes;
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
    auto nodes = ParseMarkdown(new_md).nodes;
    EXPECT_LE(node, 1); // "First" or "Last"
}

TEST(DiffToNode, AppendToEnd)
{
    std::string old_md = "Existing";
    std::string new_md = "Existing\n\nAppended";
    int node = SimulateEditAndFindNode(old_md, new_md);
    auto nodes = ParseMarkdown(new_md).nodes;
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
    auto nodes = ParseMarkdown(new_md).nodes;
    ASSERT_GE(node, 0);
    EXPECT_EQ(nodes[node].type, NodeType::CodeBlock);
}

TEST(DiffToNode, EditInListItem)
{
    // リストアイテムの編集
    std::string old_md = "- first\n- second\n- third";
    std::string new_md = "- first\n- changed\n- third";
    int node = SimulateEditAndFindNode(old_md, new_md);
    auto nodes = ParseMarkdown(new_md).nodes;
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
    auto nodes = ParseMarkdown(new_md).nodes;
    ASSERT_GE(node, 0);
    EXPECT_EQ(nodes[node].type, NodeType::BlockQuote);
}

TEST(DiffToNode, EditWithJapanese)
{
    // 日本語テキストの編集
    std::string old_md = "# はじめに\n\n旧テキスト\n\nおわり";
    std::string new_md = "# はじめに\n\n新テキスト\n\nおわり";
    int node = SimulateEditAndFindNode(old_md, new_md);
    auto nodes = ParseMarkdown(new_md).nodes;
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
    auto nodes = ParseMarkdown(new_md).nodes;
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
    auto nodes = ParseMarkdown(new_md).nodes;
    ASSERT_GE(node, 0);
    EXPECT_EQ(nodes[node].GetText(), "CHANGED paragraph 50");
}

// ============================================================
// IsPrefixOnlyDiff
// ============================================================

// diff_pos が短い方の長さと一致するときだけ true
TEST(IsPrefixOnlyDiff, TrueWhenDiffAtShorterEnd)
{
    struct Case {
        size_t diff_pos;
        size_t old_size;
        size_t new_size;
        bool expected;
    };
    constexpr Case kCases[] = {
        { 5, 10, 10, false },
        { 3, 3, 6, true },
        { 3, 6, 3, true },
        { 2, 6, 3, false },
        { 0, 0, 3, true },
        { 0, 3, 0, true },
        // 通常 FindFirstDifference は npos を返すのでここには到達しない
        { 0, 0, 0, true },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::Message() << "diff_pos=" << c.diff_pos << " old=" << c.old_size << " new=" << c.new_size);
        EXPECT_EQ(IsPrefixOnlyDiff(c.diff_pos, c.old_size, c.new_size), c.expected);
    }
}

TEST(IsPrefixOnlyDiff, IntegrationWithFindFirstDifference)
{
    // FindFirstDifference と組み合わせた実際のユースケース
    std::string_view old_text = "Hello World";
    std::string_view new_text = "Hello World, more text";
    size_t diff = FindFirstDifference(old_text, new_text);
    EXPECT_TRUE(IsPrefixOnlyDiff(diff, old_text.size(), new_text.size()));

    // 内容が異なる場合
    std::string_view old_text2 = "Hello World";
    std::string_view new_text2 = "Hello Xxxxx";
    size_t diff2 = FindFirstDifference(old_text2, new_text2);
    EXPECT_FALSE(IsPrefixOnlyDiff(diff2, old_text2.size(), new_text2.size()));
}

// ============================================================
// CalcScrollYForDiff
// ============================================================

// ヘルパー: source_offset を等間隔に設定したノード列を構築する。
// CalcScrollYForDiff は content.data() を base に source_offset を取り出すため、
// テスト側でも同じバッファ (base) を MakeNodes に渡す必要がある。
static std::pmr::vector<Node> MakeNodes(const char* base, int count, size_t offset_step = 100)
{
    std::pmr::vector<Node> nodes(count);
    for (int i = 0; i < count; ++i) {
        nodes[i].SetSourceOffset(base, i * offset_step);
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
    auto nodes = MakeNodes(content.data(), 3, 100);
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
    auto nodes = MakeNodes(content.data(), 3, 100);
    nodes[0].SetSourceOffset(content.data(), 50);
    auto cache = MakeUniformCache(3);
    // diff_pos=10 < 全ノードの最小 offset(50) → -1 → fallback
    EXPECT_FLOAT_EQ(CalcScrollYForDiff(nodes, cache, content, 10, 500.0f, 99.0f), 99.0f);
}

TEST(CalcScrollYForDiff, ScrollsToNodeStartWithMargin)
{
    // 3ノード: offset=0,100,200 / y=0,100,200 / height=100
    std::string content(300, 'x');
    auto nodes = MakeNodes(content.data(), 3, 100);
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
    auto nodes = MakeNodes(content.data(), 2, 100);
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
    auto nodes = MakeNodes(content.data(), 2, 100);
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
    nodes[0].SetSourceOffset(content.data(), 0);
    // nodes[1] は未設定（HorizontalRule等）
    nodes[2].SetSourceOffset(content.data(), 200);
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
    auto nodes = MakeNodes(content.data(), 1, 0);
    nodes[0].SetSourceOffset(content.data(), 0);
    auto cache = MakeUniformCache(1, 1000.0f);

    // diff_pos=50, next_start=content.size()=100
    // fraction = 50/100 = 0.5
    // node_y = 0 + 1000*0.5 = 500
    // margin = 200*0.2 = 40
    // result = max(0, 500-40) = 460
    EXPECT_FLOAT_EQ(CalcScrollYForDiff(nodes, cache, content, 50, 200.0f, 0.0f), 460.0f);
}

TEST(CalcScrollYForDiff, CacheSizeMismatchFallback)
{
    // ノード数とキャッシュサイズが不一致の場合のフォールバック
    const std::string content(500, 'x');
    auto nodes = MakeNodes(content.data(), 5, 100);
    auto cache = MakeUniformCache(3, 100.0f); // キャッシュは3つだけ

    // diff_pos=400 → node 4 だがキャッシュは3つ → fallback
    EXPECT_FLOAT_EQ(CalcScrollYForDiff(nodes, cache, content, 400, 500.0f, 77.0f), 77.0f);
}

TEST(CalcScrollYForDiff, ParsedMarkdownIntegration)
{
    // 実際のMarkdownをパースして差分スクロール位置を計算する統合テスト
    std::string md = "# Title\n\nFirst paragraph\n\nSecond paragraph\n\nThird paragraph";
    auto nodes = ParseMarkdown(std::string_view{ md }).nodes;
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
// 末尾追記 (PrefixGrowth) で旧コードが old_scroll を保持していたため、
// ユーザの編集場所にスクロールしなかった。AnalyzeReloadDiff と
// CalcScrollYForDiff の組合せで「fallback ではなく diff_pos に対応する
// 位置を返す」ことを担保する。
TEST(CalcScrollYForDiff, PrefixGrowthScrollsTowardAppendedTail)
{
    // 旧 doc: 3 段落
    const std::string old_md = "para_one\n\npara_two\n\npara_three";
    // 新 doc: 末尾に 1 段落追記
    const std::string new_md = old_md + "\n\npara_four_added";

    // AnalyzeReloadDiff で PrefixGrowth と判定されること
    const auto decision = AnalyzeReloadDiff(old_md, new_md);
    ASSERT_EQ(decision.op, ReloadOp::PrefixGrowth);
    ASSERT_EQ(decision.diff_pos, old_md.size());

    // 新 doc を pipeline で扱うイメージで cache を構築
    auto nodes = ParseMarkdown(std::string_view{ new_md }).nodes;
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
// IsHelpPath (残りのケースは test_help.cpp)
// ============================================================

// ============================================================
// AppendInlineHtml の LF バッチ化境界ケース
// ============================================================

// Node を手動構築して <br> の出力同一性を検証するヘルパー。
static std::pmr::string HtmlFromNodeText(std::string_view text_with_lf)
{
    Node n;
    n.type = NodeType::Paragraph;
    const auto lf_count = static_cast<int32_t>(
        std::ranges::count(text_with_lf, '\n'));
    n.SetTextWithLineCount(text_with_lf, lf_count);
    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(n));
    auto sel = TextSelection::MakeOrdered(
        0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    return ExtractSelectedTextAsHtml(nodes, sel);
}

// LF が1個: <br>が1つ出力されること。
TEST(ExtractSelectedTextAsHtml, SingleLfProducesBr)
{
    auto html = HtmlFromNodeText("line1\nline2");
    EXPECT_NE(html.find("<br>"), std::string::npos);
    EXPECT_NE(html.find("line1"), std::string::npos);
    EXPECT_NE(html.find("line2"), std::string::npos);
    // <br> が 1 個だけであること
    const auto pos1 = html.find("<br>");
    ASSERT_NE(pos1, std::string::npos);
    EXPECT_EQ(html.find("<br>", pos1 + 1), std::string::npos);
}

// 先頭 LF: <br> が先頭に出て後続テキストが続く。
TEST(ExtractSelectedTextAsHtml, LeadingLfProducesBrFirst)
{
    auto html = HtmlFromNodeText("\nafter");
    EXPECT_NE(html.find("<br>"), std::string::npos);
    EXPECT_NE(html.find("after"), std::string::npos);
    // <br> が "after" より前にある。
    EXPECT_LT(html.find("<br>"), html.find("after"));
}

// 末尾 LF: テキスト後に <br> が出力される。
TEST(ExtractSelectedTextAsHtml, TrailingLfProducesBrLast)
{
    auto html = HtmlFromNodeText("before\n");
    EXPECT_NE(html.find("<br>"), std::string::npos);
    EXPECT_NE(html.find("before"), std::string::npos);
    EXPECT_GT(html.find("<br>"), html.find("before"));
}

// 連続 LF: LF の数だけ <br> が出力される。
TEST(ExtractSelectedTextAsHtml, ConsecutiveLfsProduceMultipleBrs)
{
    auto html = HtmlFromNodeText("a\n\nb");
    // <br> が2個。
    const auto pos1 = html.find("<br>");
    ASSERT_NE(pos1, std::string::npos);
    const auto pos2 = html.find("<br>", pos1 + 1);
    EXPECT_NE(pos2, std::string::npos);
    // 3個目はない。
    EXPECT_EQ(html.find("<br>", pos2 + 1), std::string::npos);
}

// テキストが LF のみ: <br> だけが出力される。
TEST(ExtractSelectedTextAsHtml, OnlyLf)
{
    auto html = HtmlFromNodeText("\n");
    const auto p_pos = html.find("<p>");
    ASSERT_NE(p_pos, std::string::npos);
    // テキストコンテンツなし: <p> の直後に <br> が来る。
    EXPECT_EQ(html.find("<br>"), p_pos + 3);
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
// BuildCodeBlockHtmlFragment
// ============================================================

TEST(BuildCodeBlockHtmlFragment, NonCodeBlockReturnsEmpty)
{
    const auto nodes = ParseMarkdown("plain paragraph").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_TRUE(BuildCodeBlockHtmlFragment(nodes[0], false).empty());
}

TEST(BuildCodeBlockHtmlFragment, WrapsEscapedCodeInPre)
{
    const auto nodes = ParseMarkdown("```\na < b && c > d\n```").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    const auto html = BuildCodeBlockHtmlFragment(nodes[0], false);
    EXPECT_TRUE(html.starts_with("<pre "));
    EXPECT_TRUE(html.ends_with("</code></pre>"));
    EXPECT_NE(html.find("a &lt; b &amp;&amp; c &gt; d"), std::string::npos);
    EXPECT_EQ(html.find("a < b"), std::string::npos);
}

// ダーク時は貼り付け先の既定 (黒文字) で読めなくならないよう文字色を明示する。
TEST(BuildCodeBlockHtmlFragment, DarkModeSpecifiesTextColor)
{
    const auto nodes = ParseMarkdown("```\ncode\n```").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    const auto light = BuildCodeBlockHtmlFragment(nodes[0], false);
    const auto dark = BuildCodeBlockHtmlFragment(nodes[0], true);
    EXPECT_EQ(light.find(";color:"), std::string::npos);
    EXPECT_NE(dark.find(";color:"), std::string::npos);
}
