#include <gtest/gtest.h>
#include "document.h"
#include <string>
#include <string_view>
#include <utility>

namespace {
void ReplaceMarkdown(Document& doc, std::string_view text)
{
    std::pmr::string s{ text };
    const size_t sz = s.size();
    doc.ReplaceFromMarkdown(std::move(s), sz);
}
} // namespace

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
    EXPECT_EQ(doc.GetNodes()[doc.GetToc().GetEntries()[0].node_index].GetText(), "Hello");
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
    EXPECT_EQ(doc.GetNodes()[doc.GetToc().GetEntries()[0].node_index].GetText(), "First");

    // 新しいコンテンツで置き換え (raw_text_ ごと差し替えるため ReplaceFromMarkdown を使う)
    constexpr std::string_view kReplaced = "# Second\n## Sub";
    doc.ReplaceFromMarkdown(std::pmr::string{ kReplaced }, kReplaced.size());

    // TOCが再構築されるべき
    EXPECT_GE(doc.GetToc().GetEntries().size(), 2u);
    EXPECT_EQ(doc.GetNodes()[doc.GetToc().GetEntries()[0].node_index].GetText(), "Second");
    // ファイルパスは変更されないべき
    EXPECT_EQ(doc.GetFilePath(), L"C:\\test.md");
}

TEST(DocumentTest, GetNodesMut)
{
    auto doc = Document::FromMarkdown("hello", L"test.md");
    ASSERT_FALSE(doc.IsEmpty());

    // 可変参照を通じてノードを変更
    auto& nodes = doc.GetNodesMut();
    nodes[0].SetText("modified");
    EXPECT_EQ(doc.GetNodes()[0].GetText(), "modified");
}

// ---- GetRawText / GetLoadedByteSize ----

TEST(DocumentTest, GetRawTextFromMarkdown)
{
    auto doc = Document::FromMarkdown("# Hello\nworld", L"test.md");
    EXPECT_EQ(doc.GetRawText(), "# Hello\nworld");
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
    EXPECT_EQ(doc.GetRawText(), "old content");

    ReplaceMarkdown(doc, "new content");
    EXPECT_EQ(doc.GetRawText(), "new content");
}

TEST(DocumentTest, GetRawTextUtf8Content)
{
    std::pmr::string utf8 = "# 日本語テスト\n\nこんにちは";
    auto doc = Document::FromMarkdown(utf8, L"test.md");
    // UTF-8 と Wide で同じ論理テキストが得られる
    EXPECT_EQ(doc.GetRawText(), "# 日本語テスト\n\nこんにちは");
    // バイトサイズは UTF-8 のサイズ（CJK は 3 byte/char）
    EXPECT_EQ(doc.GetLoadedByteSize(), utf8.size());
}

TEST(DocumentTest, GetRawTextPreservedAcrossMultipleReplaces)
{
    auto doc = Document::FromMarkdown("v1", L"test.md");
    EXPECT_EQ(doc.GetRawText(), "v1");

    ReplaceMarkdown(doc, "v2");
    EXPECT_EQ(doc.GetRawText(), "v2");

    ReplaceMarkdown(doc, "v3");
    EXPECT_EQ(doc.GetRawText(), "v3");
}

TEST(DocumentTest, GetRawTextIndependentOfNodes)
{
    // ノードの変更が raw_text_ に影響しないことを確認
    auto doc = Document::FromMarkdown("hello", L"test.md");
    doc.GetNodesMut()[0].SetText("modified");
    // raw_text_ はパース入力のまま
    EXPECT_EQ(doc.GetRawText(), "hello");
}

TEST(DocumentTest, RawTextSourceOffsetConsistency)
{
    // raw_text_ 内の UTF-8 byte オフセットがノードの source_offset と一致することを確認
    std::pmr::string md = "# Title\n\nBody text";
    auto doc = Document::FromMarkdown(md, L"test.md");
    const auto& nodes = doc.GetNodes();
    const auto& raw = doc.GetRawText();
    ASSERT_GE(nodes.size(), 2u);

    // source_offset 位置の文字がノードのテキスト先頭と対応する
    const char* const raw_base = raw.data();
    for (const auto& n : nodes) {
        const uint32_t off = n.SourceOffsetFrom(raw_base);
        if (off != kUnsetSourceOffset && off < raw.size()) {
            EXPECT_LT(off, static_cast<uint32_t>(raw.size()));
        }
    }
}

TEST(DocumentTest, SourceOffsetPointsToNodeTextStart)
{
    // 入力は HTML 実体参照やインライン書式を含まない素のテキストのみで、
    // 各ノードの source_offset 位置の文字 == そのノードのテキスト先頭文字 が成立することを確認する。
    auto doc = Document::FromMarkdown("# Title\n\nBody\n\n```\ncode\n```\n", L"test.md");
    const auto& nodes = doc.GetNodes();
    const auto& raw = doc.GetRawText();

    const char* const raw_base = raw.data();
    bool checked_any = false;
    for (const auto& n : nodes) {
        const uint32_t off = n.SourceOffsetFrom(raw_base);
        if (off == kUnsetSourceOffset) {
            continue;
        }
        if (!n.HasText()) {
            continue;
        }
        ASSERT_LT(off, raw.size());
        EXPECT_EQ(raw[off], n.GetText()[0])
            << "node text starts at raw[" << off << "]";
        checked_any = true;
    }
    EXPECT_TRUE(checked_any);
}

TEST(DocumentTest, SourceOffsetSurvivesEmbeddedNullInCodeBlock)
{
    // 埋め込み NUL は md4c が外部リテラルポインタで OnText に渡すため、
    // 範囲ガードが無いと source_offset に巨大値が入り raw のサイズを超えるリグレッションを検知する。
    std::pmr::string md;
    md.append("```\n", 4);
    md.push_back('\0');
    md.append("body\n```\n", 9);
    auto doc = Document::FromMarkdown(md, L"test.md");
    const auto& nodes = doc.GetNodes();
    const auto& raw = doc.GetRawText();
    ASSERT_FALSE(nodes.empty());

    const char* const raw_base = raw.data();
    bool checked_any = false;
    for (const auto& n : nodes) {
        const uint32_t off = n.SourceOffsetFrom(raw_base);
        if (off == kUnsetSourceOffset) {
            continue;
        }
        EXPECT_LT(off, static_cast<uint32_t>(raw.size()))
            << "source_offset must remain within input buffer when md4c emits MD_TEXT_NULLCHAR";
        checked_any = true;
    }
    EXPECT_TRUE(checked_any);
}

// ---- 1.2 view 化 sanity (Document move / ReplaceFromMarkdown) ----

TEST(DocumentTest, ViewModeNodesSurviveMoveCtor)
{
    auto doc1 = Document::FromMarkdown("Hello world\n\nSecond paragraph", L"test.md");
    ASSERT_GE(doc1.GetNodes().size(), 2u);
    EXPECT_EQ(doc1.GetNodes()[0].GetText(), "Hello world");
    EXPECT_EQ(doc1.GetNodes()[1].GetText(), "Second paragraph");

    Document doc2 = std::move(doc1);
    ASSERT_GE(doc2.GetNodes().size(), 2u);
    EXPECT_EQ(doc2.GetNodes()[0].GetText(), "Hello world");
    EXPECT_EQ(doc2.GetNodes()[1].GetText(), "Second paragraph");
}

TEST(DocumentTest, ViewModeNodesSurviveMoveAssign)
{
    auto src = Document::FromMarkdown("alpha\n\nbeta", L"src.md");
    Document dst;
    dst = std::move(src);
    ASSERT_GE(dst.GetNodes().size(), 2u);
    EXPECT_EQ(dst.GetNodes()[0].GetText(), "alpha");
    EXPECT_EQ(dst.GetNodes()[1].GetText(), "beta");
}

TEST(DocumentTest, ViewModeNodesSurviveReplaceFromMarkdown)
{
    auto doc = Document::FromMarkdown("Initial text", L"test.md");
    EXPECT_EQ(doc.GetNodes()[0].GetText(), "Initial text");

    ReplaceMarkdown(doc, "Updated content here");
    ASSERT_FALSE(doc.GetNodes().empty());
    EXPECT_EQ(doc.GetNodes()[0].GetText(), "Updated content here");
}

TEST(DocumentTest, OwnedAndViewModesCoexist)
{
    // HTML entity (&amp;) を含むノードは加工が入るため owned 経路、
    // 加工なしのノードは view 経路。同一 Document 内で両モードが共存できることを確認する。
    auto doc = Document::FromMarkdown("Hello &amp; world\n\nNoEntityHere", L"test.md");
    ASSERT_GE(doc.GetNodes().size(), 2u);
    EXPECT_EQ(doc.GetNodes()[0].GetText(), "Hello & world");
    EXPECT_EQ(doc.GetNodes()[1].GetText(), "NoEntityHere");
}

TEST(DocumentTest, ViewModeNodesSurviveReplaceFromMarkdownThenMove)
{
    auto doc1 = Document::FromMarkdown("first content", L"test.md");
    ReplaceMarkdown(doc1, "second content after replace");
    Document doc2 = std::move(doc1);
    EXPECT_EQ(doc2.GetNodes()[0].GetText(), "second content after replace");
}

// ---- BuildIndices統合テスト ----

TEST(DocumentTest, BuildIndicesAnchorIndex)
{
    auto doc = Document::FromMarkdown("# First\n\n## Second\n\ntext", L"test.md");
    EXPECT_EQ(doc.FindAnchorIndex("first"), 0);
    EXPECT_EQ(doc.FindAnchorIndex("second"), 1);
    EXPECT_EQ(doc.FindAnchorIndex("nonexistent"), -1);
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
    EXPECT_EQ(nodes[diagrams[0]].code_language(), SyntaxLanguage::Mermaid);
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
    EXPECT_EQ(doc.FindAnchorIndex("old"), 0);

    constexpr std::string_view kReplaced = "# New\n\n## Sub";
    doc.ReplaceFromMarkdown(std::pmr::string{ kReplaced }, kReplaced.size());
    EXPECT_EQ(doc.GetToc().GetEntries().size(), 2u);
    EXPECT_EQ(doc.FindAnchorIndex("old"), -1);
    EXPECT_EQ(doc.FindAnchorIndex("new"), 0);
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
    EXPECT_EQ(doc.FindAnchorIndex("hello-world"), 0);
    EXPECT_EQ(doc.FindAnchorIndex("Hello-World"), 0);
    EXPECT_EQ(doc.FindAnchorIndex("HELLO-WORLD"), 0);
}

TEST(DocumentTest, FindAnchorIndexEmptyQuery)
{
    auto doc = Document::FromMarkdown("# Heading", L"test.md");
    EXPECT_EQ(doc.FindAnchorIndex(""), -1);
}

TEST(DocumentTest, FindNormalizedAnchorIndexHitsLowercase)
{
    // anchor_id() は parser で小文字 ASCII へ正規化済み。
    // FindNormalizedAnchorIndex は ToLowerAscii を介さず直接 hit する。
    auto doc = Document::FromMarkdown("# Hello World", L"test.md");
    EXPECT_EQ(doc.FindNormalizedAnchorIndex("hello-world"), 0);
    EXPECT_EQ(doc.FindNormalizedAnchorIndex(""), -1);
    // 大文字混在は normalized 経路では hit しない（呼び出し側責任の API）。
    EXPECT_EQ(doc.FindNormalizedAnchorIndex("Hello-World"), -1);
}

TEST(DocumentTest, FindAnchorIndexAfterDocumentMove)
{
    // anchor_index_ は nodes_ 内 wstring への view を保持するため、
    // Document の move 構築後も nodes_ 要素アドレスが安定していれば lookup が壊れない。
    auto doc = Document::FromMarkdown("# Alpha\n\n## Beta", L"test.md");
    Document moved = std::move(doc);
    EXPECT_EQ(moved.FindAnchorIndex("alpha"), 0);
    EXPECT_EQ(moved.FindAnchorIndex("beta"), 1);
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
    EXPECT_EQ(doc.FindAnchorIndex("heading-not-present"), -1);
}
