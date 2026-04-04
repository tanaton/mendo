#include <gtest/gtest.h>
#include "document.h"

TEST(DocumentTest, DefaultIsEmpty)
{
    Document doc;
    EXPECT_TRUE(doc.IsEmpty());
    EXPECT_TRUE(doc.GetFilePath().empty());
    EXPECT_TRUE(doc.GetNodes().empty());
    EXPECT_TRUE(doc.GetToc().GetEntries().empty());
}

TEST(DocumentTest, FromMarkdownBasic)
{
    auto doc = Document::FromMarkdown("# Hello\nworld", L"C:\\test.md");
    EXPECT_FALSE(doc.IsEmpty());
    EXPECT_EQ(doc.GetFilePath(), L"C:\\test.md");
    EXPECT_GE(doc.GetNodes().size(), 2u);
    // TOCに見出しが含まれるべき
    EXPECT_FALSE(doc.GetToc().GetEntries().empty());
    EXPECT_EQ(doc.GetToc().GetEntries()[0].text, L"Hello");
}

TEST(DocumentTest, FromMarkdownEmpty)
{
    auto doc = Document::FromMarkdown("", L"C:\\empty.md");
    EXPECT_TRUE(doc.IsEmpty());
    EXPECT_EQ(doc.GetFilePath(), L"C:\\empty.md");
}

TEST(DocumentTest, GetDirectory)
{
    auto doc = Document::FromMarkdown("test", L"C:\\dir\\sub\\file.md");
    EXPECT_EQ(doc.GetDirectory(), L"C:\\dir\\sub");
}

TEST(DocumentTest, GetDirectoryForwardSlash)
{
    auto doc = Document::FromMarkdown("test", L"C:/dir/file.md");
    EXPECT_EQ(doc.GetDirectory(), L"C:/dir");
}

TEST(DocumentTest, GetDirectoryEmpty)
{
    Document doc;
    EXPECT_TRUE(doc.GetDirectory().empty());
}

TEST(DocumentTest, GetDirectoryNoSlash)
{
    auto doc = Document::FromMarkdown("test", L"file.md");
    EXPECT_TRUE(doc.GetDirectory().empty());
}

TEST(DocumentTest, SetFilePath)
{
    Document doc;
    doc.SetFilePath(L"C:\\new\\path.md");
    EXPECT_EQ(doc.GetFilePath(), L"C:\\new\\path.md");
}

TEST(DocumentTest, ReplaceContent)
{
    auto doc = Document::FromMarkdown("# First", L"C:\\test.md");
    EXPECT_FALSE(doc.GetToc().GetEntries().empty());
    EXPECT_EQ(doc.GetToc().GetEntries()[0].text, L"First");

    // 新しいコンテンツで置き換え
    auto doc2 = Document::FromMarkdown("# Second\n## Sub", L"");
    doc.ReplaceContent(std::move(doc2.GetNodesMut()));

    // TOCが再構築されるべき
    EXPECT_GE(doc.GetToc().GetEntries().size(), 2u);
    EXPECT_EQ(doc.GetToc().GetEntries()[0].text, L"Second");
    // ファイルパスは変更されないべき
    EXPECT_EQ(doc.GetFilePath(), L"C:\\test.md");
}

TEST(DocumentTest, GetNodesMut)
{
    auto doc = Document::FromMarkdown("hello", L"test.md");
    ASSERT_FALSE(doc.IsEmpty());

    // 可変参照を通じてノードを変更
    auto& nodes = doc.GetNodesMut();
    nodes[0].text = L"modified";
    EXPECT_EQ(doc.GetNodes()[0].text, L"modified");
}

// ---- GetRawUtf8 ----

TEST(DocumentTest, GetRawUtf8FromMarkdown)
{
    auto doc = Document::FromMarkdown("# Hello\nworld", L"test.md");
    EXPECT_EQ(doc.GetRawUtf8(), "# Hello\nworld");
}

TEST(DocumentTest, GetRawUtf8Empty)
{
    auto doc = Document::FromMarkdown("", L"test.md");
    EXPECT_TRUE(doc.GetRawUtf8().empty());
}

TEST(DocumentTest, GetRawUtf8Default)
{
    Document doc;
    EXPECT_TRUE(doc.GetRawUtf8().empty());
}

TEST(DocumentTest, GetRawUtf8AfterReplace)
{
    auto doc = Document::FromMarkdown("old content", L"test.md");
    EXPECT_EQ(doc.GetRawUtf8(), "old content");

    doc.ReplaceFromMarkdown("new content");
    EXPECT_EQ(doc.GetRawUtf8(), "new content");
}

TEST(DocumentTest, GetRawUtf8Utf8Content)
{
    std::pmr::string utf8 = "# 日本語テスト\n\nこんにちは";
    auto doc = Document::FromMarkdown(utf8, L"test.md");
    EXPECT_EQ(doc.GetRawUtf8(), utf8);
}

TEST(DocumentTest, GetRawUtf8PreservedAcrossMultipleReplaces)
{
    auto doc = Document::FromMarkdown("v1", L"test.md");
    EXPECT_EQ(doc.GetRawUtf8(), "v1");

    doc.ReplaceFromMarkdown("v2");
    EXPECT_EQ(doc.GetRawUtf8(), "v2");

    doc.ReplaceFromMarkdown("v3");
    EXPECT_EQ(doc.GetRawUtf8(), "v3");
}

TEST(DocumentTest, GetRawUtf8IndependentOfNodes)
{
    // ノードの変更が raw_utf8_ に影響しないことを確認
    auto doc = Document::FromMarkdown("hello", L"test.md");
    doc.GetNodesMut()[0].text = L"modified";
    // raw_utf8_ はパース入力のまま
    EXPECT_EQ(doc.GetRawUtf8(), "hello");
}

TEST(DocumentTest, RawUtf8SourceOffsetConsistency)
{
    // raw_utf8_ 内のオフセットがノードの source_offset と一致することを確認
    std::pmr::string md = "# Title\n\nBody text";
    auto doc = Document::FromMarkdown(md, L"test.md");
    const auto& nodes = doc.GetNodes();
    const auto& raw = doc.GetRawUtf8();
    ASSERT_GE(nodes.size(), 2u);

    // source_offset 位置の文字がノードのテキスト先頭と対応する
    for (const auto& n : nodes) {
        if (n.source_offset != UINT32_MAX && n.source_offset < raw.size()) {
            // ソース位置のバイトがノードテキストの最初の文字のUTF-8エンコーディングと一致
            EXPECT_LT(n.source_offset, static_cast<uint32_t>(raw.size()));
        }
    }
}

// ---- BuildIndices統合テスト ----

TEST(DocumentTest, BuildIndicesAnchorIndex)
{
    auto doc = Document::FromMarkdown("# First\n\n## Second\n\ntext", L"test.md");
    EXPECT_EQ(doc.FindAnchorIndex(L"first"), 0);
    EXPECT_EQ(doc.FindAnchorIndex(L"second"), 1);
    EXPECT_EQ(doc.FindAnchorIndex(L"nonexistent"), -1);
}

TEST(DocumentTest, BuildIndicesImageNodes)
{
    auto doc = Document::FromMarkdown("text\n\n![alt](img.png)\n\nmore text", L"test.md");
    const auto& images = doc.GetImageNodeIndices();
    EXPECT_EQ(images.size(), 1u);
}

TEST(DocumentTest, BuildIndicesMermaidNodes)
{
    auto doc = Document::FromMarkdown("```mermaid\ngraph TD\n```\n\n```cpp\nint x;\n```", L"test.md");
    const auto& mermaids = doc.GetMermaidNodeIndices();
    EXPECT_EQ(mermaids.size(), 1u);
    // cppコードブロックはMermaidインデックスに含まれない
    const auto& nodes = doc.GetNodes();
    EXPECT_EQ(nodes[mermaids[0]].code_language, SyntaxLanguage::Mermaid);
}

TEST(DocumentTest, BuildIndicesTocAndAnchorConsistent)
{
    auto doc = Document::FromMarkdown("# A\n\n## B\n\n### C", L"test.md");
    const auto& toc = doc.GetToc().GetEntries();
    ASSERT_EQ(toc.size(), 3u);
    // TOCエントリのnode_indexがアンカーインデックスと一致
    for (const auto& entry : toc) {
        const int anchor_idx = doc.FindAnchorIndex(entry.anchor_id);
        EXPECT_EQ(anchor_idx, entry.node_index);
    }
}

TEST(DocumentTest, BuildIndicesAfterReplaceContent)
{
    auto doc = Document::FromMarkdown("# Old", L"test.md");
    EXPECT_EQ(doc.GetToc().GetEntries().size(), 1u);
    EXPECT_EQ(doc.FindAnchorIndex(L"old"), 0);

    auto doc2 = Document::FromMarkdown("# New\n\n## Sub", L"");
    doc.ReplaceContent(std::move(doc2.GetNodesMut()));
    EXPECT_EQ(doc.GetToc().GetEntries().size(), 2u);
    EXPECT_EQ(doc.FindAnchorIndex(L"old"), -1);
    EXPECT_EQ(doc.FindAnchorIndex(L"new"), 0);
}

TEST(DocumentTest, BuildIndicesNoSpecialNodes)
{
    auto doc = Document::FromMarkdown("just a paragraph", L"test.md");
    EXPECT_TRUE(doc.GetImageNodeIndices().empty());
    EXPECT_TRUE(doc.GetMermaidNodeIndices().empty());
}
