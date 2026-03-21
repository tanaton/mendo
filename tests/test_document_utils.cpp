#include <gtest/gtest.h>
#include <memory_resource>
#include <string_view>
#include "document_utils.h"
#include "parser.h"

// ============================================================
// ExtractSelectedText
// ============================================================

TEST(ExtractSelectedText, InactiveSelectionReturnsEmpty) {
    auto nodes = ParseMarkdown("Hello world");
    TextSelection sel;
    sel.active = false;
    EXPECT_TRUE(ExtractSelectedText(nodes, sel).empty());
}

TEST(ExtractSelectedText, SingleNodeFullSelection) {
    auto nodes = ParseMarkdown("Hello world");
    auto sel = TextSelection::MakeOrdered(0, 0, 0, static_cast<uint32_t>(nodes[0].text.size()));
    EXPECT_EQ(ExtractSelectedText(nodes, sel), L"Hello world");
}

TEST(ExtractSelectedText, SingleNodePartialSelection) {
    auto nodes = ParseMarkdown("Hello world");
    auto sel = TextSelection::MakeOrdered(0, 0, 0, 5);
    EXPECT_EQ(ExtractSelectedText(nodes, sel), L"Hello");
}

TEST(ExtractSelectedText, SingleNodeMiddleSelection) {
    auto nodes = ParseMarkdown("Hello world");
    auto sel = TextSelection::MakeOrdered(0, 6, 0, 11);
    EXPECT_EQ(ExtractSelectedText(nodes, sel), L"world");
}

TEST(ExtractSelectedText, MultipleNodesFullSelection) {
    auto nodes = ParseMarkdown("First\n\nSecond\n\nThird");
    ASSERT_EQ(nodes.size(), 3u);
    auto sel = TextSelection::MakeOrdered(
        0, 0, 2, static_cast<uint32_t>(nodes[2].text.size()));
    auto result = ExtractSelectedText(nodes, sel);
    EXPECT_NE(result.find(L"First"), std::wstring::npos);
    EXPECT_NE(result.find(L"Second"), std::wstring::npos);
    EXPECT_NE(result.find(L"Third"), std::wstring::npos);
}

TEST(ExtractSelectedText, MultipleNodesPartialSelection) {
    auto nodes = ParseMarkdown("First\n\nSecond\n\nThird");
    ASSERT_EQ(nodes.size(), 3u);
    // "First"の途中から"Third"の途中まで選択
    auto sel = TextSelection::MakeOrdered(0, 2, 2, 3);
    auto result = ExtractSelectedText(nodes, sel);
    EXPECT_EQ(result.substr(0, 3), L"rst");
    EXPECT_NE(result.find(L"Second"), std::wstring::npos);
    EXPECT_NE(result.find(L"Thi"), std::wstring::npos);
}

TEST(ExtractSelectedText, NewlineBetweenNodes) {
    auto nodes = ParseMarkdown("A\n\nB");
    ASSERT_EQ(nodes.size(), 2u);
    auto sel = TextSelection::MakeOrdered(
        0, 0, 1, static_cast<uint32_t>(nodes[1].text.size()));
    auto result = ExtractSelectedText(nodes, sel);
    // ノード間に\r\nが含まれるべき
    EXPECT_NE(result.find(L"\r\n"), std::wstring::npos);
}

TEST(ExtractSelectedText, EmptyNodes) {
    std::pmr::vector<Node> nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, 5);
    // 範囲外のノード - クラッシュしないこと
    EXPECT_TRUE(ExtractSelectedText(nodes, sel).empty());
}

TEST(ExtractSelectedText, EndBeyondTextSize) {
    auto nodes = ParseMarkdown("Short");
    auto sel = TextSelection::MakeOrdered(0, 0, 0, 1000);
    // end_posがテキストサイズを超える場合はクランプされるべき
    EXPECT_EQ(ExtractSelectedText(nodes, sel), L"Short");
}

TEST(ExtractSelectedText, JapaneseText) {
    auto nodes = ParseMarkdown("日本語テスト");
    auto sel = TextSelection::MakeOrdered(
        0, 0, 0, static_cast<uint32_t>(nodes[0].text.size()));
    EXPECT_EQ(ExtractSelectedText(nodes, sel), L"日本語テスト");
}

// ============================================================
// FindLinkAtPosition
// ============================================================

TEST(FindLinkAtPosition, NoLinks) {
    auto nodes = ParseMarkdown("plain text");
    ASSERT_EQ(nodes.size(), 1u);
    auto result = FindLinkAtPosition(nodes[0], 0);
    EXPECT_FALSE(result.has_value());
}

TEST(FindLinkAtPosition, LinkFound) {
    auto nodes = ParseMarkdown("[click](https://example.com)");
    ASSERT_EQ(nodes.size(), 1u);
    // リンクテキスト内の位置
    auto result = FindLinkAtPosition(nodes[0], 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), L"https://example.com");
}

TEST(FindLinkAtPosition, PositionOutsideLink) {
    auto nodes = ParseMarkdown("before [link](https://example.com) after");
    ASSERT_EQ(nodes.size(), 1u);
    // "before"テキスト内の位置（リンクではないはず）
    auto result = FindLinkAtPosition(nodes[0], 0);
    EXPECT_FALSE(result.has_value());
}

TEST(FindLinkAtPosition, InternalLink) {
    auto nodes = ParseMarkdown("[section](#my-section)");
    ASSERT_EQ(nodes.size(), 1u);
    auto result = FindLinkAtPosition(nodes[0], 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), L"#my-section");
}

TEST(FindLinkAtPosition, PositionAtLinkBoundary) {
    auto nodes = ParseMarkdown("[link](https://example.com)");
    ASSERT_EQ(nodes.size(), 1u);
    // リンクの最後の文字の位置
    uint32_t last_pos = static_cast<uint32_t>(nodes[0].text.size()) - 1;
    auto result = FindLinkAtPosition(nodes[0], last_pos);
    ASSERT_TRUE(result.has_value());
}

TEST(FindLinkAtPosition, PositionBeyondText) {
    auto nodes = ParseMarkdown("[link](https://example.com)");
    ASSERT_EQ(nodes.size(), 1u);
    auto result = FindLinkAtPosition(nodes[0], 9999);
    EXPECT_FALSE(result.has_value());
}

TEST(FindLinkAtPosition, EmptyNode) {
    Node node;
    auto result = FindLinkAtPosition(node, 0);
    EXPECT_FALSE(result.has_value());
}

// ============================================================
// FindAnchorNodeIndex
// ============================================================

TEST(FindAnchorNodeIndex, EmptyNodes) {
    std::pmr::vector<Node> nodes;
    EXPECT_EQ(FindAnchorNodeIndex(nodes, L"test"), -1);
}

TEST(FindAnchorNodeIndex, EmptyAnchor) {
    auto nodes = ParseMarkdown("# Title");
    EXPECT_EQ(FindAnchorNodeIndex(nodes, L""), -1);
}

TEST(FindAnchorNodeIndex, FindExistingAnchor) {
    auto nodes = ParseMarkdown("# Title\n\nParagraph\n\n## Section");
    ASSERT_GE(nodes.size(), 3u);
    int idx = FindAnchorNodeIndex(nodes, L"title");
    EXPECT_EQ(idx, 0);
}

TEST(FindAnchorNodeIndex, FindSecondHeading) {
    auto nodes = ParseMarkdown("# First\n\nParagraph\n\n## Second");
    int idx = FindAnchorNodeIndex(nodes, L"second");
    EXPECT_GE(idx, 0);
    EXPECT_EQ(nodes[idx].text, L"Second");
}

TEST(FindAnchorNodeIndex, CaseInsensitiveSearch) {
    auto nodes = ParseMarkdown("# Hello World");
    // アンカーは"hello-world"、大文字で検索
    int idx = FindAnchorNodeIndex(nodes, L"Hello-World");
    EXPECT_EQ(idx, 0);
}

TEST(FindAnchorNodeIndex, NotFound) {
    auto nodes = ParseMarkdown("# Title");
    EXPECT_EQ(FindAnchorNodeIndex(nodes, L"nonexistent"), -1);
}

TEST(FindAnchorNodeIndex, CjkAnchor) {
    auto nodes = ParseMarkdown("## コードブロック");
    int idx = FindAnchorNodeIndex(nodes, L"コードブロック");
    EXPECT_EQ(idx, 0);
}

TEST(FindAnchorNodeIndex, SkipsNonHeadings) {
    auto nodes = ParseMarkdown("Paragraph\n\n# Heading");
    int idx = FindAnchorNodeIndex(nodes, L"heading");
    EXPECT_GE(idx, 0);
    EXPECT_EQ(nodes[idx].type, NodeType::Heading);
}

// ============================================================
// FindWordBoundaries
// ============================================================

TEST(FindWordBoundaries, EmptyText) {
    auto result = FindWordBoundaries(L"", 0);
    EXPECT_FALSE(result.found);
}

TEST(FindWordBoundaries, SingleWord) {
    auto result = FindWordBoundaries(L"hello", 2);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 5u);
}

TEST(FindWordBoundaries, WordAtStart) {
    auto result = FindWordBoundaries(L"hello world", 0);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 5u);
}

TEST(FindWordBoundaries, WordAtEnd) {
    auto result = FindWordBoundaries(L"hello world", 6);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 6u);
    EXPECT_EQ(result.end, 11u);
}

TEST(FindWordBoundaries, PositionOnSpace) {
    auto result = FindWordBoundaries(L"hello world", 5);
    EXPECT_FALSE(result.found);
}

TEST(FindWordBoundaries, PositionOnPunctuation) {
    auto result = FindWordBoundaries(L"hello, world", 5);
    EXPECT_FALSE(result.found);
}

TEST(FindWordBoundaries, WordWithUnderscore) {
    auto result = FindWordBoundaries(L"my_variable = 1", 3);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 11u);
}

TEST(FindWordBoundaries, WordWithNumbers) {
    auto result = FindWordBoundaries(L"var123 = x", 3);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 6u);
}

TEST(FindWordBoundaries, PositionBeyondEnd) {
    auto result = FindWordBoundaries(L"hello", 100);
    // 最後の文字にクランプされるべき
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 5u);
}

TEST(FindWordBoundaries, NonAlphanumericAtPosition) {
    // CJK文字はIsCharAlphaNumericWで単語文字として扱われないため
    // ダブルクリックでは選択されないべき
    auto result = FindWordBoundaries(L"テスト test", 0);
    EXPECT_FALSE(result.found);
}

TEST(FindWordBoundaries, AsciiWordAfterCjk) {
    // CJKの後のASCII単語をクリックすると動作するべき
    auto result = FindWordBoundaries(L"テスト test", 4);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 4u);
    EXPECT_EQ(result.end, 8u);
}

// ============================================================
// ExtractFilename
// ============================================================

TEST(ExtractFilename, EmptyPath) {
    EXPECT_TRUE(ExtractFilename(L"").empty());
}

TEST(ExtractFilename, BackslashPath) {
    EXPECT_EQ(ExtractFilename(L"C:\\Users\\test\\file.md"), L"file.md");
}

TEST(ExtractFilename, ForwardSlashPath) {
    EXPECT_EQ(ExtractFilename(L"C:/Users/test/file.md"), L"file.md");
}

TEST(ExtractFilename, FilenameOnly) {
    EXPECT_EQ(ExtractFilename(L"file.md"), L"file.md");
}

TEST(ExtractFilename, TrailingSeparator) {
    EXPECT_EQ(ExtractFilename(L"C:\\dir\\"), L"");
}

TEST(ExtractFilename, JapaneseFilename) {
    EXPECT_EQ(ExtractFilename(L"C:\\ドキュメント\\ファイル.md"), L"ファイル.md");
}

// ============================================================
// BuildTitleString
// ============================================================

TEST(BuildTitleString, EmptyPath) {
    EXPECT_EQ(BuildTitleString(L""), L"mendo");
}

TEST(BuildTitleString, WithPath) {
    EXPECT_EQ(BuildTitleString(L"C:\\dir\\test.md"), L"test.md - mendo");
}

TEST(BuildTitleString, FilenameOnly) {
    EXPECT_EQ(BuildTitleString(L"readme.md"), L"readme.md - mendo");
}

// ============================================================
// 追加のエッジケース
// ============================================================

// ---- ExtractSelectedText 追加テスト ----

TEST(ExtractSelectedText, SelectionSpanningTableNode) {
    // テーブル型ノード（線形化テキストを持つ）でのテスト
    Node table_node;
    table_node.type = NodeType::Table;
    table_node.text = L"A\tB\n1\t2";

    std::pmr::vector<Node> nodes = {table_node};
    TextSelection sel;
    sel.start_node = 0;
    sel.start_pos = 0;
    sel.end_node = 0;
    sel.end_pos = 3;
    sel.active = true;

    auto result = ExtractSelectedText(nodes, sel);
    EXPECT_EQ(result, L"A\tB");
}

TEST(ExtractSelectedText, StartNodeOutOfRange) {
    std::pmr::vector<Node> nodes;
    Node n;
    n.text = L"hello";
    nodes.push_back(n);

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

TEST(ExtractSelectedText, EndNodeOutOfRange) {
    std::pmr::vector<Node> nodes;
    Node n;
    n.text = L"hello";
    nodes.push_back(n);

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

TEST(FindLinkAtPosition, MultipleLinkRuns) {
    Node node;
    node.text = L"link1 link2";

    TextRun r1;
    r1.start = 0; r1.length = 5;
    r1.link_url = L"https://a.com";

    TextRun r2;
    r2.start = 6; r2.length = 5;
    r2.link_url = L"https://b.com";

    node.runs = {r1, r2};

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

TEST(FindLinkAtPosition, TableCellLinkFound) {
    // セル(1, 1)にリンクを持つテーブルノードを構築:
    // | Name | URL     |
    // | foo  | [bar](https://example.com) |
    // 線形化テキスト: "Name\tURL\nfoo\tbar"
    Node node;
    node.type = NodeType::Table;

    TableRow header;
    TableCell h0; h0.text = L"Name"; h0.is_header = true;
    TextRun h0r; h0r.start = 0; h0r.length = 4;
    h0.runs.push_back(h0r);
    header.cells.push_back(h0);

    TableCell h1; h1.text = L"URL"; h1.is_header = true;
    TextRun h1r; h1r.start = 0; h1r.length = 3;
    h1.runs.push_back(h1r);
    header.cells.push_back(h1);
    node.table_rows.push_back(header);

    TableRow data;
    TableCell d0; d0.text = L"foo";
    TextRun d0r; d0r.start = 0; d0r.length = 3;
    d0.runs.push_back(d0r);
    data.cells.push_back(d0);

    TableCell d1; d1.text = L"bar";
    TextRun d1r; d1r.start = 0; d1r.length = 3;
    d1r.link_url = L"https://example.com";
    d1.runs.push_back(d1r);
    data.cells.push_back(d1);
    node.table_rows.push_back(data);

    // 線形化: "Name\tURL\nfoo\tbar"
    //          0123 4567 8901 2345
    // "Name" = オフセット 0-3, タブ 4, "URL" = 5-7, 改行 8
    // "foo" = オフセット 9-11, タブ 12, "bar" = 13-15
    node.text = L"Name\tURL\nfoo\tbar";

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

TEST(FindLinkAtPosition, TableCellLinkFromParsedMarkdown) {
    auto nodes = ParseMarkdown(
        "| Text | Link |\n"
        "|------|------|\n"
        "| hello | [click](https://example.com) |"
    );
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Table);

    // セル内にリンクのrunが存在することを確認
    ASSERT_GE(nodes[0].table_rows.size(), 2u);
    const auto& data_row = nodes[0].table_rows[1];
    ASSERT_GE(data_row.cells.size(), 2u);
    bool has_link = false;
    for (const auto& run : data_row.cells[1].runs) {
        if (run.link_url.has_value()) {
            EXPECT_EQ(*run.link_url, L"https://example.com");
            has_link = true;
        }
    }
    EXPECT_TRUE(has_link);
}

TEST(FindLinkAtPosition, TableCellInternalLink) {
    Node node;
    node.type = NodeType::Table;

    TableRow row;
    TableCell cell; cell.text = L"section";
    TextRun r; r.start = 0; r.length = 7;
    r.link_url = L"#my-section";
    cell.runs.push_back(r);
    row.cells.push_back(cell);
    node.table_rows.push_back(row);

    node.text = L"section";

    auto result = FindLinkAtPosition(node, 3);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, L"#my-section");
}

TEST(FindLinkAtPosition, TablePositionOnSeparator) {
    // タブ/改行区切り上の位置ではリンクを返さないべき
    Node node;
    node.type = NodeType::Table;

    TableRow row;
    TableCell c0; c0.text = L"A";
    TextRun r0; r0.start = 0; r0.length = 1;
    c0.runs.push_back(r0);
    row.cells.push_back(c0);

    TableCell c1; c1.text = L"B";
    TextRun r1; r1.start = 0; r1.length = 1;
    r1.link_url = L"https://b.com";
    c1.runs.push_back(r1);
    row.cells.push_back(c1);
    node.table_rows.push_back(row);

    // 線形化: "A\tB" → オフセット 0=A, 1=タブ, 2=B
    node.text = L"A\tB";

    // タブ区切り（オフセット1）はどのセルにもマッチしないべき
    auto result = FindLinkAtPosition(node, 1);
    EXPECT_FALSE(result.has_value());

    // "B"（オフセット2）でリンクが見つかるべき
    auto link = FindLinkAtPosition(node, 2);
    ASSERT_TRUE(link.has_value());
    EXPECT_EQ(*link, L"https://b.com");
}

// ---- FindAnchorNodeIndex 追加テスト ----

TEST(FindAnchorNodeIndex, DuplicateAnchors) {
    std::pmr::vector<Node> nodes;

    Node h1;
    h1.type = NodeType::Heading;
    h1.anchor_id = L"title";
    nodes.push_back(h1);

    Node h2;
    h2.type = NodeType::Heading;
    h2.anchor_id = L"title-1";
    nodes.push_back(h2);

    // 最初のマッチが優先される
    EXPECT_EQ(FindAnchorNodeIndex(nodes, L"title"), 0);
    EXPECT_EQ(FindAnchorNodeIndex(nodes, L"title-1"), 1);
}

// ---- FindWordBoundaries 追加テスト ----

TEST(FindWordBoundaries, SingleCharWord) {
    auto result = FindWordBoundaries(L"a", 0);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 1u);
}

TEST(FindWordBoundaries, AllSpaces) {
    auto result = FindWordBoundaries(L"   ", 1);
    EXPECT_FALSE(result.found);
}

TEST(FindWordBoundaries, MixedPunctuationAndWords) {
    auto result = FindWordBoundaries(L"(hello)", 3);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 1u);
    EXPECT_EQ(result.end, 6u);
}

// ---- ExtractFilename 追加テスト ----

TEST(ExtractFilename, UncPath) {
    EXPECT_EQ(ExtractFilename(L"\\\\server\\share\\file.md"), L"file.md");
}

TEST(ExtractFilename, MixedSeparators) {
    EXPECT_EQ(ExtractFilename(L"C:\\dir/subdir\\file.md"), L"file.md");
}
