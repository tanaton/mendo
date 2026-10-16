#include <gtest/gtest.h>
#include "layout_cache.h"
#include "layout_computer.h"
#include "theme.h"
#include "test_helpers.h"

TEST(LayoutCacheTest, InvalidateAllLayouts)
{
    LayoutCache cache;
    cache.Resize(3);
    const std::pmr::vector<Node> nodes(3);

    // レイアウトが設定された状態をシミュレート
    cache[0].set_effects_applied(true);
    cache[1].set_effects_applied(true);
    cache[2].set_effects_applied(true);
    cache[0].ensure_inline_code_bgs().emplace_back(0.0f, 0.0f, 10.0f, 10.0f);
    cache[1].ensure_inline_code_bgs().emplace_back(0.0f, 0.0f, 20.0f, 20.0f);

    cache.InvalidateAllLayouts(nodes);

    for (size_t i = 0; i < 3; ++i) {
        EXPECT_FALSE(cache[i].effects_applied()) << "index " << i;
        EXPECT_TRUE(cache[i].view_inline_code_bgs().empty()) << "index " << i;
        // text_layoutはComPtrで、Reset()するとnullになる
        EXPECT_EQ(cache[i].text_layout(), nullptr) << "index " << i;
    }
}

// ズームで旧倍率の実測値が推定に使われると、画面外のコードブロック等の高さが古いまま残る。
TEST(LayoutCacheTest, InvalidateAllLayoutsForgetsMeasuredSize)
{
    LayoutCache cache;
    cache.Resize(1);
    cache[0].cached_width = 600.0f;
    cache[0].cached_height = 120.0f;

    cache.InvalidateAllLayouts(std::pmr::vector<Node>(1));

    EXPECT_FALSE(cache[0].is_measured());
    EXPECT_FLOAT_EQ(cache[0].cached_height, 0.0f);
}

TEST(LayoutCacheTest, InvalidateAllLayoutsPreservesPositions)
{
    LayoutCache cache;
    cache.Resize(2);
    cache.SetTop(0, 100.0f);
    cache.SetTop(1, 150.0f);

    cache.InvalidateAllLayouts(std::pmr::vector<Node>(2));

    // 位置は次のレイアウトで組み直すまで保持されること
    EXPECT_FLOAT_EQ(cache.Top(0), 100.0f);
    EXPECT_FLOAT_EQ(cache.Top(1), 150.0f);
}

// ズームアウト後に旧倍率の高さが残ると、推定は縮めないので総高さが過大なままになる。
TEST(LayoutCacheTest, InvalidateAllLayoutsResetsTextHeightsButKeepsBitmapNodes)
{
    LayoutCache cache;
    cache.Resize(3);
    std::pmr::vector<Node> nodes(3);
    nodes[1].type = NodeType::Image;
    nodes[2].type = NodeType::CodeBlock;
    nodes[2].set_code_language(SyntaxLanguage::Mermaid);
    for (size_t i = 0; i < 3; i++) {
        cache[i].height = 500.0f;
    }

    cache.InvalidateAllLayouts(nodes);

    EXPECT_FLOAT_EQ(cache[0].height, 0.0f);
    EXPECT_FLOAT_EQ(cache[1].height, 500.0f);
    EXPECT_FLOAT_EQ(cache[2].height, 500.0f);
}

TEST(LayoutComputerTest, EstimateAfterInvalidateAllLayoutsCanShrink)
{
    const Theme theme = MakeLayoutTestTheme();
    LayoutCache cache;
    cache.Resize(1);
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode("short"));
    cache[0].height = 5000.0f;

    cache.InvalidateAllLayouts(nodes);
    layout::EstimateInvisibleNodeHeight(nodes[0], cache[0], theme, 600.0f);

    EXPECT_FLOAT_EQ(cache[0].height, layout::EstimateNodeHeight(nodes[0], theme));
}

TEST(LayoutCacheTest, InvalidateEmptyCache)
{
    LayoutCache cache;
    cache.Resize(0);

    // クラッシュしないこと
    cache.InvalidateAllLayouts({});
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

    cache.InvalidateAllLayouts(std::pmr::vector<Node>(2));

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

// ノード数が減ったら範囲外のダイアグラムを捨てる。残すと別文書の同じ番号のノードに旧ビットマップが付く。
TEST(LayoutCacheTest, ResizeDropsDiagramsBeyondNodeCount)
{
    LayoutCache cache;
    cache.Resize(4);
    cache.EnsureDiagram(1).width = 10.0f;
    cache.EnsureDiagram(3).width = 30.0f;

    cache.Resize(2);

    ASSERT_NE(cache.FindDiagram(1), nullptr);
    EXPECT_FLOAT_EQ(cache.FindDiagram(1)->width, 10.0f);
    EXPECT_EQ(cache.FindDiagram(3), nullptr);
}

// テーマ変更等の全ダイアグラム無効化でエラー状態もクリアされ、
// 再試行の契機になることを検証する (issue #271)。
TEST(LayoutCacheTest, InvalidateAllDiagramBitmapsClearsError)
{
    LayoutCache cache;
    cache.Resize(2);

    cache.EnsureDiagram(0).error = L"Parse error on line 1";
    cache.EnsureDiagram(1).error = L"Parse error on line 3";

    cache.InvalidateAllDiagramBitmaps(std::pmr::vector<Node>(2));

    EXPECT_TRUE(cache.FindDiagram(0)->error.empty());
    EXPECT_TRUE(cache.FindDiagram(1)->error.empty());
}

// デバイスロストでビットマップを捨てても、画像の寸法は再計測が表示高さに使うので残す。
// 図の寸法は従来どおり再描画待ちとして捨てる。
TEST(LayoutCacheTest, InvalidateAllDiagramBitmapsKeepsImageSize)
{
    std::pmr::vector<Node> nodes(2);
    nodes[0].type = NodeType::Image;
    nodes[1].type = NodeType::CodeBlock;
    nodes[1].set_code_language(SyntaxLanguage::Mermaid);

    LayoutCache cache;
    cache.Resize(2);
    for (size_t i = 0; i < 2; i++) {
        auto& d = cache.EnsureDiagram(i);
        d.width = 400.0f;
        d.height = 300.0f;
    }

    cache.InvalidateAllDiagramBitmaps(nodes);

    EXPECT_FLOAT_EQ(cache.FindDiagram(0)->width, 400.0f);
    EXPECT_FLOAT_EQ(cache.FindDiagram(0)->height, 300.0f);
    EXPECT_FLOAT_EQ(cache.FindDiagram(1)->width, 0.0f);
    EXPECT_FLOAT_EQ(cache.FindDiagram(1)->height, 0.0f);
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

    layout::EstimateNodeHeights(nodes, cache, theme);
    std::vector<float> expected;
    for (size_t i = 0; i < cache.size(); ++i) {
        cache[i].layout_dirty = false;
        expected.push_back(cache.Top(i));
    }

    // safe_exit_after=2 で、index >= 3 のノードについて text_top と y が一致なら早期終了。
    // EstimateNodeHeights 直後なので一致するはず → 早期終了経路を通る。
    layout::RecomputeYPositions(nodes, cache, theme, 0, 2);

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
    const auto [first, last_plus_1] = ComputeVisibleNodeRange(cache, 150.0f, 350.0f);
    EXPECT_EQ(first, 1u);
    EXPECT_EQ(last_plus_1, 4u);
}

TEST(ComputeVisibleNodeRangeTest, RangeBelowContentIsEmpty)
{
    const auto cache = MakeUniformCache(3);
    const auto [first, last_plus_1] = ComputeVisibleNodeRange(cache, 500.0f, 800.0f);
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

// トークンは text_layout と一緒に作るので、layout を捨てたら一緒に解放される。
TEST(LayoutCacheTest, SyntaxTokensReleasedWithTextLayout)
{
    NodeLayoutEntry entry;
    entry.set_syntax_tokens({ SyntaxToken{ 0, 3, SyntaxTokenType::Keyword } });
    ASSERT_EQ(entry.syntax_tokens().size(), 1u);

    entry.reset_text_layout();
    EXPECT_TRUE(entry.syntax_tokens().empty());

    entry.set_syntax_tokens({ SyntaxToken{ 0, 3, SyntaxTokenType::Keyword } });
    entry.release_layout_state();
    EXPECT_TRUE(entry.syntax_tokens().empty());
}
