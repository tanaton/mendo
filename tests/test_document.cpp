#include <gtest/gtest.h>
#include "document.h"
#include <string>
#include <utility>

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
    EXPECT_EQ(doc.GetNodes()[doc.GetToc().GetEntries()[0].node_index].GetText(), L"Hello");
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
    EXPECT_EQ(doc.GetNodes()[doc.GetToc().GetEntries()[0].node_index].GetText(), L"First");

    // 新しいコンテンツで置き換え
    doc.ReplaceContent(ParseMarkdown(L"# Second\n## Sub"));

    // TOCが再構築されるべき
    EXPECT_GE(doc.GetToc().GetEntries().size(), 2u);
    EXPECT_EQ(doc.GetNodes()[doc.GetToc().GetEntries()[0].node_index].GetText(), L"Second");
    // ファイルパスは変更されないべき
    EXPECT_EQ(doc.GetFilePath(), L"C:\\test.md");
}

TEST(DocumentTest, GetNodesMut)
{
    auto doc = Document::FromMarkdown("hello", L"test.md");
    ASSERT_FALSE(doc.IsEmpty());

    // 可変参照を通じてノードを変更
    auto& nodes = doc.GetNodesMut();
    nodes[0].SetText(L"modified");
    EXPECT_EQ(doc.GetNodes()[0].GetText(), L"modified");
}

// ---- GetRawText / GetLoadedByteSize ----

TEST(DocumentTest, GetRawTextFromMarkdown)
{
    auto doc = Document::FromMarkdown("# Hello\nworld", L"test.md");
    EXPECT_EQ(doc.GetRawText(), L"# Hello\nworld");
    EXPECT_EQ(doc.GetLoadedByteSize(), 13u);
}

TEST(DocumentTest, GetRawTextEmpty)
{
    auto doc = Document::FromMarkdown("", L"test.md");
    EXPECT_TRUE(doc.GetRawText().empty());
    EXPECT_EQ(doc.GetLoadedByteSize(), 0u);
}

TEST(DocumentTest, GetRawTextDefault)
{
    Document doc;
    EXPECT_TRUE(doc.GetRawText().empty());
    EXPECT_EQ(doc.GetLoadedByteSize(), 0u);
}

TEST(DocumentTest, GetRawTextAfterReplace)
{
    auto doc = Document::FromMarkdown("old content", L"test.md");
    EXPECT_EQ(doc.GetRawText(), L"old content");

    doc.ReplaceFromMarkdown("new content");
    EXPECT_EQ(doc.GetRawText(), L"new content");
}

TEST(DocumentTest, GetRawTextUtf8Content)
{
    std::pmr::string utf8 = "# 日本語テスト\n\nこんにちは";
    auto doc = Document::FromMarkdown(utf8, L"test.md");
    // UTF-8 と Wide で同じ論理テキストが得られる
    EXPECT_EQ(doc.GetRawText(), L"# 日本語テスト\n\nこんにちは");
    // バイトサイズは UTF-8 のサイズ（CJK は 3 byte/char）
    EXPECT_EQ(doc.GetLoadedByteSize(), utf8.size());
}

TEST(DocumentTest, GetRawTextPreservedAcrossMultipleReplaces)
{
    auto doc = Document::FromMarkdown("v1", L"test.md");
    EXPECT_EQ(doc.GetRawText(), L"v1");

    doc.ReplaceFromMarkdown("v2");
    EXPECT_EQ(doc.GetRawText(), L"v2");

    doc.ReplaceFromMarkdown("v3");
    EXPECT_EQ(doc.GetRawText(), L"v3");
}

TEST(DocumentTest, GetRawTextIndependentOfNodes)
{
    // ノードの変更が raw_wide_ に影響しないことを確認
    auto doc = Document::FromMarkdown("hello", L"test.md");
    doc.GetNodesMut()[0].SetText(L"modified");
    // raw_wide_ はパース入力のまま
    EXPECT_EQ(doc.GetRawText(), L"hello");
}

TEST(DocumentTest, RawTextSourceOffsetConsistency)
{
    // raw_wide_ 内のオフセット（UTF-16 コード単位）がノードの source_offset と一致することを確認
    std::pmr::string md = "# Title\n\nBody text";
    auto doc = Document::FromMarkdown(md, L"test.md");
    const auto& nodes = doc.GetNodes();
    const auto& raw = doc.GetRawText();
    ASSERT_GE(nodes.size(), 2u);

    // source_offset 位置の文字がノードのテキスト先頭と対応する
    for (const auto& n : nodes) {
        if (n.source_offset != kUnsetSourceOffset && n.source_offset < raw.size()) {
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

TEST(DocumentTest, BuildIndicesDiagramNodes)
{
    auto doc = Document::FromMarkdown("```mermaid\ngraph TD\n```\n\n```cpp\nint x;\n```", L"test.md");
    const auto& diagrams = doc.GetDiagramNodeIndices();
    EXPECT_EQ(diagrams.size(), 1u);
    // cppコードブロックはダイアグラムインデックスに含まれない
    const auto& nodes = doc.GetNodes();
    EXPECT_EQ(nodes[diagrams[0]].code_language, SyntaxLanguage::Mermaid);
}

TEST(DocumentTest, BuildIndicesTocAndAnchorConsistent)
{
    auto doc = Document::FromMarkdown("# A\n\n## B\n\n### C", L"test.md");
    const auto& toc = doc.GetToc().GetEntries();
    ASSERT_EQ(toc.size(), 3u);
    // TOCエントリのnode_indexがアンカーインデックスと一致
    for (const auto& entry : toc) {
        const auto& anchor = doc.GetNodes()[entry.node_index].anchor_id();
        const int anchor_idx = doc.FindAnchorIndex(anchor);
        EXPECT_EQ(anchor_idx, entry.node_index);
    }
}

TEST(DocumentTest, BuildIndicesAfterReplaceContent)
{
    auto doc = Document::FromMarkdown("# Old", L"test.md");
    EXPECT_EQ(doc.GetToc().GetEntries().size(), 1u);
    EXPECT_EQ(doc.FindAnchorIndex(L"old"), 0);

    doc.ReplaceContent(ParseMarkdown(L"# New\n\n## Sub"));
    EXPECT_EQ(doc.GetToc().GetEntries().size(), 2u);
    EXPECT_EQ(doc.FindAnchorIndex(L"old"), -1);
    EXPECT_EQ(doc.FindAnchorIndex(L"new"), 0);
}

TEST(DocumentTest, BuildIndicesNoSpecialNodes)
{
    auto doc = Document::FromMarkdown("just a paragraph", L"test.md");
    EXPECT_TRUE(doc.GetImageNodeIndices().empty());
    EXPECT_TRUE(doc.GetDiagramNodeIndices().empty());
}

// ---- C-3: anchor_index_ string_view 化 回帰テスト ----

TEST(DocumentTest, FindAnchorIndexUppercaseQueryNormalized)
{
    // parser 側スラグは小文字確定だが、クエリ引数に大文字混在があっても
    // ToLowerAscii 経由で hit すること。
    auto doc = Document::FromMarkdown("# Hello World", L"test.md");
    EXPECT_EQ(doc.FindAnchorIndex(L"hello-world"), 0);
    EXPECT_EQ(doc.FindAnchorIndex(L"Hello-World"), 0);
    EXPECT_EQ(doc.FindAnchorIndex(L"HELLO-WORLD"), 0);
}

TEST(DocumentTest, FindAnchorIndexEmptyQuery)
{
    auto doc = Document::FromMarkdown("# Heading", L"test.md");
    EXPECT_EQ(doc.FindAnchorIndex(L""), -1);
}

TEST(DocumentTest, FindNormalizedAnchorIndexHitsLowercase)
{
    // anchor_id() は parser で小文字 ASCII へ正規化済み。
    // FindNormalizedAnchorIndex は ToLowerAscii を介さず直接 hit する。
    auto doc = Document::FromMarkdown("# Hello World", L"test.md");
    EXPECT_EQ(doc.FindNormalizedAnchorIndex(L"hello-world"), 0);
    EXPECT_EQ(doc.FindNormalizedAnchorIndex(L""), -1);
    // 大文字混在は normalized 経路では hit しない（呼び出し側責任の API）。
    EXPECT_EQ(doc.FindNormalizedAnchorIndex(L"Hello-World"), -1);
}

TEST(DocumentTest, FindAnchorIndexAfterDocumentMove)
{
    // anchor_index_ は nodes_ 内 wstring への view を保持するため、
    // Document の move 構築後も nodes_ 要素アドレスが安定していれば lookup が壊れない。
    auto doc = Document::FromMarkdown("# Alpha\n\n## Beta", L"test.md");
    Document moved = std::move(doc);
    EXPECT_EQ(moved.FindAnchorIndex(L"alpha"), 0);
    EXPECT_EQ(moved.FindAnchorIndex(L"beta"), 1);
}

TEST(DocumentTest, FindAnchorIndexLargeHeadingSet)
{
    // 1000 見出しの合成 Markdown で全 anchor が hit すること（string_view 化のスケール耐性検証）。
    std::pmr::string md;
    md.reserve(40 * 1000);
    for (int i = 0; i < 1000; ++i) {
        md += "# Heading-";
        md += std::to_string(i);
        md += "\n\n";
    }
    auto doc = Document::FromMarkdown(std::move(md), L"big.md");
    const auto& toc = doc.GetToc().GetEntries();
    ASSERT_EQ(toc.size(), 1000u);
    for (const auto& entry : toc) {
        const auto anchor = doc.GetNodes()[entry.node_index].anchor_id();
        EXPECT_EQ(doc.FindAnchorIndex(anchor), entry.node_index);
    }
    EXPECT_EQ(doc.FindAnchorIndex(L"heading-not-present"), -1);
}
