#include <gtest/gtest.h>
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
    // Select from middle of "First" to middle of "Third"
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
    // Should have \r\n between nodes
    EXPECT_NE(result.find(L"\r\n"), std::wstring::npos);
}

TEST(ExtractSelectedText, EmptyNodes) {
    std::vector<RenderNode> nodes;
    auto sel = TextSelection::MakeOrdered(0, 0, 0, 5);
    // Out of range nodes - should not crash
    EXPECT_TRUE(ExtractSelectedText(nodes, sel).empty());
}

TEST(ExtractSelectedText, EndBeyondTextSize) {
    auto nodes = ParseMarkdown("Short");
    auto sel = TextSelection::MakeOrdered(0, 0, 0, 1000);
    // end_pos beyond text size should be clamped
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
    // Position within the link text
    auto result = FindLinkAtPosition(nodes[0], 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), L"https://example.com");
}

TEST(FindLinkAtPosition, PositionOutsideLink) {
    auto nodes = ParseMarkdown("before [link](https://example.com) after");
    ASSERT_EQ(nodes.size(), 1u);
    // Position in "before" text (should not be a link)
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
    // Position at the last character of the link
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
    RenderNode node;
    auto result = FindLinkAtPosition(node, 0);
    EXPECT_FALSE(result.has_value());
}

// ============================================================
// FindAnchorNodeIndex
// ============================================================

TEST(FindAnchorNodeIndex, EmptyNodes) {
    std::vector<RenderNode> nodes;
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
    // Anchor is "hello-world", search with uppercase
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
    // Should clamp to last character
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.start, 0u);
    EXPECT_EQ(result.end, 5u);
}

TEST(FindWordBoundaries, NonAlphanumericAtPosition) {
    // CJK characters are not treated as word characters by IsCharAlphaNumericW
    // so double-click on them should not select
    auto result = FindWordBoundaries(L"テスト test", 0);
    EXPECT_FALSE(result.found);
}

TEST(FindWordBoundaries, AsciiWordAfterCjk) {
    // Click on the ASCII word after CJK should work
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
    EXPECT_EQ(BuildTitleString(L""), L"MaDView");
}

TEST(BuildTitleString, WithPath) {
    EXPECT_EQ(BuildTitleString(L"C:\\dir\\test.md"), L"test.md - MaDView");
}

TEST(BuildTitleString, FilenameOnly) {
    EXPECT_EQ(BuildTitleString(L"readme.md"), L"readme.md - MaDView");
}
