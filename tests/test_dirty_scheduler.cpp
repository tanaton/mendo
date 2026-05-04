#include <gtest/gtest.h>
#include <memory_resource>
#include <thread>
#include "dirty_scheduler.h"
#include "mock_text_measurer.h"
#include "test_helpers.h"
#include "theme.h"

using mendo::layout::DirtyBatchResult;
using mendo::layout::DirtyBudget;
using mendo::layout::DirtyScheduler;
using mendo::layout::StopReason;
using mendo::layout::ViewportClip;

namespace {

// ノードを N 個 (Paragraph) 用意し、cache を Resize して y/height/dirty を初期化するヘルパ。
// y_position はノード i に対して i * 100、height は 80、layout_dirty は dirty_indices に含まれるものだけ true。
struct DirtyFixture {
    std::pmr::vector<Node> nodes;
    LayoutCache cache;

    void Build(size_t n, std::initializer_list<size_t> dirty_indices)
    {
        for (size_t i = 0; i < n; ++i) {
            nodes.push_back(MakeTextNode(L"x"));
        }
        cache.Resize(n);
        // Paragraph の spacing_above は 0 なので text_top = block_top = PrefixSum。
        // 各ノードの block_height = 100 で text_top(i) = i * 100 を再現する。
        std::pmr::vector<float> bh;
        bh.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            cache[i].text_top = static_cast<float>(i) * 100.0f;
            cache[i].height = 80.0f;
            cache[i].layout_dirty = false;
            bh.push_back(100.0f);
        }
        if (n > 0) {
            cache.BuildBlockHeights(std::span<const float>(bh.data(), bh.size()));
        }
        for (size_t idx : dirty_indices) {
            cache[idx].layout_dirty = true;
        }
    }
};

class DirtySchedulerTest : public ::testing::Test {
protected:
    MockTextMeasurer mock_;
    Theme theme_;
    DirtyScheduler scheduler_;

    void SetUp() override
    {
        theme_ = GetLightTheme();
    }
};

} // namespace

TEST_F(DirtySchedulerTest, NoneDirtyReturnsNoneDirty)
{
    DirtyFixture f;
    f.Build(5, {});
    const auto r = scheduler_.RunSerial(f.nodes, f.cache, 800.0f, theme_, mock_, ViewportClip{}, DirtyBudget{});
    EXPECT_EQ(r.processed, 0);
    EXPECT_EQ(r.reason, StopReason::NoneDirty);
    EXPECT_FALSE(r.any_nearby_skipped);
}

TEST_F(DirtySchedulerTest, AllDirtyProcessedReturnsDone)
{
    DirtyFixture f;
    f.Build(5, { 0, 1, 2, 3, 4 });
    const auto r = scheduler_.RunSerial(f.nodes, f.cache, 800.0f, theme_, mock_, ViewportClip{}, DirtyBudget{});
    EXPECT_EQ(r.processed, 5);
    EXPECT_EQ(r.reason, StopReason::Done);
    EXPECT_FALSE(r.any_nearby_skipped);
    EXPECT_EQ(r.first_processed, 0u);
    EXPECT_EQ(r.last_processed, 4u);
}

TEST_F(DirtySchedulerTest, BatchLimitStopsAtMaxNodes)
{
    DirtyFixture f;
    f.Build(10, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 });
    const auto r = scheduler_.RunSerial(f.nodes, f.cache, 800.0f, theme_, mock_, ViewportClip{}, DirtyBudget{ 3, 0 });
    EXPECT_EQ(r.processed, 3);
    EXPECT_EQ(r.reason, StopReason::BatchLimit);
    EXPECT_TRUE(r.any_nearby_skipped);
    EXPECT_EQ(r.first_processed, 0u);
    EXPECT_EQ(r.last_processed, 2u);
}

TEST_F(DirtySchedulerTest, TimeBudgetGuaranteesProgressOfAtLeastOneNode)
{
    // time_us=1 (実質ゼロ) でも進行保証で 1 ノードは処理されること
    DirtyFixture f;
    f.Build(10, { 0, 1, 2, 3, 4 });
    const auto r = scheduler_.RunSerial(f.nodes, f.cache, 800.0f, theme_, mock_, ViewportClip{}, DirtyBudget{ 0, 1 });
    EXPECT_GE(r.processed, 1);
    // 処理が 1 件で打ち切られた場合: TimeBudget。全件入った場合: Done。
    if (r.processed < 5) {
        EXPECT_EQ(r.reason, StopReason::TimeBudget);
        EXPECT_TRUE(r.any_nearby_skipped);
    }
}

TEST_F(DirtySchedulerTest, ViewportClipSkipsOffscreenDirty)
{
    // 10 ノード (y=0,100,200,...,900)、buffer_screens=0、viewport=[150, 350)
    // Skip されないのは y_position が [150, 350) または overlap するノード。
    // ノード i の rect = [i*100, i*100+80]。viewport=[150,350]。
    // i=1: rect=[100,180], overlap with [150,350] → 含まれる
    // i=2: rect=[200,280], 含まれる
    // i=3: rect=[300,380], overlap → 含まれる
    // i=0,4..9: 含まれない
    DirtyFixture f;
    f.Build(10, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 });
    const auto r = scheduler_.RunSerial(f.nodes, f.cache, 800.0f, theme_, mock_,
                                        ViewportClip{ 150.0f, 200.0f, 0.0f }, DirtyBudget{});
    EXPECT_EQ(r.processed, 3);
    EXPECT_EQ(r.reason, StopReason::Done);
    EXPECT_FALSE(r.any_nearby_skipped);
    EXPECT_EQ(r.first_processed, 1u);
    EXPECT_EQ(r.last_processed, 3u);
}

TEST_F(DirtySchedulerTest, ViewportClipWithBufferIncludesNearbyDirty)
{
    // viewport=[300, 400), buffer_screens=1.0 (height=100) → 範囲 = [200, 500)
    // i=2: rect=[200,280] → overlap → 含む
    // i=3,4: 含む
    // i=5: rect=[500,580] → IsOffscreen 判定 (y >= range_bottom か y+h <= range_top)。
    //      range_bottom=500、5の y=500 → !(y < range_bottom) なので IsOffscreen=true → 含まない
    DirtyFixture f;
    f.Build(10, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 });
    const auto r = scheduler_.RunSerial(f.nodes, f.cache, 800.0f, theme_, mock_,
                                        ViewportClip{ 300.0f, 100.0f, 1.0f }, DirtyBudget{});
    EXPECT_GE(r.processed, 3);
    EXPECT_LE(r.processed, 4);
    EXPECT_EQ(r.reason, StopReason::Done);
}

TEST_F(DirtySchedulerTest, FirstLastProcessedTracking)
{
    DirtyFixture f;
    f.Build(7, { 3, 4, 5 });
    const auto r = scheduler_.RunSerial(f.nodes, f.cache, 800.0f, theme_, mock_, ViewportClip{}, DirtyBudget{});
    EXPECT_EQ(r.processed, 3);
    EXPECT_EQ(r.first_processed, 3u);
    EXPECT_EQ(r.last_processed, 5u);
    EXPECT_EQ(r.reason, StopReason::Done);
}

TEST_F(DirtySchedulerTest, BudgetZeroIsUnlimited)
{
    DirtyFixture f;
    f.Build(50, {});
    for (int i = 0; i < 50; ++i) {
        f.cache[i].layout_dirty = true;
    }
    const auto r = scheduler_.RunSerial(f.nodes, f.cache, 800.0f, theme_, mock_, ViewportClip{}, DirtyBudget{ 0, 0 });
    EXPECT_EQ(r.processed, 50);
    EXPECT_EQ(r.reason, StopReason::Done);
    EXPECT_FALSE(r.any_nearby_skipped);
}

TEST_F(DirtySchedulerTest, MeasureNodeIsCalledOnEachProcessed)
{
    // Mock の MeasureNode は entry.layout_dirty=false を書く。処理後 dirty=false になることで
    // 計測 callback がインスタンスごとに 1 回ずつ呼ばれたことを検証する。
    DirtyFixture f;
    f.Build(5, { 0, 1, 2, 3, 4 });
    const auto r = scheduler_.RunSerial(f.nodes, f.cache, 800.0f, theme_, mock_, ViewportClip{}, DirtyBudget{});
    EXPECT_EQ(r.processed, 5);
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(f.cache[i].layout_dirty) << "node " << i << " should be cleaned";
    }
}

TEST_F(DirtySchedulerTest, NoClipProcessesAllDirtyEvenIfYUnreachable)
{
    // viewport_clip top<0 で全 dirty 対象。y_position の値に関わらず処理。
    DirtyFixture f;
    f.Build(5, { 0, 4 });
    f.cache[0].text_top = -1000.0f; // 大きく外れた値
    f.cache[4].text_top = 999999.0f;
    const auto r = scheduler_.RunSerial(f.nodes, f.cache, 800.0f, theme_, mock_, ViewportClip{}, DirtyBudget{});
    EXPECT_EQ(r.processed, 2);
    EXPECT_EQ(r.first_processed, 0u);
    EXPECT_EQ(r.last_processed, 4u);
    EXPECT_EQ(r.reason, StopReason::Done);
}
