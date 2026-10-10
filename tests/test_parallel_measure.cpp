#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <thread>
#include "dirty_node_fixture.h"
#include "document_test_helpers.h"
#include "layout.h"
#include "layout_invariants.h"
#include "mock_text_measurer.h"
#include "parser.h"
#include "parallel_measure.h"
#include "task_scheduler.h"
#include "test_helpers.h"
#include "theme.h"

using mendo::layout::DirtyBatchResult;
using mendo::layout::RunParallel;
using mendo::layout::StopReason;
using mendo::layout::ViewportClip;

namespace {

class ParallelMeasureTest : public ::testing::Test {
protected:
    MockTextMeasurer mock_;
    Theme theme_{};
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

// scheduler なし (呼び出しスレッドで直列計測) の RunParallel 用。
class RunParallelSerialTest : public ::testing::Test {
protected:
    MockTextMeasurer mock_;
    Theme theme_{};

    void SetUp() override
    {
        theme_ = GetLightTheme();
    }
};

class ThrowingMeasurer : public MockTextMeasurer {
public:
    const Node* throw_on = nullptr;
    // throw_on の計測を最初の throw_times 回だけ失敗させる。worker から呼ばれるので試行回数は atomic。
    int throw_times = std::numeric_limits<int>::max();
    mutable std::atomic<int> attempts{ 0 };

    void MeasureNode(Node& node, NodeLayoutEntry& entry, float max_width,
                     MeasureViewportRange viewport = {}) const override
    {
        if (&node == throw_on && attempts.fetch_add(1) < throw_times) {
            throw std::runtime_error("measure failed");
        }
        MockTextMeasurer::MeasureNode(node, entry, max_width, viewport);
    }
};

// ProcessDirtyBatch を残り dirty なしまで回し、回したバッチ数を返す。収束しなければ失敗にする。
int DrainDirtyBatches(LayoutEngine& engine, std::pmr::vector<Node>& nodes, LayoutCache& cache)
{
    constexpr int kMaxBatches = 20;
    for (int batches = 1; batches <= kMaxBatches; ++batches) {
        if (!engine.ProcessDirtyBatch(nodes, cache, 800.0f, 200)) {
            return batches;
        }
    }
    ADD_FAILURE() << "ProcessDirtyBatch が収束しない";
    return kMaxBatches;
}

} // namespace

TEST_F(ParallelMeasureTest, EmptyDirtyReturnsNoneDirty)
{
    DirtyNodeFixture f;
    f.Build(10, false);
    const auto r = RunParallel(f.nodes, f.cache, 800.0f, theme_, mock_,
                               ViewportClip{}, 0, &task_scheduler_);
    EXPECT_EQ(r.processed, 0);
    EXPECT_EQ(r.reason, StopReason::NoneDirty);
}

TEST_F(ParallelMeasureTest, MatchesSerialOutputOnSmallFixture)
{
    // 同じ fixture を 2 つ作り、片方を Serial、片方を Parallel に通して
    // entry.height / layout_dirty / total processed が一致することを確認する。
    constexpr size_t N = 200;
    DirtyNodeFixture f_serial;
    f_serial.Build(N, true);
    DirtyNodeFixture f_parallel;
    f_parallel.Build(N, true);

    const auto r_serial = RunParallel(f_serial.nodes, f_serial.cache, 800.0f, theme_, mock_,
                                      ViewportClip{}, 0, nullptr);
    const auto r_parallel = RunParallel(f_parallel.nodes, f_parallel.cache, 800.0f, theme_, mock_,
                                        ViewportClip{}, 0, &task_scheduler_);

    EXPECT_EQ(r_serial.processed, r_parallel.processed);
    EXPECT_EQ(r_serial.first_processed, r_parallel.first_processed);
    EXPECT_EQ(r_serial.last_processed, r_parallel.last_processed);
    EXPECT_EQ(r_serial.reason, r_parallel.reason);

    for (size_t i = 0; i < N; ++i) {
        EXPECT_FLOAT_EQ(f_serial.cache[i].height, f_parallel.cache[i].height) << "i=" << i;
        EXPECT_EQ(f_serial.cache[i].layout_dirty, f_parallel.cache[i].layout_dirty) << "i=" << i;
    }
}

TEST_F(ParallelMeasureTest, ViewportClipSkipsOffscreen)
{
    // 0..99 のうち、viewport [200, 600] に重なる buffer 圏内の dirty だけ処理される。
    // buffer_screens=1 なので clip [200-400, 600+400] = [-200, 1000] → y_pos が
    // この区間に含まれるノード (0..9) が対象になる。
    DirtyNodeFixture f;
    f.Build(100, true);
    ViewportClip clip{ 200.0f, 400.0f, 1.0f };
    const auto r = RunParallel(f.nodes, f.cache, 800.0f, theme_, mock_,
                               clip, 0, &task_scheduler_);
    // y_position[i] = i*100, height=80。clip [-200, 1000] に重なるのは i=0..10
    EXPECT_GT(r.processed, 0);
    EXPECT_LE(r.processed, 11);
    // 範囲外の i=50 は dirty のまま残るはず
    EXPECT_TRUE(f.cache[50].layout_dirty);
}

TEST_F(ParallelMeasureTest, BatchLimitClampsProcessed)
{
    DirtyNodeFixture f;
    f.Build(100, true);
    const auto r = RunParallel(f.nodes, f.cache, 800.0f, theme_, mock_,
                               ViewportClip{}, 10, &task_scheduler_);
    EXPECT_EQ(r.processed, 10);
    EXPECT_EQ(r.reason, StopReason::BatchLimit);
    EXPECT_TRUE(r.any_nearby_skipped());
}

TEST_F(ParallelMeasureTest, AllDirtyClearedAfterRun)
{
    // 散在 dirty を含む大きめのケースで、全 dirty が処理 (= layout_dirty=false 化) されること。
    constexpr size_t N = 1000;
    DirtyNodeFixture f;
    f.Build(N, true);
    const auto r = RunParallel(f.nodes, f.cache, 800.0f, theme_, mock_,
                               ViewportClip{}, 0, &task_scheduler_);
    EXPECT_EQ(r.processed, static_cast<int>(N));
    for (size_t i = 0; i < N; ++i) {
        EXPECT_FALSE(f.cache[i].layout_dirty) << "i=" << i;
        EXPECT_GT(f.cache[i].height, 0.0f) << "i=" << i;
    }
}

// EnsureVisibleLayout も scheduler があれば可視 dirty を並列計測し、直列と同じ結果になる。
TEST_F(ParallelMeasureTest, EnsureVisibleLayoutParallelMatchesSerial)
{
    std::string md;
    for (int i = 0; i < 300; i++) {
        md += "Paragraph " + std::to_string(i) + " " + std::string(static_cast<size_t>(i % 50), 'w') + "\n\n";
    }
    const auto build = [&](LayoutEngine& engine, std::pmr::vector<Node>& nodes, LayoutCache& cache) {
        nodes = ParseMarkdown(md).nodes;
        cache.Resize(nodes.size());
        EstimateNodeHeights(nodes, cache, theme_);
        engine.EnsureVisibleLayout(nodes, cache, 800.0f, 0.0f, 100000.0f);
    };

    LayoutEngine serial;
    ASSERT_TRUE(serial.Init(&mock_, theme_));
    std::pmr::vector<Node> s_nodes;
    LayoutCache s_cache;
    build(serial, s_nodes, s_cache);

    LayoutEngine parallel;
    ASSERT_TRUE(parallel.Init(&mock_, theme_));
    parallel.SetLayoutScheduler(&task_scheduler_);
    std::pmr::vector<Node> p_nodes;
    LayoutCache p_cache;
    build(parallel, p_nodes, p_cache);
    parallel.SetLayoutScheduler(nullptr);

    ASSERT_EQ(s_nodes.size(), p_nodes.size());
    for (size_t i = 0; i < s_nodes.size(); ++i) {
        EXPECT_FLOAT_EQ(s_cache[i].height, p_cache[i].height) << "i=" << i;
        EXPECT_FLOAT_EQ(s_cache.Top(i), p_cache.Top(i)) << "i=" << i;
        EXPECT_EQ(s_cache[i].layout_dirty, p_cache[i].layout_dirty) << "i=" << i;
    }
}

// 並列計測の chunk が例外で落ちても、落ちる前に計測できたノードの高さを Y に反映し、
// 計測できなかった dirty は HasDirtyNodes() で次回の再試行に回す。
TEST_F(ParallelMeasureTest, ProcessDirtyBatchRetriesAfterChunkException)
{
    struct Case {
        size_t dirty_count;
        size_t throw_at;
        bool clip;
    };
    constexpr size_t kFirstDirty = 5;
    const std::string md = MakeParagraphs(60);
    for (const Case c : { Case{ 10, 9, false }, Case{ 10, 9, true }, Case{ 10, 0, true }, Case{ 40, 20, false }, Case{ 40, 20, true } }) {
        SCOPED_TRACE("dirty=" + std::to_string(c.dirty_count) + " throw_at=" + std::to_string(c.throw_at) + " clip=" + std::to_string(c.clip));
        ThrowingMeasurer measurer;
        LayoutEngine engine;
        ASSERT_TRUE(engine.Init(&measurer, theme_));
        auto nodes = ParseMarkdown(md).nodes;
        LayoutCache cache;
        cache.Resize(nodes.size());
        engine.ComputeLayout(nodes, cache, 800.0f);
        ASSERT_FALSE(engine.HasDirtyNodes());

        // 再計測で高さが変わるようにして、計測済みノードの Y 反映漏れを見えるようにする。
        measurer.line_height *= 1.5f;
        for (size_t k = 0; k < c.dirty_count; ++k) {
            cache[kFirstDirty + k].layout_dirty = true;
        }
        const size_t thrower = kFirstDirty + c.throw_at;
        measurer.throw_on = &nodes[thrower];

        engine.SetLayoutScheduler(&task_scheduler_);
        const bool more = c.clip
            ? engine.ProcessDirtyBatch(nodes, cache, 800.0f, 200, { 0.0f, 600.0f, 100.0f })
            : engine.ProcessDirtyBatch(nodes, cache, 800.0f, 200);
        engine.SetLayoutScheduler(nullptr);

        EXPECT_TRUE(cache[thrower].layout_dirty);
        EXPECT_TRUE(more);
        EXPECT_TRUE(engine.HasDirtyNodes()) << "計測できなかった dirty の再試行が止まる";
        EXPECT_TRUE(YChainConsistent(nodes, cache, theme_));
    }
}

// 常に例外を投げるノードは kMaxMeasureAttempts 回で諦めて dirty を外し、ダーティ処理を止める。
// 同じ chunk の他のノードは例外に巻き込まれず計測される。諦めた記録は全レイアウトの無効化で消え、再び試される。
TEST_F(ParallelMeasureTest, AlwaysThrowingNodeStopsRetryingAfterMaxAttempts)
{
    constexpr size_t kFirstDirty = 5;
    constexpr size_t kDirtyCount = 10;
    constexpr size_t kThrower = kFirstDirty + 3;
    ThrowingMeasurer measurer;
    LayoutEngine engine;
    ASSERT_TRUE(engine.Init(&measurer, theme_));
    LayoutSourceStore store;
    auto [nodes, cache] = store.ParseAndLayout(engine, MakeParagraphs(60), 800.0f);

    measurer.line_height *= 1.5f;
    for (size_t k = 0; k < kDirtyCount; ++k) {
        cache[kFirstDirty + k].layout_dirty = true;
    }
    measurer.throw_on = &nodes[kThrower];

    engine.SetLayoutScheduler(&task_scheduler_);
    ASSERT_TRUE(engine.ProcessDirtyBatch(nodes, cache, 800.0f, 200));
    for (size_t k = 0; k < kDirtyCount; ++k) {
        const size_t i = kFirstDirty + k;
        if (i != kThrower) {
            EXPECT_FALSE(cache[i].layout_dirty) << "i=" << i << " は例外を投げたノードと同じ chunk でも計測される";
        }
    }
    // RecomputeYPositions は末尾シフト時に dirty 残りを保守的に true と返すため、諦めた後に 1 バッチ余分に回りうる。
    EXPECT_LE(1 + DrainDirtyBatches(engine, nodes, cache), static_cast<int>(LayoutCache::kMaxMeasureAttempts) + 1);
    EXPECT_EQ(measurer.attempts.load(), LayoutCache::kMaxMeasureAttempts);
    EXPECT_FALSE(engine.HasDirtyNodes()) << "諦めた後もダーティ処理が続いている";
    EXPECT_FALSE(cache[kThrower].layout_dirty);
    EXPECT_TRUE(YChainConsistent(nodes, cache, theme_));

    cache.InvalidateAllLayouts();
    cache[kThrower].layout_dirty = true;
    DrainDirtyBatches(engine, nodes, cache);
    engine.SetLayoutScheduler(nullptr);
    EXPECT_EQ(measurer.attempts.load(), 2 * LayoutCache::kMaxMeasureAttempts) << "無効化後は改めて上限回数まで試す";
}

// 一時的な失敗は再試行で計測され、上限に達する前に成功すれば諦めない。成功すると失敗の記録は消える。
TEST_F(ParallelMeasureTest, TransientMeasureFailureIsRetriedUntilMeasured)
{
    constexpr size_t kThrower = 7;
    ThrowingMeasurer measurer;
    LayoutEngine engine;
    ASSERT_TRUE(engine.Init(&measurer, theme_));
    LayoutSourceStore store;
    auto [nodes, cache] = store.ParseAndLayout(engine, MakeParagraphs(30), 800.0f);
    const float measured_height = cache[kThrower].height;

    cache[kThrower].layout_dirty = true;
    cache[kThrower].height = 0.0f;
    measurer.throw_on = &nodes[kThrower];
    measurer.throw_times = LayoutCache::kMaxMeasureAttempts - 1;

    engine.SetLayoutScheduler(&task_scheduler_);
    DrainDirtyBatches(engine, nodes, cache);
    EXPECT_EQ(measurer.attempts.load(), LayoutCache::kMaxMeasureAttempts);
    EXPECT_FALSE(cache[kThrower].layout_dirty);
    EXPECT_FLOAT_EQ(cache[kThrower].height, measured_height) << "最後の試行で計測できている";
    EXPECT_TRUE(YChainConsistent(nodes, cache, theme_));

    // 成功で失敗の記録が消えるので、後で失敗し始めても改めて上限回数まで試す。
    measurer.throw_times = std::numeric_limits<int>::max();
    const int before = measurer.attempts.load();
    cache[kThrower].layout_dirty = true;
    DrainDirtyBatches(engine, nodes, cache);
    engine.SetLayoutScheduler(nullptr);
    EXPECT_EQ(measurer.attempts.load() - before, LayoutCache::kMaxMeasureAttempts);
}

// 失敗回数は MeasureEntry を通るどの経路で成功しても 0 に戻る (並列計測以外の成功で残ると、
// 以後の一時的な失敗 1 回で諦めてしまう)。
TEST_F(ParallelMeasureTest, MeasureSuccessOnAnyPathResetsFailureCount)
{
    constexpr size_t kThrower = 7;
    ThrowingMeasurer measurer;
    LayoutEngine engine;
    ASSERT_TRUE(engine.Init(&measurer, theme_));
    LayoutSourceStore store;
    auto [nodes, cache] = store.ParseAndLayout(engine, MakeParagraphs(30), 800.0f);

    cache[kThrower].layout_dirty = true;
    measurer.throw_on = &nodes[kThrower];
    measurer.throw_times = LayoutCache::kMaxMeasureAttempts - 1;
    engine.SetLayoutScheduler(&task_scheduler_);
    for (int k = 0; k + 1 < LayoutCache::kMaxMeasureAttempts; ++k) {
        engine.ProcessDirtyBatch(nodes, cache, 800.0f, 200);
    }
    engine.SetLayoutScheduler(nullptr);
    ASSERT_EQ(cache[kThrower].measure_failures, LayoutCache::kMaxMeasureAttempts - 1);
    ASSERT_TRUE(cache[kThrower].layout_dirty);

    // 幅変更の全レイアウトは ComputeLayout から直接計測する。
    engine.ComputeLayout(nodes, cache, 700.0f);
    EXPECT_FALSE(cache[kThrower].layout_dirty);
    EXPECT_EQ(cache[kThrower].measure_failures, 0);
}

TEST_F(RunParallelSerialTest, NoneDirtyReturnsNoneDirty)
{
    DirtyNodeFixture f;
    f.Build(5, false);
    const auto r = RunParallel(f.nodes, f.cache, 800.0f, theme_, mock_, ViewportClip{}, 0, nullptr);
    EXPECT_EQ(r.processed, 0);
    EXPECT_EQ(r.reason, StopReason::NoneDirty);
    EXPECT_FALSE(r.any_nearby_skipped());
}

TEST_F(RunParallelSerialTest, AllDirtyProcessedReturnsDone)
{
    DirtyNodeFixture f;
    f.Build(5, true);
    const auto r = RunParallel(f.nodes, f.cache, 800.0f, theme_, mock_, ViewportClip{}, 0, nullptr);
    EXPECT_EQ(r.processed, 5);
    EXPECT_EQ(r.reason, StopReason::Done);
    EXPECT_FALSE(r.any_nearby_skipped());
    EXPECT_EQ(r.first_processed, 0u);
    EXPECT_EQ(r.last_processed, 4u);
}

TEST_F(RunParallelSerialTest, BatchLimitStopsAtMaxNodes)
{
    DirtyNodeFixture f;
    f.Build(10, true);
    const auto r = RunParallel(f.nodes, f.cache, 800.0f, theme_, mock_, ViewportClip{}, 3, nullptr);
    EXPECT_EQ(r.processed, 3);
    EXPECT_EQ(r.reason, StopReason::BatchLimit);
    EXPECT_TRUE(r.any_nearby_skipped());
    EXPECT_EQ(r.first_processed, 0u);
    EXPECT_EQ(r.last_processed, 2u);
}

TEST_F(RunParallelSerialTest, ViewportClipSkipsOffscreenDirty)
{
    // 10 ノード (y=0,100,200,...,900)、buffer_screens=0、viewport=[150, 350)
    // Skip されないのは y_position が [150, 350) または overlap するノード。
    // ノード i の rect = [i*100, i*100+80]。viewport=[150,350]。
    // i=1: rect=[100,180], overlap with [150,350] → 含まれる
    // i=2: rect=[200,280], 含まれる
    // i=3: rect=[300,380], overlap → 含まれる
    // i=0,4..9: 含まれない
    DirtyNodeFixture f;
    f.Build(10, true);
    const auto r = RunParallel(f.nodes, f.cache, 800.0f, theme_, mock_,
                               ViewportClip{ 150.0f, 200.0f, 0.0f }, 0, nullptr);
    EXPECT_EQ(r.processed, 3);
    EXPECT_EQ(r.reason, StopReason::Done);
    EXPECT_FALSE(r.any_nearby_skipped());
    EXPECT_EQ(r.first_processed, 1u);
    EXPECT_EQ(r.last_processed, 3u);
}

TEST_F(RunParallelSerialTest, ViewportClipWithBufferIncludesNearbyDirty)
{
    // viewport=[300, 400), buffer_screens=1.0 (height=100) → 範囲 = [200, 500)
    // i=2: rect=[200,280] → overlap → 含む
    // i=3,4: 含む
    // i=5: rect=[500,580] → IsOffscreen 判定 (y >= range_bottom か y+h <= range_top)。
    //      range_bottom=500、5の y=500 → !(y < range_bottom) なので IsOffscreen=true → 含まない
    DirtyNodeFixture f;
    f.Build(10, true);
    const auto r = RunParallel(f.nodes, f.cache, 800.0f, theme_, mock_,
                               ViewportClip{ 300.0f, 100.0f, 1.0f }, 0, nullptr);
    EXPECT_GE(r.processed, 3);
    EXPECT_LE(r.processed, 4);
    EXPECT_EQ(r.reason, StopReason::Done);
}

TEST_F(RunParallelSerialTest, FirstLastProcessedTracking)
{
    DirtyNodeFixture f;
    f.Build(7, { 3, 4, 5 });
    const auto r = RunParallel(f.nodes, f.cache, 800.0f, theme_, mock_, ViewportClip{}, 0, nullptr);
    EXPECT_EQ(r.processed, 3);
    EXPECT_EQ(r.first_processed, 3u);
    EXPECT_EQ(r.last_processed, 5u);
    EXPECT_EQ(r.reason, StopReason::Done);
}

TEST_F(RunParallelSerialTest, BudgetZeroIsUnlimited)
{
    DirtyNodeFixture f;
    f.Build(50, true);
    const auto r = RunParallel(f.nodes, f.cache, 800.0f, theme_, mock_, ViewportClip{}, 0, nullptr);
    EXPECT_EQ(r.processed, 50);
    EXPECT_EQ(r.reason, StopReason::Done);
    EXPECT_FALSE(r.any_nearby_skipped());
}

TEST_F(RunParallelSerialTest, MeasureNodeIsCalledOnEachProcessed)
{
    // Mock の MeasureNode は entry.layout_dirty=false を書く。処理後 dirty=false になることで
    // 計測 callback がインスタンスごとに 1 回ずつ呼ばれたことを検証する。
    DirtyNodeFixture f;
    f.Build(5, true);
    const auto r = RunParallel(f.nodes, f.cache, 800.0f, theme_, mock_, ViewportClip{}, 0, nullptr);
    EXPECT_EQ(r.processed, 5);
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(f.cache[i].layout_dirty) << "node " << i << " should be cleaned";
    }
}

TEST_F(RunParallelSerialTest, NoClipProcessesAllDirtyEvenIfYUnreachable)
{
    // viewport_clip top<0 で全 dirty 対象。y_position の値に関わらず処理。
    DirtyNodeFixture f;
    f.Build(5, { 0, 4 });
    f.cache.SetTop(0, -1000.0f); // 大きく外れた値
    f.cache.SetTop(4, 999999.0f);
    const auto r = RunParallel(f.nodes, f.cache, 800.0f, theme_, mock_, ViewportClip{}, 0, nullptr);
    EXPECT_EQ(r.processed, 2);
    EXPECT_EQ(r.first_processed, 0u);
    EXPECT_EQ(r.last_processed, 4u);
    EXPECT_EQ(r.reason, StopReason::Done);
}
