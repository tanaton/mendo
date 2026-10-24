#include <gtest/gtest.h>
#include <atomic>
#include <dwrite.h>
#include <wrl/client.h>
#include <chrono>
#include <iostream>
#include <memory_resource>
#include <numeric>
#include <string>
#include <thread>
#include "document.h"
#include "document_utils.h"
#include "document_types.h"
#include "dwrite_measurer.h"
#include "dwrite_test_base.h"
#include "example_files.h"
#include "layout.h"
#include "layout_cache.h"
#include "layout_computer.h"
#include "mock_text_measurer.h"
#include "syntax.h"
#include "task_scheduler.h"
#include "test_helpers.h"
#include "theme.h"
#include "viewport_manager.h"
#include "test_nodes.h"

namespace {

// 全ノードを MeasureIndicesParallel で計測し、計測できなかったノード数を返す。
int MeasureAll(std::pmr::vector<Node>& nodes, LayoutCache& cache, const Theme& theme, const ITextMeasurer& measurer, TaskScheduler* scheduler)
{
    std::pmr::vector<size_t> indices(nodes.size());
    std::ranges::iota(indices, size_t{ 0 });
    return layout::MeasureIndicesParallel(nodes, cache, 800.0f, theme, measurer, indices, {}, scheduler, kNoLayoutDeadline);
}

// 22000 ノード級の Mixed fixture を決定論的に生成する。配分:
// Paragraph 60% / Heading 10% / HorizontalRule 5% / Table(1x2) 10% / Image 10% / 空 Paragraph 5%
// include_code_block=true の場合は bucket==0,1 を CodeBlock(Cpp) に置換し Paragraph を 50% に減らす。
Node MakeStressNode(size_t i, bool include_code_block)
{
    Node n;
    // 派生 mock のトークン seed として使うため、必ず source_offset を埋める。
    n.SetSourceOffset(i);
    const size_t bucket = i % 20;

    if (include_code_block && bucket < 2) {
        n.type = NodeType::CodeBlock;
        n.set_code_language(SyntaxLanguage::Cpp);
        std::string text = "int v_" + std::to_string(i) + " = 0;";
        SetTestText(n, text, 0);
        return n;
    }
    if (bucket < 12) {
        n.type = NodeType::Paragraph;
        const size_t len = 5 + (i % 80);
        std::string text;
        text.reserve(len);
        for (size_t k = 0; k < len; ++k) {
            text.push_back(static_cast<wchar_t>('a' + ((i + k) % 26)));
        }
        SetTestText(n, text, 0);
        return n;
    }
    if (bucket < 14) {
        n.type = NodeType::Heading;
        n.set_heading_level(static_cast<int8_t>(1 + (i % 6)));
        std::string text = "Heading " + std::to_string(i);
        SetTestText(n, text, 0);
        return n;
    }
    if (bucket == 14) {
        n.type = NodeType::HorizontalRule;
        return n;
    }
    if (bucket < 17) {
        Node table = MakeTableNode({ { "a", "b" } });
        table.SetSourceOffset(i);
        return table;
    }
    if (bucket < 19) {
        static constexpr NodeImageData kImage{ "img.png" };
        n.type = NodeType::Image;
        n.set_image_data(&kImage);
        return n;
    }
    n.type = NodeType::Paragraph;
    n.SetText("", 0);
    return n;
}

struct StressFixture {
    std::pmr::vector<Node> nodes;
    LayoutCache cache;

    void Build(size_t n, bool include_code_block, bool all_dirty)
    {
        nodes.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            nodes.push_back(MakeStressNode(i, include_code_block));
        }
        cache = MakeUniformCache(static_cast<int>(n), 80.0f);
        for (size_t i = 0; i < n; ++i) {
            if (nodes[i].type == NodeType::Image) {
                auto& d = cache.EnsureDiagram(i);
                d.width = 100.0f + static_cast<float>(i % 50);
                d.height = 50.0f + static_cast<float>(i % 30);
            }
        }
        if (!all_dirty) {
            for (size_t i = 0; i < n; ++i) {
                cache[i].layout_dirty = false;
            }
        }
    }
};

// CodeBlock の syntax_tokens を書く派生 mock。並列経路では worker から entry へ直接書かれる。
class MockTextMeasurerWithTokens : public MockTextMeasurer {
public:
    void MeasureNode(const Node& node, NodeLayoutEntry& entry, float max_width,
                     MeasureViewportRange viewport = {}) const override
    {
        MockTextMeasurer::MeasureNode(node, entry, max_width, viewport);

        if (node.type != NodeType::CodeBlock) {
            return;
        }
        const size_t seed = node.SourceOffset();
        const auto seed32 = static_cast<uint32_t>(seed);
        const auto MakeTok = [seed32](uint32_t k) {
            return SyntaxToken{
                .start = seed32 + k * 10u,
                .length = (seed32 % 7u) + 1u + k,
                .type = static_cast<SyntaxTokenType>((seed32 + k) % 4u),
            };
        };
        entry.set_syntax_tokens({ MakeTok(0), MakeTok(1), MakeTok(2) });
    }
};

// Serial / Parallel の出力を比較。height (FLOAT_EQ) と layout_dirty / syntax_tokens を観測する。
// IDWriteTextLayout (entry.text_layout) は mock では nullptr 固定 / 実装では identity 不一致で
// equality が無意味なため除外。total_height_ は LayoutEngine 経由でないと
// 計算されないためスコープ外。
void ExpectBitExactEqual(const StressFixture& s, const StressFixture& p)
{
    ASSERT_EQ(s.nodes.size(), p.nodes.size());
    const size_t n = s.nodes.size();
    for (size_t i = 0; i < n; ++i) {
        EXPECT_FLOAT_EQ(s.cache[i].height, p.cache[i].height) << "i=" << i;
        EXPECT_EQ(s.cache[i].layout_dirty, p.cache[i].layout_dirty) << "i=" << i;

        const auto ts = s.cache[i].syntax_tokens();
        const auto tp = p.cache[i].syntax_tokens();
        ASSERT_EQ(ts.size(), tp.size()) << "syntax_tokens size mismatch i=" << i;
        for (size_t k = 0; k < ts.size(); ++k) {
            EXPECT_EQ(ts[k].start, tp[k].start) << "i=" << i << " k=" << k;
            EXPECT_EQ(ts[k].length, tp[k].length) << "i=" << i << " k=" << k;
            EXPECT_EQ(ts[k].type, tp[k].type) << "i=" << i << " k=" << k;
        }
    }
}

class ParallelMeasureStressTest : public ::testing::Test {
protected:
    MockTextMeasurerWithTokens mock_;
    Theme theme_;
    TaskScheduler task_scheduler_;

    void SetUp() override
    {
        theme_ = GetLightTheme();
        task_scheduler_.Init(2);
    }
    void TearDown() override
    {
        task_scheduler_.Shutdown();
    }
};

} // namespace

// ===== タスク 1: 22000 ノード stress + bit-exact 検証 =====

TEST_F(ParallelMeasureStressTest, MatchesSerialOutputOnLargeMixedFixture)
{
    constexpr size_t N = 22000;
    StressFixture s;
    s.Build(N, /*include_code_block=*/false, /*all_dirty=*/true);
    StressFixture p;
    p.Build(N, /*include_code_block=*/false, /*all_dirty=*/true);

    const int rs = MeasureAll(s.nodes, s.cache, theme_, mock_, nullptr);
    const int rp = MeasureAll(p.nodes, p.cache, theme_, mock_, &task_scheduler_);

    EXPECT_EQ(rs, 0);
    EXPECT_EQ(rp, 0);
    ExpectBitExactEqual(s, p);
}

// ===== タスク 2: CodeBlock の syntax_tokens 書き込み検証 =====

TEST_F(ParallelMeasureStressTest, CodeBlockSyntaxTokensAggregatedInOrder)
{
    constexpr size_t N = 2000;
    StressFixture s;
    s.Build(N, /*include_code_block=*/true, /*all_dirty=*/true);
    StressFixture p;
    p.Build(N, /*include_code_block=*/true, /*all_dirty=*/true);

    const int rs = MeasureAll(s.nodes, s.cache, theme_, mock_, nullptr);
    const int rp = MeasureAll(p.nodes, p.cache, theme_, mock_, &task_scheduler_);

    EXPECT_EQ(rs, 0);
    EXPECT_EQ(rp, 0);
    ExpectBitExactEqual(s, p);

    // 全 CodeBlock ノードが 3 トークンを持ち、token.start = source_offset + k*10 で並ぶこと。
    size_t code_block_count = 0;
    for (size_t i = 0; i < N; ++i) {
        if (p.nodes[i].type != NodeType::CodeBlock) {
            continue;
        }
        ++code_block_count;
        const auto tokens = p.cache[i].syntax_tokens();
        ASSERT_EQ(tokens.size(), 3u) << "i=" << i;
        const uint32_t seed = static_cast<uint32_t>(i);
        for (uint32_t k = 0; k < 3; ++k) {
            EXPECT_EQ(tokens[k].start, seed + k * 10u) << "i=" << i << " k=" << k;
        }
    }
    // 含有率 10% (bucket 0,1 = N/10) を確認。fixture 構成変更時の sentinel。
    EXPECT_EQ(code_block_count, N / 10);
}

// ===== タスク 3: worker 数変動テスト =====

class ParallelWorkerCountTest : public ::testing::TestWithParam<int> {
protected:
    MockTextMeasurerWithTokens mock_;
    Theme theme_;

    void SetUp() override
    {
        theme_ = GetLightTheme();
    }
};

TEST_P(ParallelWorkerCountTest, BitExactAcrossWorkerCount)
{
    const int worker_count = GetParam();
    TaskScheduler ts;
    ts.Init(worker_count);

    constexpr size_t N = 2000;
    StressFixture s;
    s.Build(N, /*include_code_block=*/true, /*all_dirty=*/true);
    StressFixture p;
    p.Build(N, /*include_code_block=*/true, /*all_dirty=*/true);

    const int rs = MeasureAll(s.nodes, s.cache, theme_, mock_, nullptr);
    const int rp = MeasureAll(p.nodes, p.cache, theme_, mock_, &ts);

    EXPECT_EQ(rs, 0);
    EXPECT_EQ(rp, 0);
    ExpectBitExactEqual(s, p);

    ts.Shutdown();
}

INSTANTIATE_TEST_SUITE_P(WorkerCounts, ParallelWorkerCountTest, ::testing::Values(1, 2, 8, 16));

// ===== タスク 4: Post 失敗 fallback テスト =====

TEST(ParallelMeasurePostFailure, AllChunksFallbackOnSaturatedQueue)
{
    // worker 0 個で Init し、queue を MAX_PENDING_TASKS まで dummy で埋める。
    // MeasureIndicesParallel の Post 試行はすべて false になり、fallback (UI スレッドで run_chunk 直接実行) を通る。
    // dummy task は capture なし lambda なので Shutdown 時の dtor で安全に解放される (実行されない)。
    TaskScheduler ts;
    ts.Init(0);
    EXPECT_EQ(ts.WorkerCount(), 0u);

    int posted = 0;
    for (size_t k = 0; k < TaskScheduler::MAX_PENDING_TASKS; ++k) {
        if (ts.Post([] static noexcept {})) {
            ++posted;
        }
    }
    EXPECT_EQ(posted, static_cast<int>(TaskScheduler::MAX_PENDING_TASKS));
    EXPECT_FALSE(ts.Post([] static noexcept {})); // 1025 個目は false

    constexpr size_t N = 128;
    StressFixture f;
    f.Build(N, /*include_code_block=*/false, /*all_dirty=*/true);

    MockTextMeasurerWithTokens mock;
    const Theme& theme = GetLightTheme();
    const int r = MeasureAll(f.nodes, f.cache, theme, mock, &ts);

    EXPECT_EQ(r, 0);
    for (size_t i = 0; i < N; ++i) {
        EXPECT_FALSE(f.cache[i].layout_dirty) << "i=" << i;
        EXPECT_GT(f.cache[i].height, 0.0f) << "i=" << i;
    }

    ts.Shutdown();
}

// ===== タスク 5: ベンチ計測 (DISABLED_) =====

namespace {

// MockTextMeasurer に MeasureNode あたり ~spin_us μs の busy spin を仕込む派生。
// chunk 数や worker 数の効果を計測するための合成ワークロード生成器。
class SlowMockMeasurer : public MockTextMeasurer {
public:
    int spin_us = 100;

    void MeasureNode(const Node& node, NodeLayoutEntry& entry, float max_width,
                     MeasureViewportRange viewport = {}) const override
    {
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + std::chrono::microseconds(spin_us);
        // busy-spin は worker thread 上での MeasureNode 並列利得を観測するため。
        while (std::chrono::steady_clock::now() < deadline) {
            // no-op
        }
        MockTextMeasurer::MeasureNode(node, entry, max_width, viewport);
    }
};

double MeasureUs(auto&& f)
{
    const auto t0 = std::chrono::steady_clock::now();
    f();
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::micro>(t1 - t0).count();
}

} // namespace

// 通常 ctest では走らない。--gtest_also_run_disabled_tests --gtest_filter=*Bench* で起動。
TEST(ParallelMeasureBench, DISABLED_ChunkSizeSweep)
{
    SlowMockMeasurer mock;
    mock.spin_us = 100;
    const Theme& theme = GetLightTheme();
    TaskScheduler ts;
    ts.Init(4);

    std::cout << "N,mode,us\n";
    for (size_t N : { static_cast<size_t>(16), static_cast<size_t>(32),
                      static_cast<size_t>(64), static_cast<size_t>(128),
                      static_cast<size_t>(256), static_cast<size_t>(1024),
                      static_cast<size_t>(4096), static_cast<size_t>(22000) }) {
        StressFixture sf, pf;
        sf.Build(N, /*include_code_block=*/false, /*all_dirty=*/true);
        pf.Build(N, /*include_code_block=*/false, /*all_dirty=*/true);

        const double us_serial = MeasureUs([&] {
            MeasureAll(sf.nodes, sf.cache, theme, mock, nullptr);
        });
        const double us_parallel = MeasureUs([&] {
            MeasureAll(pf.nodes, pf.cache, theme, mock, &ts);
        });
        std::cout << N << ",serial," << us_serial << "\n";
        std::cout << N << ",parallel," << us_parallel << "\n";
    }

    ts.Shutdown();
}

// 実際の DirectWrite で全ノードを計測し、レイアウト用ワーカーの本数ごとの所要時間を出す。
// --gtest_also_run_disabled_tests --gtest_filter=*RealMeasureWorkerSweep で起動。
TEST(ParallelMeasureBench, DISABLED_RealMeasureWorkerSweep)
{
    auto bytes = ReadExampleFileBytes("example/test.md");
    if (bytes.empty()) {
        GTEST_SKIP() << "example/test.md not found";
    }
    std::pmr::string md;
    for (int i = 0; i < 8; i++) {
        md += bytes;
    }
    const auto doc = Document::FromMarkdown(std::move(md), L"bench.md");
    Microsoft::WRL::ComPtr<IDWriteFactory> factory;
    ASSERT_TRUE(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(factory.GetAddressOf()))));
    const Theme theme = GetLightTheme();

    // cold はワーカーのファクトリと書式の作成を含む。warm は同じワーカーでの 2 回目。
    std::cout << "nodes=" << doc.GetNodes().size() << "\nworkers,cold_ms,warm_ms\n";
    for (const int workers : { 0, 0, 2, 4, 8, 12, 16 }) {
        TaskScheduler ts;
        ts.Init(workers);
        DWriteTextMeasurer measurer;
        measurer.SetFactory(factory.Get());
        LayoutEngine engine;
        ASSERT_TRUE(engine.Init(&measurer, theme));
        engine.SetLayoutScheduler(workers > 0 ? &ts : nullptr);
        measurer.SetScheduler(workers > 0 ? &ts : nullptr);
        LayoutCache cold_cache;
        const double cold_us = MeasureUs([&] { ComputeLayout(engine, doc.GetNodes(), cold_cache, 1200.0f); });
        LayoutCache warm_cache;
        const double warm_us = MeasureUs([&] { ComputeLayout(engine, doc.GetNodes(), warm_cache, 1200.0f); });
        engine.SetLayoutScheduler(nullptr);
        measurer.SetScheduler(nullptr);
        ts.Shutdown();
        std::cout << workers << "," << cold_us / 1000.0 << "," << warm_us / 1000.0 << "\n";
    }
}

namespace {

// 2 万行 6 列の表。日本語・右揃え・中央揃えを含み、中身はほぼ行ごとに違う。
std::pmr::string MakeBenchTableMd()
{
    std::pmr::string md = "| 名前 | 状態 | 説明 | 数量 | 単価 | 備考 |\n|---|---|---|--:|--:|:-:|\n";
    for (size_t r = 0; r < 20000; ++r) {
        const auto n = std::to_string(r);
        md += "| 項目" + n + " | 状態" + std::to_string(r % 97) + " | 説明のテキスト " + n + " を少し長めに書く | " + n + " | " +
              std::to_string(r * 7 % 1000) + " | 備考" + n + " |\n";
    }
    return md;
}

} // namespace

// 実際の DirectWrite で測るベンチ。UseWorkers でワーカー 8 本を計測器とエンジンに結ぶ。
class DWriteMeasureBench : public DWriteTestBase {
protected:
    void UseWorkers()
    {
        scheduler_.Init(8);
        measurer_.SetScheduler(&scheduler_);
        engine_.SetLayoutScheduler(&scheduler_);
    }
    void TearDown() override
    {
        engine_.SetLayoutScheduler(nullptr);
        measurer_.SetScheduler(nullptr);
        scheduler_.Shutdown();
    }

    TaskScheduler scheduler_;
};

// 大きな表 (evict 前提) の初回計測、1 画面分の行の復元、幅の変更の所要時間を出す。
// --gtest_also_run_disabled_tests --gtest_filter=*TableMeasureBench* で起動。
TEST_F(DWriteMeasureBench, DISABLED_TableMeasureBench)
{
    const auto doc = Document::FromMarkdown(MakeBenchTableMd(), L"bench.md");
    const auto& nodes = doc.GetNodes();
    const auto it = std::ranges::find(nodes, NodeType::Table, &Node::type);
    ASSERT_NE(it, nodes.end());
    UseWorkers();

    for (int round = 0; round < 2; ++round) {
        NodeLayoutEntry entry;
        const double initial_us = MeasureUs([&] { measurer_.MeasureNode(*it, entry, 1200.0f, {}); });
        ASSERT_NE(entry.table_layout(), nullptr);
        // 1 画面 (1000 DIP) 分の行を、表のあちこちで 20 回復元する。
        double restore_us = 0.0;
        for (int k = 0; k < 20; ++k) {
            const float top = entry.height * static_cast<float>(k) / 20.0f;
            restore_us += MeasureUs([&] { measurer_.RestoreEvictedTableRows(*it, entry, 1200.0f, { top, top + 1000.0f }); });
        }
        entry.layout_dirty = true;
        const double resize_us = MeasureUs([&] { measurer_.MeasureNode(*it, entry, 1000.0f, { 0.0f, 1000.0f }); });
        std::cout << "round" << round << ": initial_ms=" << initial_us / 1000.0 << " restore_avg_ms=" << restore_us / 20000.0
                  << " resize_ms=" << resize_us / 1000.0 << "\n";
    }
}

// 全ノードを計測したあと幅を変えたときの再計測の所要時間 (直列)。
// --gtest_also_run_disabled_tests --gtest_filter=*ResizeRemeasureBench* で起動。
TEST_F(DWriteMeasureBench, DISABLED_ResizeRemeasureBench)
{
    for (const char* path : { "example/test.md", "../README.md" }) {
        auto bytes = ReadExampleFileBytes(path);
        if (bytes.empty()) {
            continue;
        }
        const auto doc = Document::FromMarkdown(std::move(bytes), L"bench.md");
        LayoutCache cache;
        ComputeLayout(engine_, doc.GetNodes(), cache, 1200.0f);
        std::cout << path << ":";
        for (const float width : { 1100.0f, 1000.0f, 1300.0f }) {
            const double us = MeasureUs([&] { ComputeLayout(engine_, doc.GetNodes(), cache, width); });
            std::cout << " w" << width << "=" << us / 1000.0 << "ms";
        }
        std::cout << "\n";
    }
}

// 大きな表の上下 8 画面分の行が残っている状態で幅を変えたときの、可視範囲のレイアウトの所要時間。
// --gtest_also_run_disabled_tests --gtest_filter=*TableResizeViewportBench* で起動。
TEST_F(DWriteMeasureBench, DISABLED_TableResizeViewportBench)
{
    constexpr float kScreen = 1000.0f;
    const auto doc = Document::FromMarkdown(MakeBenchTableMd(), L"bench.md");
    UseWorkers();
    ViewportManager viewport;
    LayoutService service(engine_, viewport);

    for (const float width : { 1100.0f, 1000.0f, 1200.0f, 900.0f }) {
        LayoutCache cache;
        ComputeLayout(engine_, doc.GetNodes(), cache, 1300.0f, 0.0f, kScreen);
        const float mid = cache.Bottom(cache.size() - 1) / 2.0f;
        engine_.EnsureVisibleLayout(doc.GetNodes(), cache, 1300.0f, mid - 8.0f * kScreen, mid + 9.0f * kScreen);
        viewport.SetScrollY(mid);
        const double us = MeasureUs([&] { service.ViewportLayout(doc, cache, width, kScreen); });
        std::cout << "w" << width << ": " << us / 1000.0 << "ms\n";
    }
}

// 遅延レイアウトを期限で区切ったときの、全 dirty を計測し終えるまでの所要時間。
// --gtest_also_run_disabled_tests --gtest_filter=*DirtyBatchDeadlineBench* で起動。
TEST_F(DWriteMeasureBench, DISABLED_DirtyBatchDeadlineBench)
{
    const auto bytes = ReadExampleFileBytes("example/test.md");
    if (bytes.empty()) {
        GTEST_SKIP() << "example/test.md not found";
    }
    std::pmr::string md;
    for (int i = 0; i < 4; i++) {
        md += bytes;
    }
    const auto doc = Document::FromMarkdown(std::move(md), L"bench.md");
    UseWorkers();
    for (const int budget_ms : { 4, 0, 4, 0 }) {
        LayoutCache cache;
        engine_.Reestimate(doc.GetNodes(), cache, 1200.0f);
        const double us = MeasureUs([&] {
            const auto deadline = [&] {
                return budget_ms > 0 ? std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms) : kNoLayoutDeadline;
            };
            while (engine_.ProcessDirtyBatch(doc.GetNodes(), cache, 1200.0f, deadline(), {})) {
            }
        });
        std::cout << "budget" << budget_ms << "ms: " << us / 1000.0 << "ms\n";
    }
}

// 大きな文書の末尾近くを編集して保存したときの、リロードの UI スレッド側の処理の所要時間。
// --gtest_also_run_disabled_tests --gtest_filter=*ReloadCarryOverBench* で起動。
TEST(ParallelMeasureBench, DISABLED_ReloadCarryOverBench)
{
    auto bytes = ReadExampleFileBytes("example/test.md");
    if (bytes.empty()) {
        GTEST_SKIP() << "example/test.md not found";
    }
    std::pmr::string old_md;
    while (old_md.size() < 100u * 1024 * 1024) {
        old_md += bytes;
    }
    std::pmr::string new_md = old_md;
    new_md.insert(new_md.size() - 200, "\n\n追記した段落。\n\n");
    const Theme theme = GetLightTheme();
    auto old_doc = Document::FromMarkdown(std::move(old_md), L"bench.md");
    auto new_doc = Document::FromMarkdown(std::move(new_md), L"bench.md");
    auto old_cache = layout::MakeEstimatedLayoutCache(old_doc.GetNodes(), theme, 800.0f);
    auto new_cache = layout::MakeEstimatedLayoutCache(new_doc.GetNodes(), theme, 800.0f);
    UnchangedEnds ends;
    const double find_us = MeasureUs([&] { ends = FindUnchangedEnds(old_doc.GetNodes(), new_doc.GetNodes()); });
    const double carry_us = MeasureUs([&] { layout::CarryOverUnchanged(old_doc.GetNodes(), old_cache, new_doc.GetNodes(), new_cache, ends); });
    const double recompute_us = MeasureUs([&] { layout::RecomputeYPositions(new_doc.GetNodes(), new_cache, theme); });
    std::cout << "nodes=" << new_doc.GetNodes().size() << " head=" << ends.head << " tail=" << ends.tail << " find_ms=" << find_us / 1000.0
              << " carry_ms=" << carry_us / 1000.0 << " recompute_ms=" << recompute_us / 1000.0 << "\n";
}
