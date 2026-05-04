#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory_resource>
#include <thread>
#include "dirty_scheduler.h"
#include "mock_text_measurer.h"
#include "parallel_measure.h"
#include "task_scheduler.h"
#include "test_helpers.h"
#include "theme.h"

using mendo::layout::DirtyBatchResult;
using mendo::layout::DirtyBudget;
using mendo::layout::DirtyScheduler;
using mendo::layout::RunParallel;
using mendo::layout::StopReason;
using mendo::layout::ViewportClip;

namespace {

struct ParallelFixture {
    std::pmr::vector<Node> nodes;
    LayoutCache cache;

    void Build(size_t n, bool all_dirty)
    {
        for (size_t i = 0; i < n; ++i) {
            nodes.push_back(MakeTextNode(L"x"));
        }
        cache.Resize(n);
        // Paragraph の sa=0 なので block_height=100 で text_top(i) = i * 100 を再現する。
        std::pmr::vector<float> bh;
        bh.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            cache[i].text_top = static_cast<float>(i) * 100.0f;
            cache[i].height = 80.0f;
            cache[i].layout_dirty = all_dirty;
            bh.push_back(100.0f);
        }
        if (n > 0) {
            cache.BuildBlockHeights(std::span<const float>(bh.data(), bh.size()));
        }
    }
};

class ParallelMeasureTest : public ::testing::Test {
protected:
    MockTextMeasurer mock_;
    Theme theme_;
    DirtyScheduler scheduler_;
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

TEST_F(ParallelMeasureTest, EmptyDirtyReturnsNoneDirty)
{
    ParallelFixture f;
    f.Build(10, false);
    const auto r = RunParallel(f.nodes, f.cache, 800.0f, theme_, mock_,
                               ViewportClip{}, DirtyBudget{}, task_scheduler_);
    EXPECT_EQ(r.processed, 0);
    EXPECT_EQ(r.reason, StopReason::NoneDirty);
}

TEST_F(ParallelMeasureTest, MatchesSerialOutputOnSmallFixture)
{
    // 同じ fixture を 2 つ作り、片方を Serial、片方を Parallel に通して
    // entry.height / layout_dirty / total processed が一致することを確認する。
    constexpr size_t N = 200;
    ParallelFixture f_serial;
    f_serial.Build(N, true);
    ParallelFixture f_parallel;
    f_parallel.Build(N, true);

    const auto r_serial = scheduler_.RunSerial(f_serial.nodes, f_serial.cache, 800.0f, theme_, mock_,
                                               ViewportClip{}, DirtyBudget{});
    const auto r_parallel = RunParallel(f_parallel.nodes, f_parallel.cache, 800.0f, theme_, mock_,
                                        ViewportClip{}, DirtyBudget{}, task_scheduler_);

    EXPECT_EQ(r_serial.processed, r_parallel.processed);
    EXPECT_EQ(r_serial.first_processed, r_parallel.first_processed);
    EXPECT_EQ(r_serial.last_processed, r_parallel.last_processed);
    EXPECT_EQ(r_serial.reason, r_parallel.reason);

    for (size_t i = 0; i < N; ++i) {
        EXPECT_FLOAT_EQ(f_serial.cache[i].height, f_parallel.cache[i].height) << "i=" << i;
        EXPECT_EQ(f_serial.cache[i].layout_dirty, f_parallel.cache[i].layout_dirty) << "i=" << i;
    }
}

TEST_F(ParallelMeasureTest, ChunkBoundary)
{
    // chunk_size=256 を跨ぐサイズで取り漏れが出ないこと。
    // 256-1, 256, 256+1, 512+1 の前後で挙動が変わらないか確認。
    for (size_t N : { static_cast<size_t>(255), static_cast<size_t>(256),
                      static_cast<size_t>(257), static_cast<size_t>(513) }) {
        ParallelFixture f;
        f.Build(N, true);
        const auto r = RunParallel(f.nodes, f.cache, 800.0f, theme_, mock_,
                                   ViewportClip{}, DirtyBudget{}, task_scheduler_);
        EXPECT_EQ(r.processed, static_cast<int>(N)) << "N=" << N;
        for (size_t i = 0; i < N; ++i) {
            EXPECT_FALSE(f.cache[i].layout_dirty) << "i=" << i << " N=" << N;
        }
    }
}

TEST_F(ParallelMeasureTest, ViewportClipSkipsOffscreen)
{
    // 0..99 のうち、viewport [200, 600] に重なる buffer 圏内の dirty だけ処理される。
    // buffer_screens=1 なので clip [200-400, 600+400] = [-200, 1000] → y_pos が
    // この区間に含まれるノード (0..9) が対象になる。
    ParallelFixture f;
    f.Build(100, true);
    ViewportClip clip{ 200.0f, 400.0f, 1.0f };
    const auto r = RunParallel(f.nodes, f.cache, 800.0f, theme_, mock_,
                               clip, DirtyBudget{}, task_scheduler_);
    // y_position[i] = i*100, height=80。clip [-200, 1000] に重なるのは i=0..10
    EXPECT_GT(r.processed, 0);
    EXPECT_LE(r.processed, 11);
    // 範囲外の i=50 は dirty のまま残るはず
    EXPECT_TRUE(f.cache[50].layout_dirty);
}

TEST_F(ParallelMeasureTest, BatchLimitClampsProcessed)
{
    ParallelFixture f;
    f.Build(100, true);
    const auto r = RunParallel(f.nodes, f.cache, 800.0f, theme_, mock_,
                               ViewportClip{}, DirtyBudget{ 10, 0 }, task_scheduler_);
    EXPECT_EQ(r.processed, 10);
    EXPECT_EQ(r.reason, StopReason::BatchLimit);
    EXPECT_TRUE(r.any_nearby_skipped);
}

TEST_F(ParallelMeasureTest, AllDirtyClearedAfterRun)
{
    // 散在 dirty を含む大きめのケースで、全 dirty が処理 (= layout_dirty=false 化) されること。
    constexpr size_t N = 1000;
    ParallelFixture f;
    f.Build(N, true);
    const auto r = RunParallel(f.nodes, f.cache, 800.0f, theme_, mock_,
                               ViewportClip{}, DirtyBudget{}, task_scheduler_);
    EXPECT_EQ(r.processed, static_cast<int>(N));
    for (size_t i = 0; i < N; ++i) {
        EXPECT_FALSE(f.cache[i].layout_dirty) << "i=" << i;
        EXPECT_GT(f.cache[i].height, 0.0f) << "i=" << i;
    }
}
