#include <gtest/gtest.h>
#include "document.h"
#include "document_test_helpers.h"
#include "document_utils.h"
#include <string>
#include <string_view>
#include <utility>

// リロード diff (AnalyzeReloadDiff) は DocumentData::raw と LoadFile の出力が同じ LF 正規化済みである
// 前提 (issue #273)。その正規化自体を担保する。
TEST(NormalizeNewlines, CrlfAndLoneCrBecomeLf)
{
    struct Case {
        std::string_view in;
        std::string_view expected;
    };
    constexpr Case kCases[] = {
        { "a\r\nb\r\nc\r\n", "a\nb\nc\n" },
        { "a\rb\rc", "a\nb\nc" },
        { "a\nb\nc\n", "a\nb\nc\n" },
        { "", "" },
        { "a\r\nb\nc\r", "a\nb\nc\n" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(c.in));
        std::pmr::string s{ c.in };
        NormalizeNewlines(s);
        EXPECT_EQ(s, c.expected);
    }
}

TEST(DocumentTest, DefaultIsEmpty)
{
    Document doc;
    EXPECT_TRUE(doc.IsEmpty());
    EXPECT_TRUE(doc.GetFilePath().empty());
    EXPECT_TRUE(doc.GetDirectory().empty());
    EXPECT_TRUE(doc.GetNodes().empty());
    EXPECT_TRUE(doc.GetToc().empty());
    EXPECT_TRUE(doc.GetRawText().empty());
}

TEST(DocumentTest, FromMarkdownBasic)
{
    auto doc = Document::FromMarkdown("# Hello\nworld", L"C:\\test.md");
    EXPECT_FALSE(doc.IsEmpty());
    EXPECT_EQ(doc.GetFilePath(), L"C:\\test.md");
    EXPECT_GE(doc.GetNodes().size(), 2u);
    // TOCに見出しが含まれるべき
    EXPECT_FALSE(doc.GetToc().empty());
    EXPECT_EQ(doc.GetNodes()[doc.GetToc()[0].node_index].GetText(), "Hello");
}

TEST(DocumentTest, FromMarkdownEmpty)
{
    auto doc = Document::FromMarkdown("", L"C:\\empty.md");
    EXPECT_TRUE(doc.IsEmpty());
    EXPECT_EQ(doc.GetFilePath(), L"C:\\empty.md");
}

TEST(DocumentTest, GetDirectory)
{
    struct Case {
        std::wstring_view path;
        std::wstring_view expected;
    };
    constexpr Case kCases[] = {
        { L"C:\\dir\\sub\\file.md", L"C:\\dir\\sub" },
        { L"C:/dir/file.md", L"C:/dir" },
        { L"file.md", L"" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ c.path }));
        EXPECT_EQ(Document::FromMarkdown("test", c.path).GetDirectory(), c.expected);
    }
}

// ---- GetRawText ----

TEST(DocumentTest, GetRawTextKeepsInput)
{
    for (const std::string_view md : { std::string_view{ "# Hello\nworld" }, std::string_view{ "" }, std::string_view{ "# 日本語テスト\n\nこんにちは" } }) {
        SCOPED_TRACE(md);
        auto doc = Document::FromMarkdown(std::pmr::string{ md }, L"test.md");
        EXPECT_EQ(doc.GetRawText(), md);
    }
}

TEST(DocumentTest, SourceOffsetPointsToNodeTextStart)
{
    // 入力は HTML 実体参照やインライン書式を含まない素のテキストのみで、
    // 各ノードの source_offset 位置の文字 == そのノードのテキスト先頭文字 が成立することを確認する。
    auto doc = Document::FromMarkdown("# Title\n\nBody\n\n```\ncode\n```\n", L"test.md");
    const auto& nodes = doc.GetNodes();
    const std::string_view raw = doc.GetRawText();

    bool checked_any = false;
    for (const auto& n : nodes) {
        const size_t off = n.SourceOffset();
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
    const std::string_view raw = doc.GetRawText();
    ASSERT_FALSE(nodes.empty());

    bool checked_any = false;
    for (const auto& n : nodes) {
        const size_t off = n.SourceOffset();
        if (off == kUnsetSourceOffset) {
            continue;
        }
        EXPECT_LT(off, raw.size())
            << "source_offset must remain within input buffer when md4c emits MD_TEXT_NULLCHAR";
        checked_any = true;
    }
    EXPECT_TRUE(checked_any);
}

// ---- 1.2 view 化 sanity (Document move) ----

// 原文を直接指すノードのテキストも、move 後に同じ値を読めること。
TEST(DocumentTest, ViewModeNodesSurviveMove)
{
    auto check = [](const Document& doc) static {
        ASSERT_EQ(doc.GetNodes().size(), 2u);
        EXPECT_EQ(doc.GetNodes()[0].GetText(), "Hello world");
        EXPECT_EQ(doc.GetNodes()[1].GetText(), "Second paragraph");
    };
    auto src = Document::FromMarkdown("Hello world\n\nSecond paragraph", L"test.md");
    check(src);
    Document moved = std::move(src);
    check(moved);
    Document assigned;
    assigned = std::move(moved);
    check(assigned);
}

// runs・リンク URL・画像 src は Document の arena か原文を指すので、move 後も同じ値を読めること。
TEST(DocumentTest, ArenaBackedFieldsSurviveMove)
{
    auto check = [](const Document& doc) static {
        const auto& nodes = doc.GetNodes();
        ASSERT_EQ(nodes.size(), 2u);
        EXPECT_EQ(nodes[0].GetText(), "b l m");
        const auto runs = nodes[0].runs();
        ASSERT_EQ(runs.size(), 2u);
        EXPECT_TRUE(runs[0].bold());
        ASSERT_TRUE(runs[1].has_link());
        EXPECT_EQ(nodes[0].view_link_urls()[runs[1].link_url_index], "u_x");
        ASSERT_NE(nodes[1].image_data(), nullptr);
        EXPECT_EQ(nodes[1].image_data()->src, "i_x.png");
    };
    auto src = Document::FromMarkdown("**b** [l](u\\_x)\nm\n\n![a](i\\_x.png)", L"t.md");
    check(src);
    Document moved = std::move(src);
    check(moved);
    Document assigned = Document::FromMarkdown("other", L"o.md");
    assigned = std::move(moved);
    check(assigned);
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

// ---- BuildIndices統合テスト ----

TEST(DocumentTest, BuildIndicesAnchorIndex)
{
    auto doc = Document::FromMarkdown("# First\n\n## Second\n\ntext", L"test.md");
    EXPECT_EQ(doc.FindAnchorIndex("first"), 0);
    EXPECT_EQ(doc.FindAnchorIndex("second"), 1);
    EXPECT_EQ(doc.FindAnchorIndex("nonexistent"), -1);
}

// 脚注のリンク URL ("#..." ) を辿ると、参照から定義へ、定義の "↩" から参照へ移れる。
TEST(DocumentTest, FootnoteLinksResolveToDefinitionAndBack)
{
    auto doc = Document::FromMarkdown("# Head\n\nText[^1]\n\n[^1]: note", L"test.md");
    const auto& nodes = doc.GetNodes();
    ASSERT_EQ(nodes.size(), 4u);
    const auto anchor_of = [&](size_t node, size_t url) {
        return std::string(nodes[node].view_link_urls()[url].substr(1));
    };
    EXPECT_EQ(doc.FindAnchorIndex(anchor_of(1, 0)), 3);
    EXPECT_EQ(doc.FindAnchorIndex(anchor_of(3, 0)), 1);
    EXPECT_EQ(doc.FindAnchorIndex("USER-CONTENT-FN-1"), 3);
    EXPECT_EQ(doc.FindAnchorIndex("user-content-fn-2"), -1);
    EXPECT_EQ(doc.FindAnchorIndex("head"), 0);
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
    const auto& toc = doc.GetToc();
    ASSERT_EQ(toc.size(), 3u);
    // TOCエントリのnode_indexがアンカーインデックスと一致
    const auto anchors = HeadingAnchors(doc.GetNodes());
    for (const auto& entry : toc) {
        EXPECT_EQ(doc.FindAnchorIndex(anchors[entry.node_index]), entry.node_index);
    }
}

TEST(DocumentTest, BuildIndicesNoSpecialNodes)
{
    auto doc = Document::FromMarkdown("just a paragraph", L"test.md");
    EXPECT_TRUE(doc.GetImageNodeIndices().empty());
    EXPECT_TRUE(doc.GetDiagramNodeIndices().empty());
}

// ---- FindAnchorIndex 回帰テスト ----

TEST(DocumentTest, FindAnchorIndexUppercaseQueryNormalized)
{
    // アンカーは ASCII 小文字で作るが、クエリに大文字が混ざっていても引けること。
    auto doc = Document::FromMarkdown("# Hello World", L"test.md");
    EXPECT_EQ(doc.FindAnchorIndex("hello-world"), 0);
    EXPECT_EQ(doc.FindAnchorIndex("Hello-World"), 0);
    EXPECT_EQ(doc.FindAnchorIndex("HELLO-WORLD"), 0);
}

// 索引は引いた見出しまでしか作らないので、先に後ろの見出しや存在しないアンカーを引いても
// 前の見出しが索引から引けること。
TEST(DocumentTest, FindAnchorIndexExtendsIndexFromPreviousLookup)
{
    auto doc = Document::FromMarkdown("# A\n\n# B\n\n# A\n\n# C", L"test.md");
    EXPECT_EQ(doc.FindAnchorIndex("b"), 1);
    EXPECT_EQ(doc.FindAnchorIndex("a"), 0);
    EXPECT_EQ(doc.FindAnchorIndex("a-1"), 2);
    EXPECT_EQ(doc.FindAnchorIndex("missing"), -1);
    EXPECT_EQ(doc.FindAnchorIndex("c"), 3);
    EXPECT_EQ(doc.FindAnchorIndex("b"), 1);
}

TEST(DocumentTest, FindAnchorIndexCjkHeadingAndSkipsParagraph)
{
    auto doc = Document::FromMarkdown("Paragraph\n\n## コードブロック", L"test.md");
    EXPECT_EQ(doc.FindAnchorIndex("コードブロック"), 1);
    EXPECT_EQ(doc.FindAnchorIndex("paragraph"), -1);
}

// GitHub は非 ASCII も小文字にしたスラグを付けるので、そのスラグや大文字のままのリンクでも見出しに飛べること。
TEST(DocumentTest, FindAnchorIndexNonAsciiCaseInsensitive)
{
    auto doc = Document::FromMarkdown("## Введение\n\n## Über", L"test.md");
    EXPECT_EQ(doc.FindAnchorIndex("введение"), 0);
    EXPECT_EQ(doc.FindAnchorIndex("Введение"), 0);
    EXPECT_EQ(doc.FindAnchorIndex("über"), 1);
    EXPECT_EQ(doc.FindAnchorIndex("ÜBER"), 1);
}

TEST(DocumentTest, FindAnchorIndexEmptyQuery)
{
    auto doc = Document::FromMarkdown("# Heading", L"test.md");
    EXPECT_EQ(doc.FindAnchorIndex(""), -1);
}

TEST(DocumentTest, FindAnchorIndexAfterDocumentMove)
{
    // Document の move 構築後も lookup が壊れないこと。
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
    const auto& toc = doc.GetToc();
    ASSERT_EQ(toc.size(), 1000u);
    const auto anchors = HeadingAnchors(doc.GetNodes());
    for (const auto& entry : toc) {
        EXPECT_EQ(doc.FindAnchorIndex(anchors[entry.node_index]), entry.node_index);
    }
    EXPECT_EQ(doc.FindAnchorIndex("heading-not-present"), -1);
}
