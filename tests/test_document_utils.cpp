#include <gtest/gtest.h>
#include <memory_resource>
#include <string_view>
#include "document_utils.h"
#include "document_test_helpers.h"
#include "test_helpers.h"
#include "parser.h"
#include "syntax.h"

// ============================================================
// ExtractSelectedText
// ============================================================

TEST(ExtractSelectedText, InactiveSelectionReturnsEmpty)
{
    auto nodes = ParseMarkdown(L"Hello world").nodes;
    TextSelection sel;
    sel.active = false;
    EXPECT_TRUE(ExtractSelectedText(nodes, sel).empty());
}

TEST(ExtractSelectedText, SingleNodeFullSelection)
{
    auto nodes = ParseMarkdown(L"Hello world").nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    EXPECT_EQ(ExtractSelectedText(nodes, sel), L"Hello world");
}

TEST(ExtractSelectedText, SingleNodePartialSelection)
{
    auto nodes = ParseMarkdown(L"Hello world").nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, 5);
    EXPECT_EQ(ExtractSelectedText(nodes, sel), L"Hello");
}

TEST(ExtractSelectedText, SingleNodeMiddleSelection)
{
    auto nodes = ParseMarkdown(L"Hello world").nodes;
    auto sel = TextSelection::MakeOrdered(0, 6, 0, 11);
    EXPECT_EQ(ExtractSelectedText(nodes, sel), L"world");
}

TEST(ExtractSelectedText, MultipleNodesFullSelection)
{
    auto nodes = ParseMarkdown(L"First\n\nSecond\n\nThird").nodes;
    ASSERT_EQ(nodes.size(), 3u);
    auto sel = TextSelection::MakeOrdered(
        0, 0, 2, static_cast<uint32_t>(nodes[2].GetText().size()));
    auto result = ExtractSelectedText(nodes, sel);
    EXPECT_NE(result.find(L"First"), std::wstring::npos);
    EXPECT_NE(result.find(L"Second"), std::wstring::npos);
    EXPECT_NE(result.find(L"Third"), std::wstring::npos);
}

TEST(ExtractSelectedText, MultipleNodesPartialSelection)
{
    auto nodes = ParseMarkdown(L"First\n\nSecond\n\nThird").nodes;
    ASSERT_EQ(nodes.size(), 3u);
    // "First"の途中から"Third"の途中まで選択
    auto sel = TextSelection::MakeOrdered(0, 2, 2, 3);
    auto result = ExtractSelectedText(nodes, sel);
    EXPECT_EQ(result.substr(0, 3), L"rst");
    EXPECT_NE(result.find(L"Second"), std::wstring::npos);
    EXPECT_NE(result.find(L"Thi"), std::wstring::npos);
}

TEST(ExtractSelectedText, NewlineBetweenNodes)
{
    auto nodes = ParseMarkdown(L"A\n\nB").nodes;
    ASSERT_EQ(nodes.size(), 2u);
    auto sel = TextSelection::MakeOrdered(
        0, 0, 1, static_cast<uint32_t>(nodes[1].GetText().size()));
    auto result = ExtractSelectedText(nodes, sel);
    // ノード間に\r\nが含まれるべき
    EXPECT_NE(result.find(L"\r\n"), std::wstring::npos);
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
    auto nodes = ParseMarkdown(L"Short").nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, 1000);
    // end_posがテキストサイズを超える場合はクランプされるべき
    EXPECT_EQ(ExtractSelectedText(nodes, sel), L"Short");
}

TEST(ExtractSelectedText, JapaneseText)
{
    auto nodes = ParseMarkdown(L"日本語テスト").nodes;
    auto sel = TextSelection::MakeOrdered(
        0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    EXPECT_EQ(ExtractSelectedText(nodes, sel), L"日本語テスト");
}

// ============================================================
// ExtractSelectedTextAsHtml
// ============================================================

TEST(ExtractSelectedTextAsHtml, InactiveSelectionReturnsEmpty)
{
    auto nodes = ParseMarkdown(L"Hello").nodes;
    TextSelection sel;
    sel.active = false;
    EXPECT_TRUE(ExtractSelectedTextAsHtml(nodes, sel).empty());
}

TEST(ExtractSelectedTextAsHtml, ParagraphWrapsInPTag)
{
    auto nodes = ParseMarkdown(L"Hello world").nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    EXPECT_EQ(ExtractSelectedTextAsHtml(nodes, sel), L"<p>Hello world</p>");
}

TEST(ExtractSelectedTextAsHtml, HeadingLevels)
{
    auto nodes = ParseMarkdown(L"## Section").nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    EXPECT_EQ(ExtractSelectedTextAsHtml(nodes, sel), L"<h2>Section</h2>");
}

TEST(ExtractSelectedTextAsHtml, BoldAndItalic)
{
    auto nodes = ParseMarkdown(L"**bold** and *italic*").nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find(L"<strong>bold</strong>"), std::wstring::npos);
    EXPECT_NE(html.find(L"<em>italic</em>"), std::wstring::npos);
}

TEST(ExtractSelectedTextAsHtml, InlineCode)
{
    auto nodes = ParseMarkdown(L"text `code` more").nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find(L"<code>code</code>"), std::wstring::npos);
}

TEST(ExtractSelectedTextAsHtml, LinkProducesAnchor)
{
    auto nodes = ParseMarkdown(L"[click](https://example.com)").nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find(L"<a href=\"https://example.com\">click</a>"), std::wstring::npos);
}

TEST(ExtractSelectedTextAsHtml, EscapesSpecialChars)
{
    Node n;
    n.type = NodeType::Paragraph;
    n.SetText(L"a<b&c>d\"e'f");
    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(n));
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    EXPECT_EQ(ExtractSelectedTextAsHtml(nodes, sel),
        L"<p>a&lt;b&amp;c&gt;d&quot;e&#39;f</p>");
}

TEST(ExtractSelectedTextAsHtml, CodeBlockIsEscapedInPreCode)
{
    auto nodes = ParseMarkdown(L"```\nint x = 1 < 2;\n```").nodes;
    ASSERT_EQ(nodes[0].type, NodeType::CodeBlock);
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find(L"<pre"), std::wstring::npos);
    EXPECT_NE(html.find(L"<code>"), std::wstring::npos);
    EXPECT_NE(html.find(L"&lt;"), std::wstring::npos);
    EXPECT_NE(html.find(L"</code></pre>"), std::wstring::npos);
    // コードブロック内ではインラインタグ化しない
    EXPECT_EQ(html.find(L"<strong>"), std::wstring::npos);
    EXPECT_EQ(html.find(L"<em>"), std::wstring::npos);
}

TEST(ExtractSelectedTextAsHtml, CodeBlockWithoutTokensHasNoSyntaxSpans)
{
    // 言語指定なし -> syntax_tokens が空 -> span タグは付かない
    auto nodes = ParseMarkdown(L"```\nplain text\n```").nodes;
    ASSERT_EQ(nodes[0].type, NodeType::CodeBlock);
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_EQ(html.find(L"<span"), std::wstring::npos);
    EXPECT_NE(html.find(L"plain text"), std::wstring::npos);
}

TEST(ExtractSelectedTextAsHtml, CodeBlockWithSyntaxTokensWrapsInSpans)
{
    // syntax_tokens を手動でセットし、span による色付けが行われることを確認
    auto nodes = ParseMarkdown(L"```cpp\nint x = 42;\n```").nodes;
    ASSERT_EQ(nodes[0].type, NodeType::CodeBlock);
    auto& n = nodes[0];
    const std::wstring_view text = n.GetText();
    ASSERT_FALSE(text.empty());
    // "int" を Keyword, "42" を Number としてマーク
    auto& tokens = n.syntax_tokens_mut();
    tokens.clear();
    const auto int_pos = static_cast<uint32_t>(text.find(L"int"));
    const auto num_pos = static_cast<uint32_t>(text.find(L"42"));
    ASSERT_NE(int_pos, static_cast<uint32_t>(std::wstring::npos));
    ASSERT_NE(num_pos, static_cast<uint32_t>(std::wstring::npos));
    tokens.push_back(SyntaxToken{ int_pos, 3u, SyntaxTokenType::Keyword });
    tokens.push_back(SyntaxToken{ num_pos, 2u, SyntaxTokenType::Number });

    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(text.size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find(L"<span style=\"color:#af00db\">int</span>"), std::wstring::npos);
    EXPECT_NE(html.find(L"<span style=\"color:#098658\">42</span>"), std::wstring::npos);
    // その他の Plain 区間は素のテキスト
    EXPECT_NE(html.find(L" x = "), std::wstring::npos);
}

TEST(ExtractSelectedTextAsHtml, CodeBlockSpanEscapesSpecialChars)
{
    auto nodes = ParseMarkdown(L"```cpp\na<b\n```").nodes;
    auto& n = nodes[0];
    const std::wstring_view text = n.GetText();
    auto& tokens = n.syntax_tokens_mut();
    tokens.clear();
    // 全体を文字列トークンとしてマーク
    tokens.push_back(SyntaxToken{ 0u, static_cast<uint32_t>(text.size()),
                                   SyntaxTokenType::String });

    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(text.size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    // span 内のテキストも HTML エスケープされる
    EXPECT_NE(html.find(L"&lt;"), std::wstring::npos);
    EXPECT_EQ(html.find(L"a<b"), std::wstring::npos);
}

TEST(ExtractSelectedTextAsHtml, CodeBlockDarkModeUsesDarkColors)
{
    auto nodes = ParseMarkdown(L"```cpp\nint x = 42;\n```").nodes;
    auto& n = nodes[0];
    const std::wstring_view text = n.GetText();
    auto& tokens = n.syntax_tokens_mut();
    tokens.clear();
    const auto int_pos = static_cast<uint32_t>(text.find(L"int"));
    const auto num_pos = static_cast<uint32_t>(text.find(L"42"));
    tokens.push_back(SyntaxToken{ int_pos, 3u, SyntaxTokenType::Keyword });
    tokens.push_back(SyntaxToken{ num_pos, 2u, SyntaxTokenType::Number });

    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(text.size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel, /*dark_mode=*/true);
    // ダーク用の色（VS Code Dark+ 相当）が使われる
    EXPECT_NE(html.find(L"<span style=\"color:#c586c0\">int</span>"), std::wstring::npos);
    EXPECT_NE(html.find(L"<span style=\"color:#b5cea8\">42</span>"), std::wstring::npos);
    // ライト用の色は混ざらない
    EXPECT_EQ(html.find(L"#af00db"), std::wstring::npos);
    EXPECT_EQ(html.find(L"#098658"), std::wstring::npos);
    // コードブロック背景がダーク色
    EXPECT_NE(html.find(L"background-color:#2d2d2d"), std::wstring::npos);
    EXPECT_NE(html.find(L"color:#d4d4d4"), std::wstring::npos);
}

// テーブルノードは node.GetText() の線形化テキストがレイアウトパス後にのみ埋まるため、
// テストではダミーの線形化テキストを設定して selection.active を立てる。
static TextSelection MakeTableFullSelection(Node& table)
{
    if (table.GetText().empty()) {
        table.SetText(L"table");
    }
    return TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(table.GetText().size()));
}

TEST(ExtractSelectedTextAsHtml, TableRendersAsTableStructure)
{
    auto nodes = ParseMarkdown(
        L"| A | B |\n"
        L"|---|---|\n"
        L"| 1 | 2 |"
    ).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].type, NodeType::Table);
    auto sel = MakeTableFullSelection(nodes[0]);
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find(L"<table"), std::wstring::npos);
    EXPECT_NE(html.find(L"<thead>"), std::wstring::npos);
    EXPECT_NE(html.find(L"<th"), std::wstring::npos);
    EXPECT_NE(html.find(L">A</th>"), std::wstring::npos);
    EXPECT_NE(html.find(L">B</th>"), std::wstring::npos);
    EXPECT_NE(html.find(L"</thead>"), std::wstring::npos);
    EXPECT_NE(html.find(L"<tbody>"), std::wstring::npos);
    EXPECT_NE(html.find(L"<td"), std::wstring::npos);
    EXPECT_NE(html.find(L">1</td>"), std::wstring::npos);
    EXPECT_NE(html.find(L">2</td>"), std::wstring::npos);
    EXPECT_NE(html.find(L"</tbody>"), std::wstring::npos);
    EXPECT_NE(html.find(L"</table>"), std::wstring::npos);
    // フォールバックの <pre> は使われないこと
    EXPECT_EQ(html.find(L"<pre>"), std::wstring::npos);
}

TEST(ExtractSelectedTextAsHtml, TableAlignmentAppliedAsTextAlign)
{
    auto nodes = ParseMarkdown(
        L"| L | C | R |\n"
        L"|:--|:--:|--:|\n"
        L"| a | b | c |"
    ).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].type, NodeType::Table);
    auto sel = MakeTableFullSelection(nodes[0]);
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find(L"text-align:center;"), std::wstring::npos);
    EXPECT_NE(html.find(L"text-align:right;"), std::wstring::npos);
}

TEST(ExtractSelectedTextAsHtml, TablePreservesInlineFormatting)
{
    auto nodes = ParseMarkdown(
        L"| A | B |\n"
        L"|---|---|\n"
        L"| **bold** | [link](https://example.com) |"
    ).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].type, NodeType::Table);
    auto sel = MakeTableFullSelection(nodes[0]);
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find(L"<strong>bold</strong>"), std::wstring::npos);
    EXPECT_NE(html.find(L"<a href=\"https://example.com\">link</a>"), std::wstring::npos);
}

TEST(ExtractSelectedTextAsHtml, TableDarkModeUsesDarkBorder)
{
    auto nodes = ParseMarkdown(
        L"| A | B |\n"
        L"|---|---|\n"
        L"| 1 | 2 |"
    ).nodes;
    ASSERT_EQ(nodes[0].type, NodeType::Table);
    auto sel = MakeTableFullSelection(nodes[0]);
    auto html = ExtractSelectedTextAsHtml(nodes, sel, /*dark_mode=*/true);
    EXPECT_NE(html.find(L"border:1px solid #3c3c3c"), std::wstring::npos);
    EXPECT_EQ(html.find(L"#d0d7de"), std::wstring::npos);
}

TEST(ExtractSelectedTextAsHtml, TableWithoutDataFallsBackToPre)
{
    // table_data が空のノードに対しては <pre> フォールバックで出力される。
    Node n;
    n.type = NodeType::Table;
    n.SetText(L"fallback");
    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(n));
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_EQ(html.find(L"<table"), std::wstring::npos);
    EXPECT_NE(html.find(L"<pre>fallback</pre>"), std::wstring::npos);
}

TEST(ExtractSelectedTextAsHtml, UnorderedListWrapsInUl)
{
    auto nodes = ParseMarkdown(L"- one\n- two").nodes;
    ASSERT_GE(nodes.size(), 2u);
    auto sel = TextSelection::MakeOrdered(0, 0, 1, static_cast<uint32_t>(nodes[1].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find(L"<ul>"), std::wstring::npos);
    EXPECT_NE(html.find(L"<li>one</li>"), std::wstring::npos);
    EXPECT_NE(html.find(L"<li>two</li>"), std::wstring::npos);
    EXPECT_NE(html.find(L"</ul>"), std::wstring::npos);
}

TEST(ExtractSelectedTextAsHtml, OrderedListWrapsInOl)
{
    auto nodes = ParseMarkdown(L"1. first\n2. second").nodes;
    ASSERT_GE(nodes.size(), 2u);
    auto sel = TextSelection::MakeOrdered(0, 0, 1, static_cast<uint32_t>(nodes[1].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find(L"<ol>"), std::wstring::npos);
    EXPECT_NE(html.find(L"<li>first</li>"), std::wstring::npos);
    EXPECT_NE(html.find(L"<li>second</li>"), std::wstring::npos);
    EXPECT_NE(html.find(L"</ol>"), std::wstring::npos);
}

TEST(ExtractSelectedTextAsHtml, BlockQuote)
{
    auto nodes = ParseMarkdown(L"> quoted text").nodes;
    ASSERT_EQ(nodes[0].type, NodeType::BlockQuote);
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find(L"<blockquote>"), std::wstring::npos);
    EXPECT_NE(html.find(L"</blockquote>"), std::wstring::npos);
    EXPECT_NE(html.find(L"quoted text"), std::wstring::npos);
}

TEST(ExtractSelectedTextAsHtml, HorizontalRule)
{
    auto nodes = ParseMarkdown(L"before\n\n---\n\nafter").nodes;
    ASSERT_GE(nodes.size(), 3u);
    ASSERT_EQ(nodes[1].type, NodeType::HorizontalRule);
    auto sel = TextSelection::MakeOrdered(0, 0, 2, static_cast<uint32_t>(nodes[2].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find(L"<hr>"), std::wstring::npos);
}

TEST(ExtractSelectedTextAsHtml, MultiParagraph)
{
    auto nodes = ParseMarkdown(L"First\n\nSecond").nodes;
    ASSERT_EQ(nodes.size(), 2u);
    auto sel = TextSelection::MakeOrdered(0, 0, 1, static_cast<uint32_t>(nodes[1].GetText().size()));
    EXPECT_EQ(ExtractSelectedTextAsHtml(nodes, sel),
        L"<p>First</p><p>Second</p>");
}

TEST(ExtractSelectedTextAsHtml, PartialSelectionInParagraph)
{
    auto nodes = ParseMarkdown(L"Hello world").nodes;
    auto sel = TextSelection::MakeOrdered(0, 6, 0, 11);
    EXPECT_EQ(ExtractSelectedTextAsHtml(nodes, sel), L"<p>world</p>");
}

TEST(ExtractSelectedTextAsHtml, OrderedTaskListWrapsInOl)
{
    auto nodes = ParseMarkdown(L"1. [ ] first\n2. [x] second").nodes;
    ASSERT_GE(nodes.size(), 2u);
    ASSERT_EQ(nodes[0].type, NodeType::TaskListItem);
    ASSERT_EQ(nodes[1].type, NodeType::TaskListItem);
    auto sel = TextSelection::MakeOrdered(0, 0, 1, static_cast<uint32_t>(nodes[1].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find(L"<ol>"), std::wstring::npos);
    EXPECT_NE(html.find(L"</ol>"), std::wstring::npos);
    EXPECT_EQ(html.find(L"<ul>"), std::wstring::npos);
}

TEST(ExtractSelectedTextAsHtml, UnsafeSchemeLinkIsStripped)
{
    auto nodes = ParseMarkdown(L"[click](javascript:alert(1))").nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_EQ(html.find(L"<a href="), std::wstring::npos);
    EXPECT_EQ(html.find(L"javascript"), std::wstring::npos);
    EXPECT_NE(html.find(L"click"), std::wstring::npos);
}

TEST(ExtractSelectedTextAsHtml, FileSchemeLinkIsStripped)
{
    auto nodes = ParseMarkdown(L"[open](file:///C:/secret.txt)").nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_EQ(html.find(L"<a href="), std::wstring::npos);
    EXPECT_NE(html.find(L"open"), std::wstring::npos);
}

TEST(ExtractSelectedTextAsHtml, MailtoLinkIsKept)
{
    auto nodes = ParseMarkdown(L"[mail](mailto:user@example.com)").nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_NE(html.find(L"<a href=\"mailto:user@example.com\">mail</a>"), std::wstring::npos);
}

TEST(ExtractSelectedTextAsHtml, InternalAnchorLinkIsStripped)
{
    auto nodes = ParseMarkdown(L"[sec](#section)").nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    auto html = ExtractSelectedTextAsHtml(nodes, sel);
    EXPECT_EQ(html.find(L"<a href="), std::wstring::npos);
    EXPECT_NE(html.find(L"sec"), std::wstring::npos);
}

// ============================================================
// FindLinkAtPosition
// ============================================================

TEST(FindLinkAtPosition, NoLinks)
{
    auto nodes = ParseMarkdown(L"plain text").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    auto result = FindLinkAtPosition(nodes[0], 0);
    EXPECT_FALSE(result.has_value());
}

TEST(FindLinkAtPosition, LinkFound)
{
    auto nodes = ParseMarkdown(L"[click](https://example.com)").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    // リンクテキスト内の位置
    auto result = FindLinkAtPosition(nodes[0], 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), L"https://example.com");
}

TEST(FindLinkAtPosition, PositionOutsideLink)
{
    auto nodes = ParseMarkdown(L"before [link](https://example.com) after").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    // "before"テキスト内の位置（リンクではないはず）
    auto result = FindLinkAtPosition(nodes[0], 0);
    EXPECT_FALSE(result.has_value());
}

TEST(FindLinkAtPosition, InternalLink)
{
    auto nodes = ParseMarkdown(L"[section](#my-section)").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    auto result = FindLinkAtPosition(nodes[0], 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), L"#my-section");
}

TEST(FindLinkAtPosition, PositionAtLinkBoundary)
{
    auto nodes = ParseMarkdown(L"[link](https://example.com)").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    // リンクの最後の文字の位置
    uint32_t last_pos = static_cast<uint32_t>(nodes[0].GetText().size()) - 1;
    auto result = FindLinkAtPosition(nodes[0], last_pos);
    ASSERT_TRUE(result.has_value());
}

TEST(FindLinkAtPosition, PositionBeyondText)
{
    auto nodes = ParseMarkdown(L"[link](https://example.com)").nodes;
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
    EXPECT_EQ(FindAnchorNodeIndexLinear(nodes, L"test"), -1);
}

TEST(FindAnchorNodeIndex, EmptyAnchor)
{
    auto nodes = ParseMarkdown(L"# Title").nodes;
    EXPECT_EQ(FindAnchorNodeIndexLinear(nodes, L""), -1);
}

TEST(FindAnchorNodeIndex, FindExistingAnchor)
{
    auto nodes = ParseMarkdown(L"# Title\n\nParagraph\n\n## Section").nodes;
    ASSERT_GE(nodes.size(), 3u);
    int idx = FindAnchorNodeIndexLinear(nodes, L"title");
    EXPECT_EQ(idx, 0);
}

TEST(FindAnchorNodeIndex, FindSecondHeading)
{
    auto nodes = ParseMarkdown(L"# First\n\nParagraph\n\n## Second").nodes;
    int idx = FindAnchorNodeIndexLinear(nodes, L"second");
    EXPECT_GE(idx, 0);
    EXPECT_EQ(nodes[idx].GetText(), L"Second");
}

TEST(FindAnchorNodeIndex, CaseInsensitiveSearch)
{
    auto nodes = ParseMarkdown(L"# Hello World").nodes;
    // アンカーは"hello-world"、大文字で検索
    int idx = FindAnchorNodeIndexLinear(nodes, L"Hello-World");
    EXPECT_EQ(idx, 0);
}

TEST(FindAnchorNodeIndex, NotFound)
{
    auto nodes = ParseMarkdown(L"# Title").nodes;
    EXPECT_EQ(FindAnchorNodeIndexLinear(nodes, L"nonexistent"), -1);
}

TEST(FindAnchorNodeIndex, CjkAnchor)
{
    auto nodes = ParseMarkdown(L"## コードブロック").nodes;
    int idx = FindAnchorNodeIndexLinear(nodes, L"コードブロック");
    EXPECT_EQ(idx, 0);
}

TEST(FindAnchorNodeIndex, SkipsNonHeadings)
{
    auto nodes = ParseMarkdown(L"Paragraph\n\n# Heading").nodes;
    int idx = FindAnchorNodeIndexLinear(nodes, L"heading");
    EXPECT_GE(idx, 0);
    EXPECT_EQ(nodes[idx].type, NodeType::Heading);
}

// ============================================================
// FindWordBoundaries
// ============================================================

TEST(FindWordBoundaries, EmptyText)
{
    auto result = FindWordBoundaries(L"", 0);
    EXPECT_FALSE(result.found);
}

TEST(FindWordBoundaries, SingleWord)
{
    auto result = FindWordBoundaries(L"hello", 2);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 5u);
}

TEST(FindWordBoundaries, WordAtStart)
{
    auto result = FindWordBoundaries(L"hello world", 0);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 5u);
}

TEST(FindWordBoundaries, WordAtEnd)
{
    auto result = FindWordBoundaries(L"hello world", 6);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 6u);
    EXPECT_EQ(result.end, 11u);
}

TEST(FindWordBoundaries, PositionOnSpace)
{
    auto result = FindWordBoundaries(L"hello world", 5);
    EXPECT_FALSE(result.found);
}

TEST(FindWordBoundaries, PositionOnPunctuation)
{
    auto result = FindWordBoundaries(L"hello, world", 5);
    EXPECT_FALSE(result.found);
}

TEST(FindWordBoundaries, WordWithUnderscore)
{
    auto result = FindWordBoundaries(L"my_variable = 1", 3);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 11u);
}

TEST(FindWordBoundaries, WordWithNumbers)
{
    auto result = FindWordBoundaries(L"var123 = x", 3);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 6u);
}

TEST(FindWordBoundaries, PositionBeyondEnd)
{
    auto result = FindWordBoundaries(L"hello", 100);
    // 最後の文字にクランプされるべき
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 5u);
}

TEST(FindWordBoundaries, NonAlphanumericAtPosition)
{
    // CJK文字はIsCharAlphaNumericWで単語文字として扱われないため
    // ダブルクリックでは選択されないべき
    auto result = FindWordBoundaries(L"テスト test", 0);
    EXPECT_FALSE(result.found);
}

TEST(FindWordBoundaries, AsciiWordAfterCjk)
{
    // CJKの後のASCII単語をクリックすると動作するべき
    auto result = FindWordBoundaries(L"テスト test", 4);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 4u);
    EXPECT_EQ(result.end, 8u);
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
    table_node.SetText(L"A\tB\n1\t2");

    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(table_node));
    TextSelection sel;
    sel.start_node = 0;
    sel.start_pos = 0;
    sel.end_node = 0;
    sel.end_pos = 3;
    sel.active = true;

    auto result = ExtractSelectedText(nodes, sel);
    EXPECT_EQ(result, L"A\tB");
}

TEST(ExtractSelectedText, StartNodeOutOfRange)
{
    std::pmr::vector<Node> nodes;
    Node n;
    n.SetText(L"hello");
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
    n.SetText(L"hello");
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
    node.SetText(L"link1 link2");

    node.ensure_link_urls().emplace_back(L"https://a.com");
    node.ensure_link_urls().emplace_back(L"https://b.com");

    TextRun r1;
    r1.start = 0; r1.length = 5;
    r1.link_url_index = 0;

    TextRun r2;
    r2.start = 6; r2.length = 5;
    r2.link_url_index = 1;

    node.runs = { r1, r2 };

    auto result1 = FindLinkAtPosition(node, 2);
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(*result1, L"https://a.com");

    auto result2 = FindLinkAtPosition(node, 8);
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(*result2, L"https://b.com");

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

    TableRow header;
    TableCell h0; h0.text = L"Name"; h0.is_header = true;
    TextRun h0r; h0r.start = 0; h0r.length = 4;
    h0.runs.emplace_back(h0r);
    header.cells.emplace_back(h0);

    TableCell h1; h1.text = L"URL"; h1.is_header = true;
    TextRun h1r; h1r.start = 0; h1r.length = 3;
    h1.runs.emplace_back(h1r);
    header.cells.emplace_back(h1);
    node.ensure_table();
    node.table_rows().emplace_back(header);

    TableRow data;
    TableCell d0; d0.text = L"foo";
    TextRun d0r; d0r.start = 0; d0r.length = 3;
    d0.runs.emplace_back(d0r);
    data.cells.emplace_back(d0);

    TableCell d1; d1.text = L"bar";
    TextRun d1r; d1r.start = 0; d1r.length = 3;
    d1r.link_url_index = static_cast<int16_t>(node.view_link_urls().size());
    node.ensure_link_urls().emplace_back(L"https://example.com");
    d1.runs.emplace_back(d1r);
    data.cells.emplace_back(d1);
    node.table_rows().emplace_back(data);

    // 線形化: "Name\tURL\nfoo\tbar"
    //          0123 4567 8901 2345
    // "Name" = オフセット 0-3, タブ 4, "URL" = 5-7, 改行 8
    // "foo" = オフセット 9-11, タブ 12, "bar" = 13-15
    node.SetText(L"Name\tURL\nfoo\tbar");

    // "bar"内の位置（オフセット13）でリンクが見つかるべき
    auto result = FindLinkAtPosition(node, 13);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, L"https://example.com");

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
        L"| Text | Link |\n"
        L"|------|------|\n"
        L"| hello | [click](https://example.com) |"
    ).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Table);

    // セル内にリンクのrunが存在することを確認
    ASSERT_GE(nodes[0].table_rows().size(), 2u);
    const auto& data_row = nodes[0].table_rows()[1];
    ASSERT_GE(data_row.cells.size(), 2u);
    bool has_link = false;
    for (const auto& run : data_row.cells[1].runs) {
        if (run.has_link()) {
            EXPECT_EQ(nodes[0].view_link_urls()[run.link_url_index], L"https://example.com");
            has_link = true;
        }
    }
    EXPECT_TRUE(has_link);
}

TEST(FindLinkAtPosition, TableCellInternalLink)
{
    Node node;
    node.type = NodeType::Table;

    TableRow row;
    TableCell cell; cell.text = L"section";
    TextRun r; r.start = 0; r.length = 7;
    r.link_url_index = static_cast<int16_t>(node.view_link_urls().size());
    node.ensure_link_urls().emplace_back(L"#my-section");
    cell.runs.emplace_back(r);
    row.cells.emplace_back(cell);
    node.ensure_table();
    node.table_rows().emplace_back(row);

    node.SetText(L"section");

    auto result = FindLinkAtPosition(node, 3);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, L"#my-section");
}

TEST(FindLinkAtPosition, TablePositionOnSeparator)
{
    // タブ/改行区切り上の位置ではリンクを返さないべき
    Node node;
    node.type = NodeType::Table;

    TableRow row;
    TableCell c0; c0.text = L"A";
    TextRun r0; r0.start = 0; r0.length = 1;
    c0.runs.emplace_back(r0);
    row.cells.emplace_back(c0);

    TableCell c1; c1.text = L"B";
    TextRun r1; r1.start = 0; r1.length = 1;
    r1.link_url_index = static_cast<int16_t>(node.view_link_urls().size());
    node.ensure_link_urls().emplace_back(L"https://b.com");
    c1.runs.emplace_back(r1);
    row.cells.emplace_back(c1);
    node.ensure_table();
    node.table_rows().emplace_back(row);

    // 線形化: "A\tB" → オフセット 0=A, 1=タブ, 2=B
    node.SetText(L"A\tB");

    // タブ区切り（オフセット1）はどのセルにもマッチしないべき
    auto result = FindLinkAtPosition(node, 1);
    EXPECT_FALSE(result.has_value());

    // "B"（オフセット2）でリンクが見つかるべき
    auto link = FindLinkAtPosition(node, 2);
    ASSERT_TRUE(link.has_value());
    EXPECT_EQ(*link, L"https://b.com");
}

// ---- FindAnchorNodeIndex 追加テスト ----

TEST(FindAnchorNodeIndex, DuplicateAnchors)
{
    std::pmr::vector<Node> nodes;

    Node h1;
    h1.type = NodeType::Heading;
    h1.ensure_heading();
    h1.heading_data()->anchor_id = L"title";
    nodes.emplace_back(std::move(h1));

    Node h2;
    h2.type = NodeType::Heading;
    h2.ensure_heading();
    h2.heading_data()->anchor_id = L"title-1";
    nodes.emplace_back(std::move(h2));

    // 最初のマッチが優先される
    EXPECT_EQ(FindAnchorNodeIndexLinear(nodes, L"title"), 0);
    EXPECT_EQ(FindAnchorNodeIndexLinear(nodes, L"title-1"), 1);
}

// ---- FindWordBoundaries 追加テスト ----

TEST(FindWordBoundaries, SingleCharWord)
{
    auto result = FindWordBoundaries(L"a", 0);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 1u);
}

TEST(FindWordBoundaries, AllSpaces)
{
    auto result = FindWordBoundaries(L"   ", 1);
    EXPECT_FALSE(result.found);
}

TEST(FindWordBoundaries, MixedPunctuationAndWords)
{
    auto result = FindWordBoundaries(L"(hello)", 3);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 1u);
    EXPECT_EQ(result.end, 6u);
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
    EXPECT_EQ(FindFirstDifference(L"hello", L"hello"), std::wstring_view::npos);
}

TEST(FindFirstDifference, BothEmpty)
{
    EXPECT_EQ(FindFirstDifference(L"", L""), std::wstring_view::npos);
}

TEST(FindFirstDifference, DifferentFirstUnit)
{
    EXPECT_EQ(FindFirstDifference(L"abc", L"xbc"), 0u);
}

TEST(FindFirstDifference, DifferentMiddle)
{
    EXPECT_EQ(FindFirstDifference(L"abcdef", L"abcXef"), 3u);
}

TEST(FindFirstDifference, DifferentLastUnit)
{
    EXPECT_EQ(FindFirstDifference(L"abc", L"abX"), 2u);
}

TEST(FindFirstDifference, NewLongerThanOld)
{
    EXPECT_EQ(FindFirstDifference(L"abc", L"abcdef"), 3u);
}

TEST(FindFirstDifference, OldLongerThanNew)
{
    EXPECT_EQ(FindFirstDifference(L"abcdef", L"abc"), 3u);
}

TEST(FindFirstDifference, EmptyOld)
{
    EXPECT_EQ(FindFirstDifference(L"", L"new"), 0u);
}

TEST(FindFirstDifference, EmptyNew)
{
    EXPECT_EQ(FindFirstDifference(L"old", L""), 0u);
}

TEST(FindFirstDifference, CjkContent)
{
    // BMP 内の CJK は 1 wchar_t/char。"う"(U+3046)/"え"(U+3048) は 1 unit 違い。
    std::wstring a = L"あいう";
    std::wstring b = L"あいえ";
    size_t diff = FindFirstDifference(a, b);
    EXPECT_EQ(diff, 2u); // 3 文字目で差分（インデックス 2）
}

// ============================================================
// AnalyzeReloadDiff
// ============================================================

TEST(AnalyzeReloadDiff, IdenticalContentReturnsNoChange)
{
    const auto d = AnalyzeReloadDiff(L"hello world", L"hello world");
    EXPECT_EQ(d.op, ReloadOp::NoChange);
    EXPECT_EQ(d.diff_pos, std::wstring_view::npos);
}

TEST(AnalyzeReloadDiff, BothEmptyReturnsNoChange)
{
    const auto d = AnalyzeReloadDiff(L"", L"");
    EXPECT_EQ(d.op, ReloadOp::NoChange);
    EXPECT_EQ(d.diff_pos, std::wstring_view::npos);
}

TEST(AnalyzeReloadDiff, AppendedSuffixIsPrefixGrowth)
{
    // 末尾に追記 → prefix-only growth（スクロール維持）
    const auto d = AnalyzeReloadDiff(L"abc", L"abcdef");
    EXPECT_EQ(d.op, ReloadOp::PrefixGrowth);
    EXPECT_EQ(d.diff_pos, 3u);
}

TEST(AnalyzeReloadDiff, EmptyToContentIsPrefixGrowth)
{
    // 空ファイル → 何か書いた。prefix-only growth として扱う。
    const auto d = AnalyzeReloadDiff(L"", L"new content");
    EXPECT_EQ(d.op, ReloadOp::PrefixGrowth);
    EXPECT_EQ(d.diff_pos, 0u);
}

TEST(AnalyzeReloadDiff, TruncatedSuffixIsDeferPrefixShrink)
{
    // 末尾が消えた = truncate。エディタの truncate→rewrite 前半の可能性があるため defer。
    const auto d = AnalyzeReloadDiff(L"abcdef", L"abc");
    EXPECT_EQ(d.op, ReloadOp::DeferPrefixShrink);
    EXPECT_EQ(d.diff_pos, 3u);
}

TEST(AnalyzeReloadDiff, ContentToEmptyIsDeferPrefixShrink)
{
    // 全消去も truncate → rewrite の前半とみなして defer する
    const auto d = AnalyzeReloadDiff(L"old content", L"");
    EXPECT_EQ(d.op, ReloadOp::DeferPrefixShrink);
    EXPECT_EQ(d.diff_pos, 0u);
}

TEST(AnalyzeReloadDiff, MiddleChangeIsFullReload)
{
    // 中間で変化した。prefix-only ではないので全体リロード。
    const auto d = AnalyzeReloadDiff(L"abcdef", L"abcXef");
    EXPECT_EQ(d.op, ReloadOp::FullReload);
    EXPECT_EQ(d.diff_pos, 3u);
}

TEST(AnalyzeReloadDiff, FirstUnitChangeIsFullReload)
{
    // 先頭で差分があれば必ず FullReload（同一長さなので prefix-only にならない）。
    const auto d = AnalyzeReloadDiff(L"abc", L"Xbc");
    EXPECT_EQ(d.op, ReloadOp::FullReload);
    EXPECT_EQ(d.diff_pos, 0u);
}

TEST(AnalyzeReloadDiff, LengthChangedWithMiddleDiffIsFullReload)
{
    // 途中で差分があり、かつ長さも変わる → prefix-only ではなく FullReload。
    const auto d = AnalyzeReloadDiff(L"abcdef", L"abcYYYz");
    EXPECT_EQ(d.op, ReloadOp::FullReload);
    EXPECT_EQ(d.diff_pos, 3u);
}

TEST(AnalyzeReloadDiff, CjkSuffixAppendedIsPrefixGrowth)
{
    // CJK 末尾追記も prefix-only growth として扱える
    const std::wstring old_text = L"あいう";
    const std::wstring new_text = L"あいうえお";
    const auto d = AnalyzeReloadDiff(old_text, new_text);
    EXPECT_EQ(d.op, ReloadOp::PrefixGrowth);
    EXPECT_EQ(d.diff_pos, 3u); // "あいう" = 3 wchar_t
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
    auto nodes = ParseMarkdown(L"# Title\n\nParagraph\n\n## Section").nodes;
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
    auto nodes = ParseMarkdown(L"AAA\n\n---\n\nBBB").nodes;
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
static int SimulateEditAndFindNode(std::wstring_view old_md, std::wstring_view new_md)
{
    size_t diff_pos = FindFirstDifference(old_md, new_md);
    if (diff_pos == std::wstring_view::npos) {
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
    std::wstring old_md = L"First\n\nSecond\n\nThird";
    std::wstring new_md = L"First\n\nModified\n\nThird";
    int node = SimulateEditAndFindNode(old_md, new_md);
    auto nodes = ParseMarkdown(new_md).nodes;
    EXPECT_EQ(node, 1); // 2番目の段落
    EXPECT_EQ(nodes[node].GetText(), L"Modified");
}

TEST(DiffToNode, EditFirstParagraph)
{
    std::wstring old_md = L"Hello\n\nWorld";
    std::wstring new_md = L"Changed\n\nWorld";
    int node = SimulateEditAndFindNode(old_md, new_md);
    EXPECT_EQ(node, 0);
}

TEST(DiffToNode, EditLastParagraph)
{
    std::wstring old_md = L"First\n\nSecond\n\nThird";
    std::wstring new_md = L"First\n\nSecond\n\nChanged";
    int node = SimulateEditAndFindNode(old_md, new_md);
    EXPECT_EQ(node, 2); // 最後の段落
}

TEST(DiffToNode, InsertNewParagraph)
{
    // 段落を挿入
    std::wstring old_md = L"Before\n\nAfter";
    std::wstring new_md = L"Before\n\nInserted\n\nAfter";
    int node = SimulateEditAndFindNode(old_md, new_md);
    auto nodes = ParseMarkdown(new_md).nodes;
    ASSERT_GE(node, 0);
    // 挿入位置のノード（"Inserted" または "Before"の次）
    EXPECT_EQ(nodes[node].GetText(), L"Inserted");
}

TEST(DiffToNode, DeleteParagraph)
{
    // 段落を削除
    std::wstring old_md = L"First\n\nRemoveMe\n\nLast";
    std::wstring new_md = L"First\n\nLast";
    int node = SimulateEditAndFindNode(old_md, new_md);
    ASSERT_GE(node, 0);
    // diff_pos=7（"RemoveMe" vs "Last"の開始位置）→ "Last"(offset=7)か"First"
    auto nodes = ParseMarkdown(new_md).nodes;
    EXPECT_LE(node, 1); // "First" or "Last"
}

TEST(DiffToNode, AppendToEnd)
{
    std::wstring old_md = L"Existing";
    std::wstring new_md = L"Existing\n\nAppended";
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
    std::wstring old_md = L"text\n\n```\nold code\n```\n\nend";
    std::wstring new_md = L"text\n\n```\nnew code\n```\n\nend";
    int node = SimulateEditAndFindNode(old_md, new_md);
    auto nodes = ParseMarkdown(new_md).nodes;
    ASSERT_GE(node, 0);
    EXPECT_EQ(nodes[node].type, NodeType::CodeBlock);
}

TEST(DiffToNode, EditInListItem)
{
    // リストアイテムの編集
    std::wstring old_md = L"- first\n- second\n- third";
    std::wstring new_md = L"- first\n- changed\n- third";
    int node = SimulateEditAndFindNode(old_md, new_md);
    auto nodes = ParseMarkdown(new_md).nodes;
    ASSERT_GE(node, 0);
    EXPECT_EQ(nodes[node].GetText(), L"changed");
}

TEST(DiffToNode, EditHeading)
{
    // 見出しテキストの編集
    std::wstring old_md = L"# Old Title\n\nBody";
    std::wstring new_md = L"# New Title\n\nBody";
    int node = SimulateEditAndFindNode(old_md, new_md);
    EXPECT_EQ(node, 0); // 見出しノード
}

TEST(DiffToNode, NoChange)
{
    std::wstring md = L"Same content";
    EXPECT_EQ(SimulateEditAndFindNode(md, md), -1);
}

TEST(DiffToNode, EditInBlockQuote)
{
    std::wstring old_md = L"normal\n\n> old quote\n\nafter";
    std::wstring new_md = L"normal\n\n> new quote\n\nafter";
    int node = SimulateEditAndFindNode(old_md, new_md);
    auto nodes = ParseMarkdown(new_md).nodes;
    ASSERT_GE(node, 0);
    EXPECT_EQ(nodes[node].type, NodeType::BlockQuote);
}

TEST(DiffToNode, EditWithJapanese)
{
    // 日本語テキストの編集
    std::wstring old_md = L"# はじめに\n\n旧テキスト\n\nおわり";
    std::wstring new_md = L"# はじめに\n\n新テキスト\n\nおわり";
    int node = SimulateEditAndFindNode(old_md, new_md);
    auto nodes = ParseMarkdown(new_md).nodes;
    ASSERT_GE(node, 0);
    // "# はじめに\n\n" = 2 + 15 + 2 = 19バイト
    // diff_pos は "新" vs "旧" の位置
    EXPECT_EQ(node, 1); // 2番目の段落
}

TEST(DiffToNode, EditInTable)
{
    std::wstring old_md =
        L"| A | B |\n"
        L"|---|---|\n"
        L"| 1 | 2 |";
    std::wstring new_md =
        L"| A | B |\n"
        L"|---|---|\n"
        L"| X | 2 |";
    int node = SimulateEditAndFindNode(old_md, new_md);
    auto nodes = ParseMarkdown(new_md).nodes;
    ASSERT_GE(node, 0);
    EXPECT_EQ(nodes[node].type, NodeType::Table);
}

TEST(DiffToNode, LargeDocumentMiddleEdit)
{
    // 多数のノードを持つ文書の中間を編集
    std::wstring old_md, new_md;
    for (int i = 0; i < 100; ++i) {
        old_md += L"Paragraph " + std::to_wstring(i) + L"\n\n";
        if (i == 50) {
            new_md += L"CHANGED paragraph 50\n\n";
        }
        else {
            new_md += L"Paragraph " + std::to_wstring(i) + L"\n\n";
        }
    }
    int node = SimulateEditAndFindNode(old_md, new_md);
    auto nodes = ParseMarkdown(new_md).nodes;
    ASSERT_GE(node, 0);
    EXPECT_EQ(nodes[node].GetText(), L"CHANGED paragraph 50");
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
    std::wstring_view old_text = L"Hello World";
    std::wstring_view new_text = L"Hello World, more text";
    size_t diff = FindFirstDifference(old_text, new_text);
    EXPECT_TRUE(IsPrefixOnlyDiff(diff, old_text.size(), new_text.size()));

    // 内容が異なる場合
    std::wstring_view old_text2 = L"Hello World";
    std::wstring_view new_text2 = L"Hello Xxxxx";
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
    EXPECT_FLOAT_EQ(CalcScrollYForDiff(nodes, cache, L"content", 0, 500.0f, 42.0f), 42.0f);
}

TEST(CalcScrollYForDiff, FallbackWhenNodeNotFound)
{
    // すべてのノードの source_offset が diff_pos より大きい
    auto nodes = MakeNodes(3, 100);
    nodes[0].source_offset = 50;
    auto cache = MakeUniformCache(3);
    // diff_pos=10 < 全ノードの最小 offset(50) → -1 → fallback
    EXPECT_FLOAT_EQ(CalcScrollYForDiff(nodes, cache, L"content", 10, 500.0f, 99.0f), 99.0f);
}

TEST(CalcScrollYForDiff, ScrollsToNodeStartWithMargin)
{
    // 3ノード: offset=0,100,200 / y=0,100,200 / height=100
    auto nodes = MakeNodes(3, 100);
    auto cache = MakeUniformCache(3, 100.0f);
    std::wstring content(300, L'x');

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
    std::wstring content(200, L'x');

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
    std::wstring content(200, L'x');

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
    std::wstring content(300, L'x');

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
    std::wstring content(100, L'x');

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
    EXPECT_FLOAT_EQ(CalcScrollYForDiff(nodes, cache, std::wstring(500, L'x'), 400, 500.0f, 77.0f), 77.0f);
}

TEST(CalcScrollYForDiff, ParsedMarkdownIntegration)
{
    // 実際のMarkdownをパースして差分スクロール位置を計算する統合テスト
    std::wstring md = L"# Title\n\nFirst paragraph\n\nSecond paragraph\n\nThird paragraph";
    auto nodes = ParseMarkdown(std::wstring_view{ md }).nodes;
    ASSERT_GE(nodes.size(), 4u);

    // ノード高さを設定
    LayoutCache cache;
    cache.Resize(nodes.size());
    float y = 0.0f;
    for (size_t i = 0; i < nodes.size(); ++i) {
        cache[i].y_position = y;
        cache[i].height = 50.0f;
        y += 50.0f;
    }

    // "Second paragraph" の先頭で diff
    size_t diff_pos = static_cast<size_t>(md.find(L"Second"));
    ASSERT_NE(diff_pos, std::wstring::npos);

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
    const std::wstring old_md = L"para_one\n\npara_two\n\npara_three";
    // 新 doc: 末尾に 1 段落追記
    const std::wstring new_md = old_md + L"\n\npara_four_added";

    // AnalyzeReloadDiff で PrefixGrowth と判定されること
    const auto decision = AnalyzeReloadDiff(old_md, new_md);
    ASSERT_EQ(decision.op, ReloadOp::PrefixGrowth);
    ASSERT_EQ(decision.diff_pos, old_md.size());

    // 新 doc を pipeline で扱うイメージで cache を構築
    auto nodes = ParseMarkdown(std::wstring_view{ new_md }).nodes;
    ASSERT_GE(nodes.size(), 4u);

    LayoutCache cache;
    cache.Resize(nodes.size());
    float y = 0.0f;
    for (size_t i = 0; i < nodes.size(); ++i) {
        cache[i].y_position = y;
        cache[i].height = 100.0f;
        y += 100.0f;
    }
    const float last_old_node_y = cache[nodes.size() - 2].y_position;
    const float appended_node_y = cache[nodes.size() - 1].y_position;

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
// ToLowerAscii
// ============================================================

TEST(ToLowerAscii, AllUppercase)
{
    EXPECT_EQ(ToLowerAscii(L"HELLO"), L"hello");
}

TEST(ToLowerAscii, AllLowercase)
{
    EXPECT_EQ(ToLowerAscii(L"hello"), L"hello");
}

TEST(ToLowerAscii, MixedCase)
{
    EXPECT_EQ(ToLowerAscii(L"HeLLo WoRLd"), L"hello world");
}

TEST(ToLowerAscii, Empty)
{
    EXPECT_TRUE(ToLowerAscii(L"").empty());
}

TEST(ToLowerAscii, NonAsciiUnchanged)
{
    EXPECT_EQ(ToLowerAscii(L"日本語"), L"日本語");
}

TEST(ToLowerAscii, DigitsAndSymbols)
{
    EXPECT_EQ(ToLowerAscii(L"ABC-123_XYZ"), L"abc-123_xyz");
}

TEST(ToLowerAscii, BoundaryChars)
{
    // A(0x41)の直前@(0x40)、Z(0x5A)の直後[(0x5B)は変換されないこと
    EXPECT_EQ(ToLowerAscii(L"@A[Z"), L"@a[z");
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
