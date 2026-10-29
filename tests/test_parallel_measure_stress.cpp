#include <gtest/gtest.h>
#include <numeric>
#include "document_types.h"
#include "layout_cache.h"
#include "layout_computer.h"
#include "mock_text_measurer.h"
#include "task_scheduler.h"
#include "test_helpers.h"
#include "theme.h"
#include "test_nodes.h"

namespace {

// 全ノードを MeasureIndicesParallel で計測し、計測できなかったノード数を返す。scheduler が null なら直列に計測する。
int MeasureAll(std::vector<Node>& nodes, LayoutCache& cache, const Theme& theme, ITextMeasurer& measurer, TaskScheduler* scheduler)
{
    std::vector<size_t> indices(nodes.size());
    std::ranges::iota(indices, size_t{ 0 });
    measurer.SetTheme(theme);
    measurer.SetScheduler(scheduler);
    const int left = layout::MeasureIndicesParallel(nodes, cache, 800.0f, measurer, indices, YRange::All(), kNoLayoutDeadline);
    measurer.SetScheduler(nullptr);
    return left;
}

// CodeBlock の syntax_tokens を書く派生 mock。並列経路では worker から entry へ直接書かれる。
class MockTextMeasurerWithTokens : public MockTextMeasurer {
public:
    std::optional<float> MeasureNode(const Node& node, NodeLayoutEntry& entry, float max_width,
                                     YRange viewport = YRange::All()) const override
    {
        const auto height = MockTextMeasurer::MeasureNode(node, entry, max_width, viewport);

        if (node.type != NodeType::CodeBlock) {
            return height;
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
        return height;
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
    s.Build(N, /*include_code_block=*/false);
    StressFixture p;
    p.Build(N, /*include_code_block=*/false);

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
    s.Build(N, /*include_code_block=*/true);
    StressFixture p;
    p.Build(N, /*include_code_block=*/true);

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
    s.Build(N, /*include_code_block=*/true);
    StressFixture p;
    p.Build(N, /*include_code_block=*/true);

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
    f.Build(N, /*include_code_block=*/false);

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

