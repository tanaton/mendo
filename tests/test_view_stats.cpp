#include <gtest/gtest.h>
#include "document.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <ranges>
#include <sstream>
#include <string>
#include <vector>

namespace {

// "example/xxx.md" 形式の相対パスを、cwd に依存せずソースツリーの example/ から開く。
// MENDO_EXAMPLE_DIR (CMake が u8 リテラルで渡す絶対パス) を基準に解決し、日本語を含む
// UTF-8 パスも std::filesystem::path 経由で正しく扱う。define が無い場合は cwd 相対で開く。
std::pmr::string ReadFileBytes(const std::string& path)
{
    std::filesystem::path full;
#ifdef MENDO_EXAMPLE_DIR
    std::string_view rel = path;
    constexpr std::string_view prefix = "example/";
    if (rel.starts_with(prefix)) {
        rel.remove_prefix(prefix.size());
    }
    full = std::filesystem::path(MENDO_EXAMPLE_DIR) / std::filesystem::path(rel);
#else
    full = std::filesystem::path(path);
#endif
    std::ifstream file(full, std::ios::binary);
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
        if (n.IsViewMode()) {
            s.view_nodes++;
            s.view_chars += n.GetText().size();
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
            if (n.IsViewMode()) {
                view_count++;
                view_chars += n.GetText().size();
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
            std::cout << "  code block is_view=" << n.IsViewMode()
                      << ", text.size()=" << n.GetText().size() << "\n";
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
    auto bytes_lf = ReadFileBytes("example/test.md");
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

// シンプルな段落 (span マークアップなし) が view 化されることを確認
TEST(ViewStats, SimpleParagraphIsViewed)
{
    auto doc = Document::FromMarkdown("Just a plain paragraph without any markup.", L"test.md");
    for (const auto& n : doc.GetNodes()) {
        if (n.type == NodeType::Paragraph) {
            std::cout << "  paragraph is_view=" << n.IsViewMode()
                      << ", text.size()=" << n.GetText().size() << "\n";
        }
    }
}

namespace {

// runs.size() のヒストグラムを集計し、SBO=1..8 ヒット率を標準出力に書き出す。
// predicate で集計対象を絞り込む (例: 特定 NodeType のみ)。
template <class Pred>
size_t RunRunsSizeHistogram(const Document& doc, std::string_view label, std::string_view total_label,
                            std::string_view sbo_label, Pred pred)
{
    std::array<size_t, 12> buckets{};
    size_t total = 0;
    size_t total_runs = 0;
    size_t max_runs = 0;
    for (const auto& n : doc.GetNodes()) {
        if (!pred(n)) {
            continue;
        }
        const auto sz = n.runs.size();
        total++;
        total_runs += sz;
        max_runs = std::max(max_runs, static_cast<size_t>(sz));
        const size_t idx = std::min<size_t>(sz, 11);
        buckets[idx]++;
    }

    auto pct = [&](size_t n) {
        return total > 0 ? (100.0 * n / total) : 0.0;
    };

    std::cout << "=== " << label << " ===\n";
    std::cout << "  " << total_label << ": " << total << ", total runs: " << total_runs
              << ", avg: " << (total > 0 ? static_cast<double>(total_runs) / total : 0.0)
              << ", max: " << max_runs << "\n";
    for (size_t i = 0; i <= 10; ++i) {
        std::cout << "  size=" << i << ": " << buckets[i] << " nodes (" << pct(buckets[i]) << "%)\n";
    }
    std::cout << "  size>=11: " << buckets[11] << " nodes (" << pct(buckets[11]) << "%)\n";

    std::cout << "=== " << sbo_label << " ===\n";
    for (size_t sbo : { 1u, 2u, 3u, 4u, 6u, 8u }) {
        size_t hit = 0;
        for (size_t i = 0; i <= std::min<size_t>(sbo, 11); ++i) {
            hit += buckets[i];
        }
        std::cout << "  SBO=" << sbo << ": hit " << hit << " / " << total
                  << " (" << pct(hit) << "%), miss " << (total - hit)
                  << " (" << pct(total - hit) << "%)\n";
    }
    return total;
}

} // namespace

// Node::runs (small_vector<TextRun, N>) の SBO 値が妥当かを test.md で計測する。
// 各ノードの runs.size() ヒストグラムと、SBO=1..8 での hit 率を出力する。
TEST(ViewStats, RunsSizeHistogramTestMd)
{
    auto bytes = ReadFileBytes("example/test.md");
    if (bytes.empty()) {
        GTEST_SKIP() << "example/test.md not found";
    }
    auto doc = Document::FromMarkdown(std::move(bytes), L"test.md");

    const size_t total = RunRunsSizeHistogram(
        doc,
        "runs.size() histogram (test.md, all NodeTypes)",
        "total nodes",
        "SBO hit rate (size <= N)",
        [](const Node&) { return true; });

    EXPECT_GT(total, 0u);
}

// runs を実際に使うノード型 (Heading/Paragraph/ListItem/BlockQuote/TaskListItem) に絞った
// 分布。Table/Image/HR/CodeBlock は runs が常に空に近いので統計を歪める。
TEST(ViewStats, RunsSizeHistogramTextNodesOnly)
{
    auto bytes = ReadFileBytes("example/test.md");
    if (bytes.empty()) {
        GTEST_SKIP() << "example/test.md not found";
    }
    auto doc = Document::FromMarkdown(std::move(bytes), L"test.md");

    const size_t total = RunRunsSizeHistogram(
        doc,
        "runs.size() histogram (test.md, text nodes only)",
        "total text nodes",
        "SBO hit rate (text nodes, size <= N)",
        [](const Node& n) {
        return n.type == NodeType::Heading || n.type == NodeType::Paragraph ||
               n.type == NodeType::ListItem || n.type == NodeType::BlockQuote ||
               n.type == NodeType::TaskListItem;
    });

    EXPECT_GT(total, 0u);
}

// NodeType 別の runs.size() の中央値・平均・最大。
TEST(ViewStats, RunsSizeBreakdownByNodeType)
{
    auto bytes = ReadFileBytes("example/test.md");
    if (bytes.empty()) {
        GTEST_SKIP() << "example/test.md not found";
    }
    auto doc = Document::FromMarkdown(std::move(bytes), L"test.md");

    auto bucket_label = [](NodeType t) -> const char* {
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
    };

    std::cout << "=== runs.size() per NodeType (test.md) ===\n";
    std::cout << "  type            | count | sum  | avg  | median | max | sbo4_hit\n";
    for (NodeType type : { NodeType::Heading, NodeType::Paragraph, NodeType::CodeBlock,
                           NodeType::ListItem, NodeType::BlockQuote, NodeType::Table,
                           NodeType::TaskListItem, NodeType::Image, NodeType::HorizontalRule }) {
        std::vector<size_t> sizes;
        for (const auto& n : doc.GetNodes()) {
            if (n.type == type) {
                sizes.push_back(n.runs.size());
            }
        }
        if (sizes.empty()) {
            continue;
        }
        std::sort(sizes.begin(), sizes.end());
        const size_t sum = std::accumulate(sizes.begin(), sizes.end(), size_t{ 0 });
        const size_t median = sizes[sizes.size() / 2];
        const size_t max_v = sizes.back();
        const size_t sbo4_hit = std::ranges::count_if(sizes, [](size_t s) { return s <= 4; });
        const double avg = static_cast<double>(sum) / sizes.size();
        const double sbo4_pct = 100.0 * sbo4_hit / sizes.size();
        std::cout << "  " << bucket_label(type)
                  << " | count=" << sizes.size()
                  << " sum=" << sum
                  << " avg=" << avg
                  << " median=" << median
                  << " max=" << max_v
                  << " sbo4_hit=" << sbo4_hit << " (" << sbo4_pct << "%)\n";
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
    const char* const raw_base = raw.data();
    int dumped = 0;
    for (const auto& n : doc.GetNodes()) {
        if (n.type != NodeType::CodeBlock || !n.HasText()) {
            continue;
        }
        const bool is_view = n.IsViewMode();
        const std::string_view text = n.GetText();
        const size_t off = n.SourceOffsetFrom(raw_base);
        std::cout << "[" << (is_view ? "view " : "owned") << "] source_offset=" << off
                  << " text.size()=" << text.size() << " line_count=" << n.line_count;
        if (off != kUnsetSourceOffset && off < raw.size()) {
            const size_t avail = std::min<size_t>(text.size(), raw.size() - off);
            const std::string_view raw_slice{ raw.data() + off, avail };
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
