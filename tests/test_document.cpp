#include <gtest/gtest.h>
#include "document.h"
#include "document_test_helpers.h"
#include "document_utils.h"
#include "test_nodes.h"
#include <string>
#include <string_view>
#include <utility>

// リロード diff (AnalyzeReloadDiff) は DocumentData::raw と LoadFile の出力が同じ LF 正規化済みである
// 前提 (issue #273)。その正規化自体を担保する。
// NUL は CommonMark と GitHub と同じく U+FFFD (3 byte) にする。
TEST(NormalizeSource, NewlinesBecomeLfAndNulsBecomeReplacement)
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
        { std::string_view{ "a\0b", 3 }, "a\xEF\xBF\xBD" "b" },
        { std::string_view{ "\0\r\n\0", 4 }, "\xEF\xBF\xBD\n\xEF\xBF\xBD" },
        { std::string_view{ "\0", 1 }, "\xEF\xBF\xBD" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(c.in));
        std::string s{ c.in };
        NormalizeSource(s);
        EXPECT_EQ(s, c.expected);
    }
}

// NUL はパースの前に U+FFFD になるので、md4c が NUL を渡す形の違う行内・コードブロック・コードスパン・数式のどこでも
// 同じく出る。HTML ブロックの中身は描かないので出ない。
TEST(DocumentTest, NulsBecomeReplacementEverywhere)
{
    struct Case {
        std::string_view md;
        std::string_view text;
    };
    constexpr Case kCases[] = {
        { std::string_view{ "a\0b", 3 }, "a\xEF\xBF\xBD" "b" },
        { std::string_view{ "```\n\0\0x\n```\n", 12 }, "\xEF\xBF\xBD\xEF\xBF\xBD" "x" },
        { std::string_view{ "`a\0b`", 5 }, "a\xEF\xBF\xBD" "b" },
        { std::string_view{ "$a\0b$", 5 }, "$a\xEF\xBF\xBD" "b$" },
        { std::string_view{ "- \0", 3 }, "\xEF\xBF\xBD" },
        { std::string_view{ "<div>\0</div>\n\npara", 18 }, "para" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(c.md));
        const auto doc = Document::FromMarkdown(std::string{ c.md }, L"t.md");
        const auto& nodes = doc.GetNodes();
        ASSERT_FALSE(nodes.empty());
        EXPECT_EQ(nodes.back().GetText(), c.text);
        const auto replacements = std::ranges::count_if(nodes, [](const Node& n) { return n.GetText().contains("\xEF\xBF\xBD"); });
        EXPECT_EQ(replacements, c.text.contains("\xEF\xBF\xBD") ? 1 : 0);
        EXPECT_FALSE(nodes.back().GetText().contains('\0'));
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
        auto doc = Document::FromMarkdown(std::string{ md }, L"test.md");
        EXPECT_EQ(doc.GetRawText(), md);
    }
}

TEST(DocumentTest, SourceOffsetSurvivesEmbeddedNullInCodeBlock)
{
    // 埋め込み NUL は md4c が外部リテラルポインタで OnText に渡すため、範囲ガードが無いと source_offset に巨大値が入り
    // raw のサイズを超えるリグレッションを検知する。Document は NUL を先に置き換えるので、パーサに直に渡す。
    std::string md;
    md.append("```\n", 4);
    md.push_back('\0');
    md.append("body\n```\n", 9);
    const auto nodes = ParseNodes(md);
    ASSERT_FALSE(nodes.empty());
    for (const auto& n : nodes) {
        EXPECT_LT(n.SourceOffset(), md.size()) << "source_offset must remain within input buffer when md4c emits MD_TEXT_NULLCHAR";
    }
}

// ---- 1.2 view 化 sanity (Document move) ----

// 原文を直接指すノード (view) と、加工が入って arena に持つノード (owned) が同じ文書に並ぶ。
// runs・リンク URL・画像 src も arena か原文を指すので、move 後も同じ値を読め、見出しの索引も引ける。
TEST(DocumentTest, NodesSurviveMove)
{
    auto check = [](const Document& doc) static {
        const auto& nodes = doc.GetNodes();
        ASSERT_EQ(nodes.size(), 5u);
        EXPECT_EQ(nodes[0].GetText(), "Alpha");
        EXPECT_EQ(nodes[1].GetText(), "Hello world");
        EXPECT_EQ(nodes[2].GetText(), "Hello & world");
        EXPECT_EQ(nodes[3].GetText(), "b l m");
        const auto runs = nodes[3].runs();
        ASSERT_EQ(runs.size(), 2u);
        EXPECT_TRUE(runs[0].bold());
        ASSERT_TRUE(runs[1].has_link());
        EXPECT_EQ(nodes[3].view_link_urls()[runs[1].link_url_index], "u_x");
        ASSERT_EQ(nodes[4].runs().size(), 1u);
        EXPECT_EQ(nodes[4].image_src(nodes[4].runs()[0]), "i_x.png");
    };
    auto src = Document::FromMarkdown("# Alpha\n\nHello world\n\nHello &amp; world\n\n**b** [l](u\\_x)\nm\n\n![a](i\\_x.png)", L"t.md");
    check(src);
    Document moved = std::move(src);
    // 索引は move の後に初めて作る。
    EXPECT_EQ(moved.FindAnchorIndex("alpha"), 0);
    check(moved);
    Document assigned = Document::FromMarkdown("other", L"o.md");
    assigned = std::move(moved);
    check(assigned);
    EXPECT_EQ(assigned.FindAnchorIndex("alpha"), 0);
}

// ---- 索引 ----

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

// 画像を持つノードと図 (Mermaid) のノードの索引。ほかのコードブロックは図に数えない。
TEST(DocumentTest, ImageAndDiagramNodeIndices)
{
    auto doc = Document::FromMarkdown("text\n\n![alt](img.png)\n\n```mermaid\ngraph TD\n```\n\n```cpp\nint x;\n```", L"test.md");
    const auto& nodes = doc.GetNodes();
    ASSERT_EQ(doc.GetImageNodeIndices().size(), 1u);
    EXPECT_EQ(nodes[doc.GetImageNodeIndices()[0]].GetText(), "alt");
    ASSERT_EQ(doc.GetDiagramNodeIndices().size(), 1u);
    EXPECT_EQ(nodes[doc.GetDiagramNodeIndices()[0]].code_language, SyntaxLanguage::Mermaid);

    auto plain = Document::FromMarkdown("just a paragraph", L"test.md");
    EXPECT_TRUE(plain.GetImageNodeIndices().empty());
    EXPECT_TRUE(plain.GetDiagramNodeIndices().empty());
}

// ---- FindAnchorIndex 回帰テスト ----

// アンカーは小文字で作るが、大文字の混じったクエリでも引ける。GitHub は非 ASCII も小文字にしたスラグを付けるので、
// そのスラグや大文字のままのリンクでも見出しに飛べる。見出し以外と空のクエリは引けない。
TEST(DocumentTest, FindAnchorIndexMatchesHeadingSlug)
{
    struct Case {
        std::string_view md;
        std::string_view anchor;
        int expected;
    };
    constexpr Case kCases[] = {
        { "# Hello World", "hello-world", 0 },
        { "# Hello World", "Hello-World", 0 },
        { "# Hello World", "HELLO-WORLD", 0 },
        { "Paragraph\n\n## コードブロック", "コードブロック", 1 },
        { "Paragraph\n\n## コードブロック", "paragraph", -1 },
        { "## Введение\n\n## Über", "введение", 0 },
        { "## Введение\n\n## Über", "Введение", 0 },
        { "## Введение\n\n## Über", "über", 1 },
        { "## Введение\n\n## Über", "ÜBER", 1 },
        { "# Heading", "", -1 },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::Message() << c.md << " #" << c.anchor);
        auto doc = Document::FromMarkdown(std::string{ c.md }, L"test.md");
        EXPECT_EQ(doc.FindAnchorIndex(c.anchor), c.expected);
    }
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
