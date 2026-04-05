#include <gtest/gtest.h>
#include <memory_resource>
#include <string_view>
#include "document_utils.h"
#include "parser.h"

// ============================================================
// ExtractSelectedText
// ============================================================

TEST(ExtractSelectedText, InactiveSelectionReturnsEmpty)
{
    auto nodes = ParseMarkdown("Hello world");
    TextSelection sel;
    sel.active = false;
    EXPECT_TRUE(ExtractSelectedText(nodes, sel).empty());
}

TEST(ExtractSelectedText, SingleNodeFullSelection)
{
    auto nodes = ParseMarkdown("Hello world");
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    EXPECT_EQ(ExtractSelectedText(nodes, sel), L"Hello world");
}

TEST(ExtractSelectedText, SingleNodePartialSelection)
{
    auto nodes = ParseMarkdown("Hello world");
    auto sel = TextSelection::MakeOrdered(0, 0, 0, 5);
    EXPECT_EQ(ExtractSelectedText(nodes, sel), L"Hello");
}

TEST(ExtractSelectedText, SingleNodeMiddleSelection)
{
    auto nodes = ParseMarkdown("Hello world");
    auto sel = TextSelection::MakeOrdered(0, 6, 0, 11);
    EXPECT_EQ(ExtractSelectedText(nodes, sel), L"world");
}

TEST(ExtractSelectedText, MultipleNodesFullSelection)
{
    auto nodes = ParseMarkdown("First\n\nSecond\n\nThird");
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
    auto nodes = ParseMarkdown("First\n\nSecond\n\nThird");
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
    auto nodes = ParseMarkdown("A\n\nB");
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
    auto nodes = ParseMarkdown("Short");
    auto sel = TextSelection::MakeOrdered(0, 0, 0, 1000);
    // end_posがテキストサイズを超える場合はクランプされるべき
    EXPECT_EQ(ExtractSelectedText(nodes, sel), L"Short");
}

TEST(ExtractSelectedText, JapaneseText)
{
    auto nodes = ParseMarkdown("日本語テスト");
    auto sel = TextSelection::MakeOrdered(
        0, 0, 0, static_cast<uint32_t>(nodes[0].GetText().size()));
    EXPECT_EQ(ExtractSelectedText(nodes, sel), L"日本語テスト");
}

// ============================================================
// FindLinkAtPosition
// ============================================================

TEST(FindLinkAtPosition, NoLinks)
{
    auto nodes = ParseMarkdown("plain text");
    ASSERT_EQ(nodes.size(), 1u);
    auto result = FindLinkAtPosition(nodes[0], 0);
    EXPECT_FALSE(result.has_value());
}

TEST(FindLinkAtPosition, LinkFound)
{
    auto nodes = ParseMarkdown("[click](https://example.com)");
    ASSERT_EQ(nodes.size(), 1u);
    // リンクテキスト内の位置
    auto result = FindLinkAtPosition(nodes[0], 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), L"https://example.com");
}

TEST(FindLinkAtPosition, PositionOutsideLink)
{
    auto nodes = ParseMarkdown("before [link](https://example.com) after");
    ASSERT_EQ(nodes.size(), 1u);
    // "before"テキスト内の位置（リンクではないはず）
    auto result = FindLinkAtPosition(nodes[0], 0);
    EXPECT_FALSE(result.has_value());
}

TEST(FindLinkAtPosition, InternalLink)
{
    auto nodes = ParseMarkdown("[section](#my-section)");
    ASSERT_EQ(nodes.size(), 1u);
    auto result = FindLinkAtPosition(nodes[0], 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), L"#my-section");
}

TEST(FindLinkAtPosition, PositionAtLinkBoundary)
{
    auto nodes = ParseMarkdown("[link](https://example.com)");
    ASSERT_EQ(nodes.size(), 1u);
    // リンクの最後の文字の位置
    uint32_t last_pos = static_cast<uint32_t>(nodes[0].GetText().size()) - 1;
    auto result = FindLinkAtPosition(nodes[0], last_pos);
    ASSERT_TRUE(result.has_value());
}

TEST(FindLinkAtPosition, PositionBeyondText)
{
    auto nodes = ParseMarkdown("[link](https://example.com)");
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
    EXPECT_EQ(FindAnchorNodeIndex(nodes, L"test"), -1);
}

TEST(FindAnchorNodeIndex, EmptyAnchor)
{
    auto nodes = ParseMarkdown("# Title");
    EXPECT_EQ(FindAnchorNodeIndex(nodes, L""), -1);
}

TEST(FindAnchorNodeIndex, FindExistingAnchor)
{
    auto nodes = ParseMarkdown("# Title\n\nParagraph\n\n## Section");
    ASSERT_GE(nodes.size(), 3u);
    int idx = FindAnchorNodeIndex(nodes, L"title");
    EXPECT_EQ(idx, 0);
}

TEST(FindAnchorNodeIndex, FindSecondHeading)
{
    auto nodes = ParseMarkdown("# First\n\nParagraph\n\n## Second");
    int idx = FindAnchorNodeIndex(nodes, L"second");
    EXPECT_GE(idx, 0);
    EXPECT_EQ(nodes[idx].GetText(), L"Second");
}

TEST(FindAnchorNodeIndex, CaseInsensitiveSearch)
{
    auto nodes = ParseMarkdown("# Hello World");
    // アンカーは"hello-world"、大文字で検索
    int idx = FindAnchorNodeIndex(nodes, L"Hello-World");
    EXPECT_EQ(idx, 0);
}

TEST(FindAnchorNodeIndex, NotFound)
{
    auto nodes = ParseMarkdown("# Title");
    EXPECT_EQ(FindAnchorNodeIndex(nodes, L"nonexistent"), -1);
}

TEST(FindAnchorNodeIndex, CjkAnchor)
{
    auto nodes = ParseMarkdown("## コードブロック");
    int idx = FindAnchorNodeIndex(nodes, L"コードブロック");
    EXPECT_EQ(idx, 0);
}

TEST(FindAnchorNodeIndex, SkipsNonHeadings)
{
    auto nodes = ParseMarkdown("Paragraph\n\n# Heading");
    int idx = FindAnchorNodeIndex(nodes, L"heading");
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

    node.link_urls.emplace_back(L"https://a.com");
    node.link_urls.emplace_back(L"https://b.com");

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
    d1r.link_url_index = static_cast<int16_t>(node.link_urls.size());
    node.link_urls.emplace_back(L"https://example.com");
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
        "| Text | Link |\n"
        "|------|------|\n"
        "| hello | [click](https://example.com) |"
    );
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Table);

    // セル内にリンクのrunが存在することを確認
    ASSERT_GE(nodes[0].table_rows().size(), 2u);
    const auto& data_row = nodes[0].table_rows()[1];
    ASSERT_GE(data_row.cells.size(), 2u);
    bool has_link = false;
    for (const auto& run : data_row.cells[1].runs) {
        if (run.has_link()) {
            EXPECT_EQ(nodes[0].link_urls[run.link_url_index], L"https://example.com");
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
    r.link_url_index = static_cast<int16_t>(node.link_urls.size());
    node.link_urls.emplace_back(L"#my-section");
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
    r1.link_url_index = static_cast<int16_t>(node.link_urls.size());
    node.link_urls.emplace_back(L"https://b.com");
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
    h1.anchor_id = L"title";
    nodes.emplace_back(std::move(h1));

    Node h2;
    h2.type = NodeType::Heading;
    h2.anchor_id = L"title-1";
    nodes.emplace_back(std::move(h2));

    // 最初のマッチが優先される
    EXPECT_EQ(FindAnchorNodeIndex(nodes, L"title"), 0);
    EXPECT_EQ(FindAnchorNodeIndex(nodes, L"title-1"), 1);
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
// FindFirstDifference
// ============================================================

TEST(FindFirstDifference, IdenticalStrings)
{
    EXPECT_EQ(FindFirstDifference("hello", "hello"), std::string_view::npos);
}

TEST(FindFirstDifference, BothEmpty)
{
    EXPECT_EQ(FindFirstDifference("", ""), std::string_view::npos);
}

TEST(FindFirstDifference, DifferentFirstByte)
{
    EXPECT_EQ(FindFirstDifference("abc", "xbc"), 0u);
}

TEST(FindFirstDifference, DifferentMiddle)
{
    EXPECT_EQ(FindFirstDifference("abcdef", "abcXef"), 3u);
}

TEST(FindFirstDifference, DifferentLastByte)
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

TEST(FindFirstDifference, Utf8Content)
{
    // UTF-8: "う"=E3 81 86, "え"=E3 81 88 → 先頭2バイト共通、3バイト目で差分
    std::string a = "あいう";
    std::string b = "あいえ";
    size_t diff = FindFirstDifference(a, b);
    EXPECT_EQ(diff, 8u); // "あいう"/"あいえ" の最後のバイトで差分
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
    nodes[1].source_offset = UINT32_MAX; // 未設定（HorizontalRule等）
    nodes[2].source_offset = 20;
    // UINT32_MAX は常に diff_offset 以上にならない（UINT32_MAX <= diff_offset は通常 false）
    // → ノード0を返す
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 10), 0);
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 20), 2);
}

TEST(FindNodeBySourceOffset, ParsedMarkdown)
{
    auto nodes = ParseMarkdown("# Title\n\nParagraph\n\n## Section");
    ASSERT_GE(nodes.size(), 3u);
    // 各ノードが有効な source_offset を持つ
    for (const auto& n : nodes) {
        EXPECT_NE(n.source_offset, UINT32_MAX);
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
    // 全ノードが UINT32_MAX（テキストなし）→ 該当なし
    std::pmr::vector<Node> nodes(3);
    nodes[0].source_offset = UINT32_MAX;
    nodes[1].source_offset = UINT32_MAX;
    nodes[2].source_offset = UINT32_MAX;
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 50), -1);
}

TEST(FindNodeBySourceOffset, MixedWithHorizontalRules)
{
    // パース結果で HorizontalRule が混在するケース
    auto nodes = ParseMarkdown("AAA\n\n---\n\nBBB");
    ASSERT_GE(nodes.size(), 3u);
    // "AAA" offset=0, "---" offset=UINT32_MAX, "BBB" offset=10
    EXPECT_EQ(nodes[0].source_offset, 0u);
    EXPECT_EQ(nodes[1].source_offset, UINT32_MAX);

    // offset=5（"---"のソース位置付近）→ AAA(offset=0)を返す（HRはスキップ）
    EXPECT_EQ(FindNodeBySourceOffset(nodes, 5), 0);
    // offset=10（BBBのソース位置）→ BBBを返す
    int bbb_idx = FindNodeBySourceOffset(nodes, 10);
    EXPECT_EQ(bbb_idx, 2);
}

// ============================================================
// 統合テスト: diff検出 → ノード特定
// ============================================================

// ヘルパー: old→new の編集をシミュレートし、変更箇所のノードを特定する
static int SimulateEditAndFindNode(std::string_view old_md, std::string_view new_md)
{
    size_t diff_pos = FindFirstDifference(old_md, new_md);
    if (diff_pos == std::string_view::npos) {
        return -1;
    }
    auto nodes = ParseMarkdown(new_md);
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
    auto nodes = ParseMarkdown(new_md);
    EXPECT_EQ(node, 1); // 2番目の段落
    EXPECT_EQ(nodes[node].GetText(), L"Modified");
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
    auto nodes = ParseMarkdown(new_md);
    ASSERT_GE(node, 0);
    // 挿入位置のノード（"Inserted" または "Before"の次）
    EXPECT_EQ(nodes[node].GetText(), L"Inserted");
}

TEST(DiffToNode, DeleteParagraph)
{
    // 段落を削除
    std::string old_md = "First\n\nRemoveMe\n\nLast";
    std::string new_md = "First\n\nLast";
    int node = SimulateEditAndFindNode(old_md, new_md);
    ASSERT_GE(node, 0);
    // diff_pos=7（"RemoveMe" vs "Last"の開始位置）→ "Last"(offset=7)か"First"
    auto nodes = ParseMarkdown(new_md);
    EXPECT_LE(node, 1); // "First" or "Last"
}

TEST(DiffToNode, AppendToEnd)
{
    std::string old_md = "Existing";
    std::string new_md = "Existing\n\nAppended";
    int node = SimulateEditAndFindNode(old_md, new_md);
    auto nodes = ParseMarkdown(new_md);
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
    auto nodes = ParseMarkdown(new_md);
    ASSERT_GE(node, 0);
    EXPECT_EQ(nodes[node].type, NodeType::CodeBlock);
}

TEST(DiffToNode, EditInListItem)
{
    // リストアイテムの編集
    std::string old_md = "- first\n- second\n- third";
    std::string new_md = "- first\n- changed\n- third";
    int node = SimulateEditAndFindNode(old_md, new_md);
    auto nodes = ParseMarkdown(new_md);
    ASSERT_GE(node, 0);
    EXPECT_EQ(nodes[node].GetText(), L"changed");
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
    auto nodes = ParseMarkdown(new_md);
    ASSERT_GE(node, 0);
    EXPECT_EQ(nodes[node].type, NodeType::BlockQuote);
}

TEST(DiffToNode, EditWithJapanese)
{
    // 日本語テキストの編集
    std::string old_md = "# はじめに\n\n旧テキスト\n\nおわり";
    std::string new_md = "# はじめに\n\n新テキスト\n\nおわり";
    int node = SimulateEditAndFindNode(old_md, new_md);
    auto nodes = ParseMarkdown(new_md);
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
    auto nodes = ParseMarkdown(new_md);
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
    auto nodes = ParseMarkdown(new_md);
    ASSERT_GE(node, 0);
    EXPECT_EQ(nodes[node].GetText(), L"CHANGED paragraph 50");
}
