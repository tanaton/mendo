#include <gtest/gtest.h>
#include "document.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::pmr::string ReadFileBytes(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    const std::string s = ss.str();
    return std::pmr::string{ s.data(), s.size() };
}

struct Stats {
    size_t view_nodes = 0;
    size_t owned_nodes = 0;
    size_t empty_nodes = 0;
    size_t view_chars = 0;
    size_t owned_chars = 0;
    size_t small_owned_nodes = 0; // <= 8 chars (SBO 圏)
    size_t big_owned_nodes = 0;   // > 8 chars (heap 確保あり)
};

Stats CountStats(const Document& doc)
{
    Stats s{};
    for (const auto& n : doc.GetNodes()) {
        if (!n.HasText()) {
            s.empty_nodes++;
            continue;
        }
        if (n.view_length > 0) {
            s.view_nodes++;
            s.view_chars += n.view_length;
        }
        else {
            s.owned_nodes++;
            const auto sz = n.GetText().size();
            s.owned_chars += sz;
            if (sz <= 8) {
                s.small_owned_nodes++;
            }
            else {
                s.big_owned_nodes++;
            }
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
    std::cout << "  owned nodes: " << s.owned_nodes << " (" << s.small_owned_nodes << " small, "
              << s.big_owned_nodes << " big >8 chars) " << s.owned_chars << " chars\n";
    std::cout << "  empty nodes: " << s.empty_nodes << "\n";
}

} // namespace

TEST(ViewStats, TestMd)
{
    auto bytes = ReadFileBytes("example/test.md");
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
    auto bytes = ReadFileBytes("example/nested.md");
    if (bytes.empty()) {
        GTEST_SKIP() << "example/nested.md not found";
    }
    auto doc = Document::FromMarkdown(std::move(bytes), L"nested.md");
    Stats s = CountStats(doc);
    PrintStats("nested.md", s);
    EXPECT_GT(s.view_nodes + s.owned_nodes, 0u);
}

// code block / 段落 / 見出しの典型ケースで view 化されるかを個別に確認
TEST(ViewStats, BreakdownByNodeType)
{
    auto bytes = ReadFileBytes("example/test.md");
    if (bytes.empty()) {
        GTEST_SKIP() << "example/test.md not found";
    }
    auto doc = Document::FromMarkdown(std::move(bytes), L"test.md");

    auto bucket_label = [](NodeType t) -> const char* {
        switch (t) {
        case NodeType::Heading:        return "Heading";
        case NodeType::Paragraph:      return "Paragraph";
        case NodeType::CodeBlock:      return "CodeBlock";
        case NodeType::HorizontalRule: return "HR";
        case NodeType::ListItem:       return "ListItem";
        case NodeType::BlockQuote:     return "BlockQuote";
        case NodeType::Table:          return "Table";
        case NodeType::TaskListItem:   return "TaskListItem";
        case NodeType::Image:          return "Image";
        }
        return "?";
    };

    std::cout << "=== breakdown by NodeType (test.md) ===\n";
    for (NodeType type : { NodeType::Heading, NodeType::Paragraph, NodeType::CodeBlock,
                           NodeType::ListItem, NodeType::BlockQuote, NodeType::Table }) {
        size_t view_count = 0, owned_count = 0;
        size_t view_chars = 0, owned_chars = 0;
        for (const auto& n : doc.GetNodes()) {
            if (n.type != type || !n.HasText()) {
                continue;
            }
            if (n.view_length > 0) {
                view_count++;
                view_chars += n.view_length;
            }
            else {
                owned_count++;
                owned_chars += n.GetText().size();
            }
        }
        std::cout << "  " << bucket_label(type) << ": view=" << view_count << " (" << view_chars
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
            std::cout << "  code block view_length=" << n.view_length
                      << ", text.size()=" << n.GetText().size() << "\n";
        }
    }
    ASSERT_TRUE(found_code);
}

// シンプルな段落 (span マークアップなし) が view 化されることを確認
TEST(ViewStats, SimpleParagraphIsViewed)
{
    auto doc = Document::FromMarkdown("Just a plain paragraph without any markup.", L"test.md");
    for (const auto& n : doc.GetNodes()) {
        if (n.type == NodeType::Paragraph) {
            std::cout << "  paragraph view_length=" << n.view_length
                      << ", text.size()=" << n.GetText().size() << "\n";
        }
    }
}

// test.md の owned code block の最初の数個を表示し、なぜ view 化されないかの手がかりを得る
TEST(ViewStats, DumpFirstOwnedCodeBlocks)
{
    auto bytes = ReadFileBytes("example/test.md");
    if (bytes.empty()) {
        GTEST_SKIP() << "example/test.md not found";
    }
    auto doc = Document::FromMarkdown(std::move(bytes), L"test.md");
    const auto& raw = doc.GetRawText();
    int dumped = 0;
    for (const auto& n : doc.GetNodes()) {
        if (n.type != NodeType::CodeBlock || !n.HasText()) {
            continue;
        }
        const bool is_view = n.view_length > 0;
        const std::wstring_view text = n.GetText();
        std::cout << "[" << (is_view ? "view " : "owned") << "] source_offset=" << n.source_offset
                  << " text.size()=" << text.size() << " line_count=" << n.line_count;
        if (n.source_offset != kUnsetSourceOffset && n.source_offset < raw.size()) {
            const size_t avail = std::min<size_t>(text.size(), raw.size() - n.source_offset);
            const std::wstring_view raw_slice{ raw.data() + n.source_offset, avail };
            // 先頭 N 文字の差分位置を探す
            size_t first_diff = avail;
            for (size_t i = 0; i < avail; ++i) {
                if (raw_slice[i] != text[i]) {
                    first_diff = i;
                    break;
                }
            }
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
