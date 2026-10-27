#include <gtest/gtest.h>
#include "document.h"
#include "example_files.h"
#include "test_nodes.h"
#include <algorithm>
#include <iostream>
#include <string>
#include <utility>

namespace {

struct Stats {
    size_t view_nodes = 0;
    size_t owned_nodes = 0;
    size_t empty_nodes = 0;
    size_t view_chars = 0;
    size_t owned_chars = 0;
    size_t runs = 0;
};

Stats CountStats(const Document& doc)
{
    Stats s{};
    for (const auto& n : doc.GetNodes()) {
        s.runs += n.runs().size();
        if (!n.HasText()) {
            s.empty_nodes++;
            continue;
        }
        if (TextPointsIntoRaw(n, doc.GetRawText())) {
            s.view_nodes++;
            s.view_chars += n.GetText().size();
        }
        else {
            s.owned_nodes++;
            s.owned_chars += n.GetText().size();
        }
    }
    return s;
}

void PrintStats(const std::string& label, const Stats& s)
{
    const size_t total = s.view_nodes + s.owned_nodes + s.empty_nodes;
    const double view_pct = total > 0 ? (100.0 * s.view_nodes / total) : 0.0;
    std::cout << "=== " << label << " ===\n";
    std::cout << "  total nodes: " << total << "\n";
    std::cout << "  view nodes:  " << s.view_nodes << " (" << view_pct << "%) " << s.view_chars << " chars\n";
    std::cout << "  owned nodes: " << s.owned_nodes << " " << s.owned_chars << " chars (copied to arena)\n";
    std::cout << "  empty nodes: " << s.empty_nodes << "\n";
    std::cout << "  runs:        " << s.runs << " (" << s.runs * sizeof(TextRun) << " bytes in arena)\n";
}

const char* NodeTypeLabel(NodeType t)
{
    switch (t) {
    case NodeType::Heading:
        return "Heading";
    case NodeType::Paragraph:
        return "Paragraph";
    case NodeType::CodeBlock:
        return "CodeBlock";
    case NodeType::HorizontalRule:
        return "HR";
    case NodeType::ListItem:
        return "ListItem";
    case NodeType::BlockQuote:
        return "BlockQuote";
    case NodeType::Table:
        return "Table";
    case NodeType::TaskListItem:
        return "TaskListItem";
    case NodeType::Image:
        return "Image";
    case NodeType::Diagram:
        return "Diagram";
    }
    return "?";
}

} // namespace

TEST(ViewStats, TestMd)
{
    auto bytes = ReadExampleFileBytes("example/test.md");
    if (bytes.empty()) {
        GTEST_SKIP() << "example/test.md not found";
    }
    auto doc = Document::FromMarkdown(std::move(bytes), L"test.md");
    Stats s = CountStats(doc);
    PrintStats("test.md", s);
    EXPECT_GT(s.view_nodes + s.owned_nodes, 0u);
}

TEST(ViewStats, NestedMd)
{
    auto bytes = ReadExampleFileBytes("example/nested.md");
    if (bytes.empty()) {
        GTEST_SKIP() << "example/nested.md not found";
    }
    auto doc = Document::FromMarkdown(std::move(bytes), L"nested.md");
    Stats s = CountStats(doc);
    PrintStats("nested.md", s);
    EXPECT_GT(s.view_nodes + s.owned_nodes, 0u);
}

// NodeType 別の view/owned 内訳を出力する診断ダンプ (assert なし)。
// 手元で実行: --gtest_also_run_disabled_tests --gtest_filter="ViewStats.DISABLED_*"
TEST(ViewStats, DISABLED_BreakdownByNodeType)
{
    auto bytes = ReadExampleFileBytes("example/test.md");
    if (bytes.empty()) {
        GTEST_SKIP() << "example/test.md not found";
    }
    auto doc = Document::FromMarkdown(std::move(bytes), L"test.md");

    std::cout << "=== breakdown by NodeType (test.md) ===\n";
    for (NodeType type : { NodeType::Heading, NodeType::Paragraph, NodeType::CodeBlock,
                           NodeType::ListItem, NodeType::BlockQuote, NodeType::Table }) {
        size_t view_count = 0, owned_count = 0;
        size_t view_chars = 0, owned_chars = 0;
        for (const auto& n : doc.GetNodes()) {
            if (n.type != type || !n.HasText()) {
                continue;
            }
            if (TextPointsIntoRaw(n, doc.GetRawText())) {
                view_count++;
                view_chars += n.GetText().size();
            }
            else {
                owned_count++;
                owned_chars += n.GetText().size();
            }
        }
        std::cout << "  " << NodeTypeLabel(type) << ": view=" << view_count << " (" << view_chars
                  << " chars), owned=" << owned_count << " (" << owned_chars << " chars)\n";
    }
}

// シンプルな code block が view 化されることを確認 (回帰防止)
TEST(ViewStats, SimpleCodeBlockIsViewed)
{
    auto doc = Document::FromMarkdown("```cpp\nint x = 0;\nint y = 1;\n```", L"test.md");
    bool found_code = false;
    for (const auto& n : doc.GetNodes()) {
        if (n.type == NodeType::CodeBlock) {
            found_code = true;
            EXPECT_TRUE(TextPointsIntoRaw(n, doc.GetRawText()));
        }
    }
    ASSERT_TRUE(found_code);
}

// シンプルな段落 (span マークアップなし) が view 化されることを確認
TEST(ViewStats, SimpleParagraphIsViewed)
{
    auto doc = Document::FromMarkdown("Just a plain paragraph without any markup.", L"test.md");
    bool found_paragraph = false;
    for (const auto& n : doc.GetNodes()) {
        if (n.type == NodeType::Paragraph) {
            found_paragraph = true;
            EXPECT_TRUE(TextPointsIntoRaw(n, doc.GetRawText()));
        }
    }
    ASSERT_TRUE(found_paragraph);
}

// 字下げは md4c から静的な空白で届くが、原文も空白なら 1 行目が字下げされたコードも原文を指す。
// タブの字下げは空白に置き換わるのでコピーになる。
TEST(ViewStats, CodeBlockWithIndentedFirstLineIsViewed)
{
    for (const auto& [md, viewed] : {
             std::pair{ "```\n    return x;\nend\n```\n",    true  },
             std::pair{ "text\n\n```cpp\n  a\n    b\n```\n", true  },
             std::pair{ "```\n\tx\n```\n",                   false }
    }) {
        auto doc = Document::FromMarkdown(md, L"test.md");
        const auto it = std::ranges::find(doc.GetNodes(), NodeType::CodeBlock, &Node::type);
        ASSERT_NE(it, doc.GetNodes().end()) << md;
        EXPECT_EQ(TextPointsIntoRaw(*it, doc.GetRawText()), viewed) << md;
        if (viewed) {
            EXPECT_EQ(doc.GetRawText().substr(it->SourceOffset(), it->GetText().size()), it->GetText()) << md;
        }
    }
}

TEST(ViewStats, DISABLED_DumpFirstOwnedCodeBlocks)
{
    auto bytes = ReadExampleFileBytes("example/test.md");
    if (bytes.empty()) {
        GTEST_SKIP() << "example/test.md not found";
    }
    auto doc = Document::FromMarkdown(std::move(bytes), L"test.md");
    const std::string_view raw = doc.GetRawText();
    int dumped = 0;
    for (const auto& n : doc.GetNodes()) {
        if (n.type != NodeType::CodeBlock || !n.HasText()) {
            continue;
        }
        const bool is_view = TextPointsIntoRaw(n, raw);
        const std::string_view text = n.GetText();
        const size_t off = n.SourceOffset();
        std::cout << "[" << (is_view ? "view " : "owned") << "] source_offset=" << off
                  << " text.size()=" << text.size() << " line_count=" << n.line_count;
        if (off != kUnsetSourceOffset && off < raw.size()) {
            const size_t avail = std::min<size_t>(text.size(), raw.size() - off);
            const std::string_view raw_slice = std::string_view{ raw }.substr(off, avail);
            const size_t first_diff = static_cast<size_t>(std::ranges::mismatch(raw_slice, text).in1 - raw_slice.begin());
            std::cout << " first_diff=" << first_diff;
            if (first_diff < avail) {
                std::cout << " raw[" << first_diff << "]=" << static_cast<int>(raw_slice[first_diff])
                          << " text[" << first_diff << "]=" << static_cast<int>(text[first_diff]);
            }
        }
        std::cout << "\n";
        if (++dumped >= 5) {
            break;
        }
    }
}
