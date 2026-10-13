#include <gtest/gtest.h>
#include <windows.h>
#include <psapi.h>
#include "document.h"
#include "example_files.h"
#include "test_nodes.h"
#include <algorithm>
#include <chrono>
#include <format>
#include <iterator>
#include <memory>
#include <iostream>
#include <string>
#include <vector>

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

// FromMarkdown 全体の処理時間を CRLF / LF それぞれで計測 (相対比較)。
// CRLF は NormalizeNewlines slow path、LF は fast path (\r 検索のみ)。
// 差分が NormalizeNewlines slow path のコスト概算 (parse 部分は同等のため)。
//
// DISABLED_ プレフィックスでデフォルト無効化 (50 iter × 2 で 100ms+ 占有するため)。
// 手元で実行: build/tests/Release/mendo_tests.exe --gtest_also_run_disabled_tests \
//   --gtest_filter="ViewStats.DISABLED_BenchFromMarkdownCrlfVsLf"
TEST(ViewStats, DISABLED_BenchFromMarkdownCrlfVsLf)
{
    auto bytes_lf = ReadExampleFileBytes("example/test.md");
    if (bytes_lf.empty()) {
        GTEST_SKIP() << "example/test.md not found";
    }
    // bytes_lf はリポジトリ上 CRLF (Windows checkout)。LF only 版を作る。
    std::pmr::string bytes_crlf = bytes_lf; // 既に CRLF
    std::pmr::string lf_only;
    lf_only.reserve(bytes_lf.size());
    for (char c : bytes_lf) {
        if (c != '\r') {
            lf_only.push_back(c);
        }
    }

    constexpr int kIterations = 50;

    auto bench = [&](std::string_view label, const std::pmr::string& src) {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kIterations; ++i) {
            std::pmr::string copy = src;
            auto doc = Document::FromMarkdown(std::move(copy), L"bench.md");
            (void)doc;
        }
        auto end = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(end - start).count();
        std::cout << "  " << label << " (" << src.size() << " bytes): "
                  << (ms / kIterations) << " ms/iter (total " << ms << " ms over "
                  << kIterations << " iters)\n";
    };

    std::cout << "=== FromMarkdown bench ===\n";
    bench("CRLF input", bytes_crlf);
    bench("LF only input", lf_only);
}

namespace {

PROCESS_MEMORY_COUNTERS_EX MemoryCounters()
{
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    ::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    return pmc;
}

constexpr size_t kBenchTargetBytes = 100 * 1024 * 1024;

// パース・破棄の時間とパース後のメモリ増分を測る。
void BenchLargeDocument(std::string_view label, const std::pmr::string& src)
{
    constexpr int kIterations = 3;
    double parse_ms = 0.0;
    double destroy_ms = 0.0;
    size_t nodes = 0;
    // 初回のみ (2 回目以降は解放済み領域の再利用で小さく出る)。
    size_t private_delta = 0;
    size_t working_set_delta = 0;
    for (int i = 0; i < kIterations; ++i) {
        std::pmr::string copy = src;
        const auto before = MemoryCounters();
        const auto t0 = std::chrono::steady_clock::now();
        auto doc = std::make_unique<Document>(Document::FromMarkdown(std::move(copy), L"bench.md"));
        const auto t1 = std::chrono::steady_clock::now();
        if (i == 0) {
            const auto after = MemoryCounters();
            private_delta = after.PrivateUsage - before.PrivateUsage;
            working_set_delta = after.WorkingSetSize - before.WorkingSetSize;
        }
        nodes = doc->GetNodes().size();
        doc.reset();
        const auto t2 = std::chrono::steady_clock::now();
        parse_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
        destroy_ms += std::chrono::duration<double, std::milli>(t2 - t1).count();
    }
    std::cout << "=== " << label << " (" << src.size() << " bytes, " << nodes << " nodes, sizeof(Node)="
              << sizeof(Node) << ") ===\n"
              << "  parse:   " << parse_ms / kIterations << " ms\n"
              << "  destroy: " << destroy_ms / kIterations << " ms\n"
              << "  committed by parse:   " << private_delta / (1024.0 * 1024.0) << " MB\n"
              << "  working set by parse: " << working_set_delta / (1024.0 * 1024.0) << " MB\n";
}

} // namespace

// 手元で実行: build/tests/Release/mendo_tests.exe --gtest_also_run_disabled_tests \
//   --gtest_filter="ViewStats.DISABLED_BenchLarge*"
TEST(ViewStats, DISABLED_BenchLargeDocument)
{
    const auto unit = ReadExampleFileBytes("example/test.md");
    if (unit.empty()) {
        GTEST_SKIP() << "example/test.md not found";
    }
    std::pmr::string src;
    src.reserve(kBenchTargetBytes + unit.size());
    while (src.size() < kBenchTargetBytes) {
        src += unit;
        src += "\n\n";
    }
    BenchLargeDocument("large document (test.md)", src);
}

// リンク集のように全行がリンクを持つ文書。
TEST(ViewStats, DISABLED_BenchLargeLinkDocument)
{
    std::pmr::string src;
    src.reserve(kBenchTargetBytes + 256);
    for (size_t i = 0; src.size() < kBenchTargetBytes; ++i) {
        std::format_to(std::back_inserter(src),
                       "- [Item {0}](https://example.com/docs/page-{0}.html) と [詳細](#section-{1}) を参照\n",
                       i, i % 100);
        if (i % 20 == 19) {
            src += '\n';
        }
    }
    BenchLargeDocument("large document (links)", src);
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

TEST(ViewStats, DISABLED_DumpFirstOwnedCodeBlocks)
{
    auto bytes = ReadExampleFileBytes("example/test.md");
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
        const bool is_view = TextPointsIntoRaw(n, raw);
        const std::string_view text = n.GetText();
        const size_t off = n.SourceOffset();
        std::cout << "[" << (is_view ? "view " : "owned") << "] source_offset=" << off
                  << " text.size()=" << text.size() << " line_count=" << n.line_count;
        if (off != kUnsetSourceOffset && off < raw.size()) {
            const size_t avail = std::min<size_t>(text.size(), raw.size() - off);
            const std::string_view raw_slice = std::string_view{ raw }.substr(off, avail);
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
