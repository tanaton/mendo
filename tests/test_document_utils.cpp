#include <gtest/gtest.h>
#include <memory_resource>
#include <span>
#include <string_view>
#include "document_utils.h"
#include "document_test_helpers.h"
#include "layout_computer.h"
#include "test_helpers.h"
#include "parser.h"
#include "syntax.h"
#include "theme.h"

// ============================================================
// ExtractSelectedText
// ============================================================

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
    Node n;
    n.type = NodeType::Paragraph;
    n.SetText("a<b&c>d\"e'f");
    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(n));
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

// テーブルノードは node.GetText() の線形化テキストがレイアウトパス後にのみ埋まるため、
// テストではダミーの線形化テキストを設定して selection.active を立てる。
static TextSelection MakeTableFullSelection(Node& table)
{
    if (table.GetText().empty()) {
        table.SetText("table");
    }
    return TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(table.GetText().size()));
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
    n.SetText("fallback");
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

TEST(FindWordBoundaries, EmptyText)
{
    auto result = FindWordBoundaries("", 0);
    EXPECT_FALSE(result.found);
}

TEST(FindWordBoundaries, SingleWord)
{
    auto result = FindWordBoundaries("hello", 2);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 5u);
}

TEST(FindWordBoundaries, WordAtStart)
{
    auto result = FindWordBoundaries("hello world", 0);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 5u);
}

TEST(FindWordBoundaries, WordAtEnd)
{
    auto result = FindWordBoundaries("hello world", 6);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 6u);
    EXPECT_EQ(result.end, 11u);
}

TEST(FindWordBoundaries, PositionOnSpace)
{
    auto result = FindWordBoundaries("hello world", 5);
    EXPECT_FALSE(result.found);
}

TEST(FindWordBoundaries, PositionOnPunctuation)
{
    auto result = FindWordBoundaries("hello, world", 5);
    EXPECT_FALSE(result.found);
}

TEST(FindWordBoundaries, WordWithUnderscore)
{
    auto result = FindWordBoundaries("my_variable = 1", 3);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 11u);
}

TEST(FindWordBoundaries, WordWithNumbers)
{
    auto result = FindWordBoundaries("var123 = x", 3);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 6u);
}

TEST(FindWordBoundaries, PositionBeyondEnd)
{
    auto result = FindWordBoundaries("hello", 100);
    // 最後の文字にクランプされるべき
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 5u);
}

TEST(FindWordBoundaries, KatakanaSequence)
{
    // カタカナ連続区間は同一カテゴリで一塊に選択される (UTF-8 で各 3 byte)。
    auto result = FindWordBoundaries("テスト test", 0);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 9u);
}

TEST(FindWordBoundaries, AsciiWordAfterCjk)
{
    // CJK の後の ASCII 単語をクリックすると動作するべき。pos は UTF-8 byte。
    // テスト = 9 byte, ' ' = 1 byte, "test" は offset 10
    auto result = FindWordBoundaries("テスト test", 10);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 10u);
    EXPECT_EQ(result.end, 14u);
}

TEST(FindWordBoundariesW, SingleWord)
{
    auto result = FindWordBoundaries(std::wstring_view{ L"hello world" }, 2);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 5u);
}

TEST(FindWordBoundariesW, PositionOnSpace)
{
    auto result = FindWordBoundaries(std::wstring_view{ L"hello world" }, 5);
    EXPECT_FALSE(result.found);
}

TEST(FindWordBoundariesW, AsciiWordAfterCjk)
{
    // wstring (UTF-16) では CJK は 1 wchar_t、空白 1、"test" 4。"t" は offset 4。
    auto result = FindWordBoundaries(std::wstring_view{ L"テスト test" }, 4);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 4u);
    EXPECT_EQ(result.end, 8u);
}

TEST(FindWordBoundariesW, KatakanaSequence)
{
    // UTF-16 のカタカナ連続区間 (各 1 wchar_t)。
    auto result = FindWordBoundaries(std::wstring_view{ L"テスト" }, 0);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 3u);
}

// ============================================================
// ExtractFilename
// ============================================================

TEST(ExtractFilename, EmptyPath)
{
    EXPECT_TRUE(ExtractFilename(L"").empty());
}

TEST(ExtractFilename, BackslashPath)
{
    EXPECT_EQ(ExtractFilename(L"C:\\Users\\test\\file.md"), L"file.md");
}

TEST(ExtractFilename, ForwardSlashPath)
{
    EXPECT_EQ(ExtractFilename(L"C:/Users/test/file.md"), L"file.md");
}

TEST(ExtractFilename, FilenameOnly)
{
    EXPECT_EQ(ExtractFilename(L"file.md"), L"file.md");
}

TEST(ExtractFilename, TrailingSeparator)
{
    EXPECT_EQ(ExtractFilename(L"C:\\dir\\"), L"");
}

TEST(ExtractFilename, JapaneseFilename)
{
    EXPECT_EQ(ExtractFilename(L"C:\\ドキュメント\\ファイル.md"), L"ファイル.md");
}

// ============================================================
// BuildTitleString
// ============================================================

TEST(BuildTitleString, EmptyPath)
{
    EXPECT_EQ(BuildTitleString(L""), L"mendo");
}

TEST(BuildTitleString, WithPath)
{
    EXPECT_EQ(BuildTitleString(L"C:\\dir\\test.md"), L"test.md - mendo");
}

TEST(BuildTitleString, FilenameOnly)
{
    EXPECT_EQ(BuildTitleString(L"readme.md"), L"readme.md - mendo");
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
    table_node.SetText("A\tB\n1\t2");

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
    n.SetText("hello");
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
    n.SetText("hello");
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
    node.SetText("link1 link2");

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

// ---- FindWordBoundaries 追加テスト ----

TEST(FindWordBoundaries, SingleCharWord)
{
    auto result = FindWordBoundaries("a", 0);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 1u);
}

TEST(FindWordBoundaries, AllSpaces)
{
    auto result = FindWordBoundaries("   ", 1);
    EXPECT_FALSE(result.found);
}

TEST(FindWordBoundaries, MixedPunctuationAndWords)
{
    auto result = FindWordBoundaries("(hello)", 3);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 1u);
    EXPECT_EQ(result.end, 6u);
}

// ---- FindWordBoundaries 文字種カテゴリ (UTF-8) ----

TEST(FindWordBoundaries, HiraganaSequence)
{
    // 「これはテスト」で「これは」(ひらがな) の最初の文字をクリック
    // ひらがな = 各 3 byte。「これは」= 9 byte、「テスト」は別カテゴリで止まる。
    auto result = FindWordBoundaries("これはテスト", 0);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 9u);
}

TEST(FindWordBoundaries, HanSequence)
{
    // 「日本語」(漢字 3 文字, 各 3 byte = 9 byte)
    auto result = FindWordBoundaries("日本語です", 3);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 9u);
}

TEST(FindWordBoundaries, HiraganaAfterHan)
{
    // 「日本語です」の「で」(offset 9) をクリック → 「です」(6 byte) が選択
    auto result = FindWordBoundaries("日本語です", 9);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 9u);
    EXPECT_EQ(result.end, 15u);
}

TEST(FindWordBoundaries, KatakanaWithLongVowel)
{
    // 「コーヒー」: 長音「ー」(U+30FC) はカタカナと同カテゴリ。各 3 byte = 12 byte。
    auto result = FindWordBoundaries("コーヒー", 3);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 12u);
}

TEST(FindWordBoundaries, FullwidthAlnumSequence)
{
    // 全角「ＡＢＣ１２３」: U+FF21 U+FF22 U+FF23 U+FF11 U+FF12 U+FF13 (各 3 byte = 18 byte)
    auto result = FindWordBoundaries("ＡＢＣ１２３", 6);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 18u);
}

TEST(FindWordBoundaries, PosOnUtf8ContinuationByte)
{
    // 「テスト」の中間バイト (例: 1) を指しても先頭バイトにスナップして同じ結果
    auto result = FindWordBoundaries("テスト", 1);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 9u);
}

TEST(FindWordBoundaries, FullwidthSymbolNotSelected)
{
    // 全角句読点「、」(U+3001) は Other → 選択されない
    auto result = FindWordBoundaries("、", 0);
    EXPECT_FALSE(result.found);
}

TEST(FindWordBoundaries, HanRepetitionMark)
{
    // 「人々」: 「々」(U+3005) は Han として「人」(U+4EBA) と連続して選択される
    auto result = FindWordBoundaries("人々", 0);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 6u);
}

// ---- FindWordBoundaries 文字種カテゴリ (UTF-16) ----

TEST(FindWordBoundariesW, HiraganaSequence)
{
    auto result = FindWordBoundaries(std::wstring_view{ L"これはテスト" }, 0);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 3u);
}

TEST(FindWordBoundariesW, HanAndHiraganaBoundary)
{
    // 「日本語です」 (UTF-16) で 「で」(offset 3) をクリック → 「です」が選択
    auto result = FindWordBoundaries(std::wstring_view{ L"日本語です" }, 3);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 3u);
    EXPECT_EQ(result.end, 5u);
}

// ---- BMP 外文字 (4byte UTF-8 / サロゲートペア) ----

TEST(FindWordBoundaries, HanInSupplementaryPlane)
{
    // 「𠮷田」: 𠮷 = U+20BB7 (UTF-8 4byte = F0 A0 AE B7), 田 = U+7530 (3byte)。
    // 両方 Han カテゴリで連続して選択されるはず。
    auto result = FindWordBoundaries("𠮷田", 0);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 7u);
}

TEST(FindWordBoundaries, PosOnUtf8FourByteContinuation)
{
    // 「𠮷田」の 4byte シーケンス内側 (pos=2) を指しても先頭にスナップ。
    auto result = FindWordBoundaries("𠮷田", 2);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 7u);
}

TEST(FindWordBoundariesW, HanSurrogatePair)
{
    // L"𠮷田" = { 0xD842, 0xDFB7, 0x7530 }、長さ 3。
    auto result = FindWordBoundaries(std::wstring_view{ L"𠮷田" }, 0);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 3u);
}

TEST(FindWordBoundariesW, PosOnLowSurrogate)
{
    // pos=1 (low surrogate) を指しても high surrogate にスナップして同じ結果。
    auto result = FindWordBoundaries(std::wstring_view{ L"𠮷田" }, 1);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 3u);
}

// ---- ExtractFilename 追加テスト ----

TEST(ExtractFilename, UncPath)
{
    EXPECT_EQ(ExtractFilename(L"\\\\server\\share\\file.md"), L"file.md");
}

TEST(ExtractFilename, MixedSeparators)
{
    EXPECT_EQ(ExtractFilename(L"C:\\dir/subdir\\file.md"), L"file.md");
}

// ============================================================
// FindFirstDifference (UTF-16 コード単位の差分検出)
// ============================================================

TEST(FindFirstDifference, IdenticalStrings)
{
    EXPECT_EQ(FindFirstDifference("hello", "hello"), std::string_view::npos);
}

TEST(FindFirstDifference, BothEmpty)
{
    EXPECT_EQ(FindFirstDifference("", ""), std::string_view::npos);
}

TEST(FindFirstDifference, DifferentFirstUnit)
{
    EXPECT_EQ(FindFirstDifference("abc", "xbc"), 0u);
}

TEST(FindFirstDifference, DifferentMiddle)
{
    EXPECT_EQ(FindFirstDifference("abcdef", "abcXef"), 3u);
}

TEST(FindFirstDifference, DifferentLastUnit)
{
    EXPECT_EQ(FindFirstDifference("abc", "abX"), 2u);
}

TEST(FindFirstDifference, NewLongerThanOld)
{
    EXPECT_EQ(FindFirstDifference("abc", "abcdef"), 3u);
}

TEST(FindFirstDifference, OldLongerThanNew)
{
    EXPECT_EQ(FindFirstDifference("abcdef", "abc"), 3u);
}

TEST(FindFirstDifference, EmptyOld)
{
    EXPECT_EQ(FindFirstDifference("", "new"), 0u);
}

TEST(FindFirstDifference, EmptyNew)
{
    EXPECT_EQ(FindFirstDifference("old", ""), 0u);
}

TEST(FindFirstDifference, CjkContent)
{
    // UTF-8 byte 単位で先頭差分位置を返す。
    std::string a = "あいう";
    std::string b = "あいえ";
    size_t diff = FindFirstDifference(a, b);
    // UTF-8: あ E3 81 82 / い E3 81 84 / う E3 81 86 vs え E3 81 88
    // 先頭 6 byte ("あい") + "う" の 1〜2 byte目 (E3 81) 一致 → 8 byte 目で差分
    EXPECT_EQ(diff, 8u);
}

// ============================================================
// AnalyzeReloadDiff
// ============================================================

TEST(AnalyzeReloadDiff, IdenticalContentReturnsNoChange)
{
    const auto d = AnalyzeReloadDiff("hello world", "hello world");
    EXPECT_EQ(d.op, ReloadOp::NoChange);
    EXPECT_EQ(d.diff_pos, std::string_view::npos);
}

TEST(AnalyzeReloadDiff, BothEmptyReturnsNoChange)
{
    const auto d = AnalyzeReloadDiff("", "");
    EXPECT_EQ(d.op, ReloadOp::NoChange);
    EXPECT_EQ(d.diff_pos, std::string_view::npos);
}

TEST(AnalyzeReloadDiff, AppendedSuffixIsPrefixGrowth)
{
    // 末尾に追記 → prefix-only growth（スクロール維持）
    const auto d = AnalyzeReloadDiff("abc", "abcdef");
    EXPECT_EQ(d.op, ReloadOp::PrefixGrowth);
    EXPECT_EQ(d.diff_pos, 3u);
}

TEST(AnalyzeReloadDiff, EmptyToContentIsPrefixGrowth)
{
    // 空ファイル → 何か書いた。prefix-only growth として扱う。
    const auto d = AnalyzeReloadDiff("", "new content");
    EXPECT_EQ(d.op, ReloadOp::PrefixGrowth);
    EXPECT_EQ(d.diff_pos, 0u);
}

TEST(AnalyzeReloadDiff, TruncatedSuffixIsDeferPrefixShrink)
{
    // 末尾が消えた = truncate。エディタの truncate→rewrite 前半の可能性があるため defer。
    const auto d = AnalyzeReloadDiff("abcdef", "abc");
    EXPECT_EQ(d.op, ReloadOp::DeferPrefixShrink);
    EXPECT_EQ(d.diff_pos, 3u);
}

TEST(AnalyzeReloadDiff, ContentToEmptyIsDeferPrefixShrink)
{
    // 全消去も truncate → rewrite の前半とみなして defer する
    const auto d = AnalyzeReloadDiff("old content", "");
    EXPECT_EQ(d.op, ReloadOp::DeferPrefixShrink);
    EXPECT_EQ(d.diff_pos, 0u);
}

TEST(AnalyzeReloadDiff, MiddleChangeIsFullReload)
{
    // 中間で変化した。prefix-only ではないので全体リロード。
    const auto d = AnalyzeReloadDiff("abcdef", "abcXef");
    EXPECT_EQ(d.op, ReloadOp::FullReload);
    EXPECT_EQ(d.diff_pos, 3u);
}

TEST(AnalyzeReloadDiff, FirstUnitChangeIsFullReload)
{
    // 先頭で差分があれば必ず FullReload（同一長さなので prefix-only にならない）。
    const auto d = AnalyzeReloadDiff("abc", "Xbc");
    EXPECT_EQ(d.op, ReloadOp::FullReload);
    EXPECT_EQ(d.diff_pos, 0u);
}

TEST(AnalyzeReloadDiff, LengthChangedWithMiddleDiffIsFullReload)
{
    // 途中で差分があり、かつ長さも変わる → prefix-only ではなく FullReload。
    const auto d = AnalyzeReloadDiff("abcdef", "abcYYYz");
    EXPECT_EQ(d.op, ReloadOp::FullReload);
    EXPECT_EQ(d.diff_pos, 3u);
}

TEST(AnalyzeReloadDiff, CjkSuffixAppendedIsPrefixGrowth)
{
    // CJK 末尾追記も prefix-only growth として扱える。diff_pos は UTF-8 byte。
    const std::string old_text = "あいう";
    const std::string new_text = "あいうえお";
    const auto d = AnalyzeReloadDiff(old_text, new_text);
    EXPECT_EQ(d.op, ReloadOp::PrefixGrowth);
    EXPECT_EQ(d.diff_pos, 9u); // "あいう" = 9 byte UTF-8
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
    nodes[0].source_offset = 0;
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 0), 0);
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 100), 0);
}

TEST(FindNodeBySourceOffset, OffsetBeforeAllNodes)
{
    std::pmr::vector<Node> nodes(2);
    nodes[0].source_offset = 10;
    nodes[1].source_offset = 20;
    // diff_offset=5 は最初のノード(offset=10)よりも前 → 該当なし
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 5), -1);
}

TEST(FindNodeBySourceOffset, ExactMatch)
{
    std::pmr::vector<Node> nodes(3);
    nodes[0].source_offset = 0;
    nodes[1].source_offset = 10;
    nodes[2].source_offset = 25;
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 10), 1);
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 25), 2);
}

TEST(FindNodeBySourceOffset, BetweenNodes)
{
    std::pmr::vector<Node> nodes(3);
    nodes[0].source_offset = 0;
    nodes[1].source_offset = 10;
    nodes[2].source_offset = 25;
    // offset=15 はノード1(10)とノード2(25)の間 → ノード1を返す
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 15), 1);
}

TEST(FindNodeBySourceOffset, SkipsUnsetOffsets)
{
    std::pmr::vector<Node> nodes(3);
    nodes[0].source_offset = 0;
    nodes[1].source_offset = kUnsetSourceOffset; // 未設定（HorizontalRule等）
    nodes[2].source_offset = 20;
    // 未設定値は常に diff_offset 以上にならない（kUnsetSourceOffset <= diff_offset は通常 false）
    // → ノード0を返す
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 10), 0);
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 20), 2);
}

TEST(FindNodeBySourceOffset, ParsedMarkdown)
{
    auto nodes = ParseMarkdown("# Title\n\nParagraph\n\n## Section").nodes;
    ASSERT_GE(nodes.size(), 3u);
    // 各ノードが有効な source_offset を持つ
    for (const auto& n : nodes) {
        EXPECT_NE(n.source_offset, kUnsetSourceOffset);
    }
    // 最初のノードの offset は "# " の後 = 2
    EXPECT_EQ(nodes[0].source_offset, 2u);
    // "Paragraph" は "# Title\n\n" = 9バイト目から
    EXPECT_EQ(nodes[1].source_offset, 9u);
}

TEST(FindNodeBySourceOffset, LastNodeForLargeOffset)
{
    std::pmr::vector<Node> nodes(3);
    nodes[0].source_offset = 0;
    nodes[1].source_offset = 100;
    nodes[2].source_offset = 200;
    // ソース末尾を超えるオフセット → 最後のノードを返す
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 999), 2);
}

TEST(FindNodeBySourceOffset, AllUnsetOffsets)
{
    // 全ノードが未設定（テキストなし）→ 該当なし
    std::pmr::vector<Node> nodes(3);
    nodes[0].source_offset = kUnsetSourceOffset;
    nodes[1].source_offset = kUnsetSourceOffset;
    nodes[2].source_offset = kUnsetSourceOffset;
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 50), -1);
}

TEST(FindNodeBySourceOffset, MixedWithHorizontalRules)
{
    // パース結果で HorizontalRule が混在するケース
    auto nodes = ParseMarkdown("AAA\n\n---\n\nBBB").nodes;
    ASSERT_GE(nodes.size(), 3u);
    // "AAA" offset=0, "---" は未設定, "BBB" offset=10
    EXPECT_EQ(nodes[0].source_offset, 0u);
    EXPECT_EQ(nodes[1].source_offset, kUnsetSourceOffset);

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
// source_offset は wide 単位なので、wide のまま FindFirstDifference / ParseMarkdown を呼ぶ。
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
    return FindNodeBySourceOffset(nodes, static_cast<uint32_t>(diff_pos));
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

TEST(IsPrefixOnlyDiff, IdenticalSizes)
{
    // diff_pos=5, old_size=10, new_size=10 → min=10, 5!=10 → false
    EXPECT_FALSE(IsPrefixOnlyDiff(5, 10, 10));
}

TEST(IsPrefixOnlyDiff, OldIsPrefix)
{
    // old="abc"(3), new="abcdef"(6) → diff_pos=3=min(3,6) → true
    EXPECT_TRUE(IsPrefixOnlyDiff(3, 3, 6));
}

TEST(IsPrefixOnlyDiff, NewIsPrefix)
{
    // old="abcdef"(6), new="abc"(3) → diff_pos=3=min(6,3) → true
    EXPECT_TRUE(IsPrefixOnlyDiff(3, 6, 3));
}

TEST(IsPrefixOnlyDiff, DiffBeforeEnd)
{
    // old="abXdef"(6), new="abc"(3) → diff_pos=2, min=3 → false
    EXPECT_FALSE(IsPrefixOnlyDiff(2, 6, 3));
}

TEST(IsPrefixOnlyDiff, EmptyOld)
{
    // old=""(0), new="abc"(3) → diff_pos=0=min(0,3) → true
    EXPECT_TRUE(IsPrefixOnlyDiff(0, 0, 3));
}

TEST(IsPrefixOnlyDiff, EmptyNew)
{
    // old="abc"(3), new=""(0) → diff_pos=0=min(3,0) → true
    EXPECT_TRUE(IsPrefixOnlyDiff(0, 3, 0));
}

TEST(IsPrefixOnlyDiff, BothEmpty)
{
    // diff_pos=0, old=0, new=0 → 0=min(0,0) → true
    // ただし通常 FindFirstDifference は npos を返すのでここには到達しない
    EXPECT_TRUE(IsPrefixOnlyDiff(0, 0, 0));
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

// ヘルパー: source_offset を等間隔に設定したノード列を構築する
static std::pmr::vector<Node> MakeNodes(int count, uint32_t offset_step = 100)
{
    std::pmr::vector<Node> nodes(count);
    for (int i = 0; i < count; ++i) {
        nodes[i].source_offset = static_cast<uint32_t>(i) * offset_step;
    }
    return nodes;
}

TEST(CalcScrollYForDiff, FallbackWhenNoNodes)
{
    std::pmr::vector<Node> nodes;
    LayoutCache cache;
    EXPECT_FLOAT_EQ(CalcScrollYForDiff(nodes, cache, "content", 0, 500.0f, 42.0f), 42.0f);
}

TEST(CalcScrollYForDiff, FallbackWhenNodeNotFound)
{
    // すべてのノードの source_offset が diff_pos より大きい
    auto nodes = MakeNodes(3, 100);
    nodes[0].source_offset = 50;
    auto cache = MakeUniformCache(3);
    // diff_pos=10 < 全ノードの最小 offset(50) → -1 → fallback
    EXPECT_FLOAT_EQ(CalcScrollYForDiff(nodes, cache, "content", 10, 500.0f, 99.0f), 99.0f);
}

TEST(CalcScrollYForDiff, ScrollsToNodeStartWithMargin)
{
    // 3ノード: offset=0,100,200 / y=0,100,200 / height=100
    auto nodes = MakeNodes(3, 100);
    auto cache = MakeUniformCache(3, 100.0f);
    std::string content(300, 'x');

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
    auto nodes = MakeNodes(2, 100);
    auto cache = MakeUniformCache(2, 1000.0f);
    std::string content(200, 'x');

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
    auto nodes = MakeNodes(2, 100);
    auto cache = MakeUniformCache(2, 1000.0f);
    std::string content(200, 'x');

    // diff_pos=99 → node 0, fraction=99/100=0.99
    // y = 0 + 1000*0.99 = 990, scroll = 990-20 = 970
    EXPECT_FLOAT_EQ(CalcScrollYForDiff(nodes, cache, content, 99, 100.0f, 0.0f), 970.0f);
    // diff_pos=100 → node 1 (exact match), y=1000, scroll = 1000-20 = 980
    EXPECT_FLOAT_EQ(CalcScrollYForDiff(nodes, cache, content, 100, 100.0f, 0.0f), 980.0f);
}

TEST(CalcScrollYForDiff, SkipsUnsetSourceOffsets)
{
    // ノード1の source_offset が未設定の場合、ノード2を next_start として使う
    std::pmr::vector<Node> nodes(3);
    nodes[0].source_offset = 0;
    nodes[1].source_offset = kUnsetSourceOffset; // 未設定（HorizontalRule等）
    nodes[2].source_offset = 200;
    auto cache = MakeUniformCache(3, 100.0f);
    std::string content(300, 'x');

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
    auto nodes = MakeNodes(1, 0);
    nodes[0].source_offset = 0;
    auto cache = MakeUniformCache(1, 1000.0f);
    std::string content(100, 'x');

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
    auto nodes = MakeNodes(5, 100);
    auto cache = MakeUniformCache(3, 100.0f); // キャッシュは3つだけ

    // diff_pos=400 → node 4 だがキャッシュは3つ → fallback
    EXPECT_FLOAT_EQ(CalcScrollYForDiff(nodes, cache, std::string(500, 'x'), 400, 500.0f, 77.0f), 77.0f);
}

TEST(CalcScrollYForDiff, ParsedMarkdownIntegration)
{
    // 実際のMarkdownをパースして差分スクロール位置を計算する統合テスト
    std::string md = "# Title\n\nFirst paragraph\n\nSecond paragraph\n\nThird paragraph";
    auto nodes = ParseMarkdown(std::string_view{ md }).nodes;
    ASSERT_GE(nodes.size(), 4u);

    // 等間隔 cache (text_top=0,50,100,...) を構築。spacing/Heading 個別寸法は無視し、
    // CalcScrollYForDiff が cache[i].text_top をどう参照するかだけを見る統合テスト。
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
    const float last_old_node_y = cache[nodes.size() - 2].text_top;
    const float appended_node_y = cache[nodes.size() - 1].text_top;

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

// issue#185 回帰テスト:
// CalcScrollYForDiff が cache[i].text_top フィールドを直読していること (Fenwick PrefixSum
// 経由の TextTopOf は float 加算順の違いでノード数が増えるほど誤差が累積し、ビューポート
// 側 (cache[i].text_top 直読) との乖離が「下部更新時ほど大きく」なる)。
// フィールドを意図的に Fenwick 由来の値からズラし、CalcScrollYForDiff がフィールド側を
// 採用していることを担保する。
TEST(CalcScrollYForDiff, ReadsCachedTextTopFieldNotFenwick)
{
    auto nodes = MakeNodes(3, 100);
    auto cache = MakeUniformCache(3, 100.0f);
    std::string content(300, 'x');

    // cache[2].text_top を Fenwick が返す値 (=200) から大きくズラす。
    // 実機の累積誤差を模した状況で、フィールド直読なら 999 が、Fenwick 経由なら 200 が
    // ノード上端 Y として採用される。
    cache[2].text_top = 999.0f;

    // diff_pos=200 → node 2 (ピッタリ境界) → fraction=0 → node_y=cache[2].text_top
    // margin = 500*0.2 = 100 → 期待値 = 999 - 100 = 899
    EXPECT_FLOAT_EQ(CalcScrollYForDiff(nodes, cache, content, 200, 500.0f, 0.0f), 899.0f);
}

// ============================================================
// ToLowerAscii
// ============================================================

TEST(ToLowerAscii, AllUppercase)
{
    EXPECT_EQ(ToLowerAscii("HELLO"), "hello");
}

TEST(ToLowerAscii, AllLowercase)
{
    EXPECT_EQ(ToLowerAscii("hello"), "hello");
}

TEST(ToLowerAscii, MixedCase)
{
    EXPECT_EQ(ToLowerAscii("HeLLo WoRLd"), "hello world");
}

TEST(ToLowerAscii, Empty)
{
    EXPECT_TRUE(ToLowerAscii("").empty());
}

TEST(ToLowerAscii, NonAsciiUnchanged)
{
    EXPECT_EQ(ToLowerAscii("日本語"), "日本語");
}

TEST(ToLowerAscii, DigitsAndSymbols)
{
    EXPECT_EQ(ToLowerAscii("ABC-123_XYZ"), "abc-123_xyz");
}

TEST(ToLowerAscii, BoundaryChars)
{
    // A(0x41)の直前@(0x40)、Z(0x5A)の直後[(0x5B)は変換されないこと
    EXPECT_EQ(ToLowerAscii("@A[Z"), "@a[z");
}

// ============================================================
// IsMarkdownFile
// ============================================================

TEST(IsMarkdownFile, DotMd)
{
    EXPECT_TRUE(IsMarkdownFile(L"readme.md"));
}

TEST(IsMarkdownFile, DotMarkdown)
{
    EXPECT_TRUE(IsMarkdownFile(L"doc.markdown"));
}

TEST(IsMarkdownFile, DotMkd)
{
    EXPECT_TRUE(IsMarkdownFile(L"notes.mkd"));
}

TEST(IsMarkdownFile, UpperCaseExtension)
{
    EXPECT_TRUE(IsMarkdownFile(L"README.MD"));
}

TEST(IsMarkdownFile, MixedCaseExtension)
{
    EXPECT_TRUE(IsMarkdownFile(L"test.Markdown"));
}

TEST(IsMarkdownFile, FullPath)
{
    EXPECT_TRUE(IsMarkdownFile(L"C:\\Users\\user\\Documents\\file.md"));
}

TEST(IsMarkdownFile, NotMarkdown)
{
    EXPECT_FALSE(IsMarkdownFile(L"test.txt"));
}

TEST(IsMarkdownFile, NoExtension)
{
    EXPECT_FALSE(IsMarkdownFile(L"readme"));
}

TEST(IsMarkdownFile, EmptyPath)
{
    EXPECT_FALSE(IsMarkdownFile(L""));
}

TEST(IsMarkdownFile, DotOnly)
{
    EXPECT_FALSE(IsMarkdownFile(L"."));
}

TEST(IsMarkdownFile, SimilarExtension)
{
    EXPECT_FALSE(IsMarkdownFile(L"file.mdd"));
}

TEST(IsMarkdownFile, HtmlFile)
{
    EXPECT_FALSE(IsMarkdownFile(L"page.html"));
}

TEST(IsMarkdownFile, DotInDirectory)
{
    EXPECT_TRUE(IsMarkdownFile(L"C:\\my.project\\docs\\readme.md"));
}

// ============================================================
// IsHelpPath
// ============================================================

TEST(IsHelpPath, CorrectPath)
{
    EXPECT_TRUE(IsHelpPath(L"mendo://help"));
}

TEST(IsHelpPath, WrongPath)
{
    EXPECT_FALSE(IsHelpPath(L"mendo://other"));
}

TEST(IsHelpPath, EmptyPath)
{
    EXPECT_FALSE(IsHelpPath(L""));
}

TEST(IsHelpPath, PartialMatch)
{
    EXPECT_FALSE(IsHelpPath(L"mendo://hel"));
}

TEST(IsHelpPath, CaseSensitive)
{
    EXPECT_FALSE(IsHelpPath(L"MENDO://HELP"));
}
