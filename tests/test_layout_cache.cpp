#include <gtest/gtest.h>
#include "layout_cache.h"
#include "layout_computer.h"
#include "theme.h"
#include "test_helpers.h"

TEST(LayoutCacheTest, InvalidateAllLayouts)
{
    LayoutCache cache;
    cache.Resize(3);

    // レイアウトが設定された状態をシミュレート
    cache[0].effects_applied = true;
    cache[1].effects_applied = true;
    cache[2].effects_applied = true;
    cache[0].ensure_inline_code_bgs().emplace_back(0.0f, 0.0f, 10.0f, 10.0f);
    cache[1].ensure_inline_code_bgs().emplace_back(0.0f, 0.0f, 20.0f, 20.0f);

    cache.InvalidateAllLayouts();

    for (size_t i = 0; i < 3; ++i) {
        EXPECT_FALSE(cache[i].effects_applied) << "index " << i;
        EXPECT_TRUE(cache[i].view_inline_code_bgs().empty()) << "index " << i;
        // text_layoutはComPtrで、Reset()するとnullになる
        EXPECT_EQ(cache[i].text_layout.Get(), nullptr) << "index " << i;
    }
}

TEST(LayoutCacheTest, InvalidateAllLayoutsPreservesPositions)
{
    LayoutCache cache;
    cache.Resize(2);
    cache.SetTop(0, 100.0f);
    cache[0].height = 50.0f;
    cache.SetTop(1, 150.0f);
    cache[1].height = 30.0f;

    cache.InvalidateAllLayouts();

    // 位置は保持されること
    EXPECT_FLOAT_EQ(cache.Top(0), 100.0f);
    EXPECT_FLOAT_EQ(cache[0].height, 50.0f);
    EXPECT_FLOAT_EQ(cache.Top(1), 150.0f);
    EXPECT_FLOAT_EQ(cache[1].height, 30.0f);
}

TEST(LayoutCacheTest, InvalidateEmptyCache)
{
    LayoutCache cache;
    cache.Resize(0);

    // クラッシュしないこと
    cache.InvalidateAllLayouts();
}

// ズーム時に使用されるInvalidateAllLayoutsがダイアグラムのビットマップ/サイズを
// 保持することを検証する（ズーム→復帰でMermaid図が消える問題の再発防止）。
TEST(LayoutCacheTest, InvalidateAllLayoutsPreservesDiagramEntries)
{
    LayoutCache cache;
    cache.Resize(2);

    // ダイアグラムエントリにサイズを設定（ビットマップは設定できないが、サイズで確認）
    cache.EnsureDiagram(0).width = 400.0f;
    cache.EnsureDiagram(0).height = 300.0f;
    cache.EnsureDiagram(1).width = 500.0f;
    cache.EnsureDiagram(1).height = 250.0f;

    cache.InvalidateAllLayouts();

    // ダイアグラムの幅・高さは保持されること
    EXPECT_FLOAT_EQ(cache.FindDiagram(0)->width, 400.0f);
    EXPECT_FLOAT_EQ(cache.FindDiagram(0)->height, 300.0f);
    EXPECT_FLOAT_EQ(cache.FindDiagram(1)->width, 500.0f);
    EXPECT_FLOAT_EQ(cache.FindDiagram(1)->height, 250.0f);
}

// FindDiagram は確保しない。読み取り・破棄経路で未ロードのノード分を確保させないため。
TEST(LayoutCacheTest, FindDiagramDoesNotAllocate)
{
    LayoutCache cache;
    cache.Resize(2);

    EXPECT_EQ(cache.FindDiagram(0), nullptr);

    cache.EnsureDiagram(1).width = 10.0f;
    ASSERT_NE(cache.FindDiagram(1), nullptr);
    EXPECT_FLOAT_EQ(cache.FindDiagram(1)->width, 10.0f);
    EXPECT_EQ(cache.FindDiagram(0), nullptr);
}

// テーマ変更等の全ダイアグラム無効化でエラー状態もクリアされ、
// 再試行の契機になることを検証する (issue #271)。
TEST(LayoutCacheTest, InvalidateAllDiagramBitmapsClearsError)
{
    LayoutCache cache;
    cache.Resize(2);

    cache.EnsureDiagram(0).error = L"Parse error on line 1";
    cache.EnsureDiagram(1).error = L"Parse error on line 3";

    cache.InvalidateAllDiagramBitmaps();

    EXPECT_TRUE(cache.FindDiagram(0)->error.empty());
    EXPECT_TRUE(cache.FindDiagram(1)->error.empty());
}

// 早期終了経路 (safe_exit_after で abs(text_top - y) < EPSILON ならスキップ) を通った後でも、
// 残りノードの text_top がフル計算と一致することを検証する。
TEST(LayoutCacheTest, RecomputeYPositionsEarlyExitKeepsTextTop)
{
    LayoutCache cache;
    cache.Resize(8);

    const Theme theme = MakeLayoutTestTheme();

    std::pmr::vector<Node> nodes;
    nodes.resize(8);
    for (auto& n : nodes) {
        n.type = NodeType::Paragraph;
    }

    mendo::layout::EstimateNodeHeights(nodes, cache, theme);
    std::vector<float> expected;
    for (size_t i = 0; i < cache.size(); ++i) {
        cache[i].layout_dirty = false;
        expected.push_back(cache.Top(i));
    }

    // safe_exit_after=2 で、index >= 3 のノードについて text_top と y が一致なら早期終了。
    // EstimateNodeHeights 直後なので一致するはず → 早期終了経路を通る。
    mendo::layout::RecomputeYPositions(nodes, cache, theme, 0, 2);

    for (size_t i = 0; i < cache.size(); ++i) {
        EXPECT_FLOAT_EQ(cache.Top(i), expected[i]) << "after early-exit, index " << i;
    }
}

// ---- NodeOffsetToScrollY ----

TEST(NodeOffsetToScrollYTest, ClampsNodeAndResult)
{
    EXPECT_FLOAT_EQ(NodeOffsetToScrollY(LayoutCache{}, 0, 50.0f), 0.0f);

    const auto cache = MakeUniformCache(3);
    struct Case {
        int node;
        float offset;
        float expected;
    };
    constexpr Case kCases[] = {
        { -1, 50.0f, 0.0f },
        { 1, 25.0f, 125.0f },
        // 文書が短くなった後の復元などで末尾を超えた node は最後のノードに寄せる
        { 10, 5.0f, 205.0f },
        { 0, -40.0f, 0.0f },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::Message() << c.node << "," << c.offset);
        EXPECT_FLOAT_EQ(NodeOffsetToScrollY(cache, c.node, c.offset), c.expected);
    }
}

// ---- ComputeVisibleNodeRange ----

TEST(ComputeVisibleNodeRangeTest, ReturnsNodesOverlappingRange)
{
    const auto cache = MakeUniformCache(10);
    const auto [first, last_plus_1] = ComputeVisibleNodeRange(cache, 10, 150.0f, 350.0f);
    EXPECT_EQ(first, 1u);
    EXPECT_EQ(last_plus_1, 4u);
}

TEST(ComputeVisibleNodeRangeTest, RangeBelowContentIsEmpty)
{
    const auto cache = MakeUniformCache(3);
    const auto [first, last_plus_1] = ComputeVisibleNodeRange(cache, 3, 500.0f, 800.0f);
    EXPECT_EQ(first, last_plus_1);
}

// ---- TableLayoutData の行範囲 ----

namespace {

// 高さ 10 の行が 3 行 ([0,10) [10,20) [20,30))。
TableLayoutData MakeThreeRowTable()
{
    TableLayoutData tl;
    tl.row_cum_y = { 0.0f, 10.0f, 20.0f, 30.0f };
    return tl;
}

} // namespace

TEST(TableRowRangeTest, VisibleRowRangeWithoutGeometryIsEmpty)
{
    const TableLayoutData tl;
    EXPECT_EQ(tl.VisibleRowRange(0.0f, 100.0f), (std::pair<size_t, size_t>{ 0, 0 }));
}

TEST(TableRowRangeTest, VisibleRowRangeSelectsOverlappingRows)
{
    const auto tl = MakeThreeRowTable();
    EXPECT_EQ(tl.VisibleRowRange(15.0f, 25.0f), (std::pair<size_t, size_t>{ 1, 3 }));
    EXPECT_EQ(tl.VisibleRowRange(-50.0f, 5.0f), (std::pair<size_t, size_t>{ 0, 1 }));
    EXPECT_EQ(tl.VisibleRowRange(-50.0f, 500.0f), (std::pair<size_t, size_t>{ 0, 3 }));
}

TEST(TableRowRangeTest, VisibleRowRangeBelowTableIsEmpty)
{
    const auto tl = MakeThreeRowTable();
    const auto [begin, end] = tl.VisibleRowRange(100.0f, 200.0f);
    EXPECT_GE(begin, end);
}

// 行幾何が行数と揃わない (未計測・evict 直後) ときは全行を対象にする。
TEST(TableRowRangeTest, RowsInViewportFallsBackToAllRowsWithoutGeometry)
{
    const auto tl = MakeThreeRowTable();
    EXPECT_EQ(tl.RowsInViewport(5, 15.0f, 25.0f), (std::pair<size_t, size_t>{ 0, 5 }));
    EXPECT_EQ(tl.RowsInViewport(3, 15.0f, 25.0f), (std::pair<size_t, size_t>{ 1, 3 }));
}

TEST(TableRowRangeTest, RowIndexAtBoundaries)
{
    const auto tl = MakeThreeRowTable();
    EXPECT_EQ(tl.RowIndexAt(-0.1f), -1);
    EXPECT_EQ(tl.RowIndexAt(0.0f), 0);
    EXPECT_EQ(tl.RowIndexAt(9.9f), 0);
    EXPECT_EQ(tl.RowIndexAt(10.0f), 1);
    EXPECT_EQ(tl.RowIndexAt(29.9f), 2);
    EXPECT_EQ(tl.RowIndexAt(30.0f), -1);
    EXPECT_EQ(TableLayoutData{}.RowIndexAt(0.0f), -1);
}
