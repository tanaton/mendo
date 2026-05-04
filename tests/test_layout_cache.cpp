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
    cache[0].text_top = 100.0f;
    cache[0].height = 50.0f;
    cache[1].text_top = 150.0f;
    cache[1].height = 30.0f;

    cache.InvalidateAllLayouts();

    // 位置は保持されること
    EXPECT_FLOAT_EQ(cache[0].text_top, 100.0f);
    EXPECT_FLOAT_EQ(cache[0].height, 50.0f);
    EXPECT_FLOAT_EQ(cache[1].text_top, 150.0f);
    EXPECT_FLOAT_EQ(cache[1].height, 30.0f);
}

TEST(LayoutCacheTest, MarkAllDirty)
{
    LayoutCache cache;
    cache.Resize(3);

    cache[0].layout_dirty = false;
    cache[1].layout_dirty = false;
    cache[2].layout_dirty = false;

    cache.MarkAllDirty();

    for (size_t i = 0; i < 3; ++i) {
        EXPECT_TRUE(cache[i].layout_dirty) << "index " << i;
        EXPECT_EQ(cache[i].text_layout.Get(), nullptr) << "index " << i;
    }
}

TEST(LayoutCacheTest, InvalidateEmptyCache)
{
    LayoutCache cache;
    cache.Resize(0);

    // クラッシュしないこと
    cache.InvalidateAllLayouts();
    cache.MarkAllDirty();
}

// ズーム時に使用されるInvalidateAllLayoutsがダイアグラムのビットマップ/サイズを
// 保持することを検証する（ズーム→復帰でMermaid図が消える問題の再発防止）。
TEST(LayoutCacheTest, InvalidateAllLayoutsPreservesDiagramEntries)
{
    LayoutCache cache;
    cache.Resize(2);

    // ダイアグラムエントリにサイズを設定（ビットマップは設定できないが、サイズで確認）
    cache.GetDiagram(0).width = 400.0f;
    cache.GetDiagram(0).height = 300.0f;
    cache.GetDiagram(1).width = 500.0f;
    cache.GetDiagram(1).height = 250.0f;

    cache.InvalidateAllLayouts();

    // ダイアグラムの幅・高さは保持されること
    EXPECT_FLOAT_EQ(cache.GetDiagram(0).width, 400.0f);
    EXPECT_FLOAT_EQ(cache.GetDiagram(0).height, 300.0f);
    EXPECT_FLOAT_EQ(cache.GetDiagram(1).width, 500.0f);
    EXPECT_FLOAT_EQ(cache.GetDiagram(1).height, 250.0f);
}

// SetBlockHeight 経由で Fenwick が更新され、GetBlockTop /
// GetTotalHeightFromFenwick が margin_top を考慮した正しい値を返すことを検証する。
TEST(LayoutCacheTest, BlockHeightFenwickRoundTrip)
{
    LayoutCache cache;
    cache.Resize(4);

    constexpr float kMarginTop = 12.0f;
    cache.SetBlockHeight(0, 30.0f);
    cache.SetBlockHeight(1, 40.0f);
    cache.SetBlockHeight(2, 25.0f);
    cache.SetBlockHeight(3, 50.0f);

    EXPECT_FLOAT_EQ(cache.GetBlockTop(0, kMarginTop), 12.0f);
    EXPECT_FLOAT_EQ(cache.GetBlockTop(1, kMarginTop), 12.0f + 30.0f);
    EXPECT_FLOAT_EQ(cache.GetBlockTop(2, kMarginTop), 12.0f + 70.0f);
    EXPECT_FLOAT_EQ(cache.GetBlockTop(3, kMarginTop), 12.0f + 95.0f);
    // 総高さ = 2 * margin_top + sum(block heights)
    EXPECT_FLOAT_EQ(cache.GetTotalHeightFromFenwick(kMarginTop),
                    2.0f * kMarginTop + 145.0f);
}

// Resize は Fenwick の値もゼロクリアする（古い文書の高さ情報が残らない）。
TEST(LayoutCacheTest, ResizeResetsFenwick)
{
    LayoutCache cache;
    cache.Resize(3);
    cache.SetBlockHeight(0, 10.0f);
    cache.SetBlockHeight(1, 20.0f);
    cache.SetBlockHeight(2, 30.0f);
    EXPECT_FLOAT_EQ(cache.GetTotalHeightFromFenwick(0.0f), 60.0f);

    // 違うサイズへ Resize → Fenwick は全てゼロクリア
    cache.Resize(5);
    EXPECT_FLOAT_EQ(cache.GetTotalHeightFromFenwick(0.0f), 0.0f);
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_FLOAT_EQ(cache.GetBlockTop(i, 0.0f), 0.0f) << "index " << i;
    }
}

// ResizePreservingPrefix で末尾を伸ばしたとき、prefix 部分の Fenwick 値は保持され、
// 新規末尾は 0 で初期化される。
TEST(LayoutCacheTest, ResizePreservingPrefixKeepsFenwickPrefix)
{
    LayoutCache cache;
    cache.Resize(3);
    cache.SetBlockHeight(0, 10.0f);
    cache.SetBlockHeight(1, 20.0f);
    cache.SetBlockHeight(2, 30.0f);

    cache.ResizePreservingPrefix(5);
    EXPECT_FLOAT_EQ(cache.GetTotalHeightFromFenwick(0.0f), 60.0f);
    EXPECT_FLOAT_EQ(cache.GetBlockTop(2, 0.0f), 30.0f);
    EXPECT_FLOAT_EQ(cache.GetBlockTop(3, 0.0f), 60.0f); // prefix sum until index 3 = 10+20+30
    EXPECT_FLOAT_EQ(cache.GetBlockTop(4, 0.0f), 60.0f); // 4 番目も 0 値追加なので同じ

    // 新規末尾に値を入れても prefix は壊れない
    cache.SetBlockHeight(3, 40.0f);
    cache.SetBlockHeight(4, 50.0f);
    EXPECT_FLOAT_EQ(cache.GetTotalHeightFromFenwick(0.0f), 150.0f);
    EXPECT_FLOAT_EQ(cache.GetBlockTop(2, 0.0f), 30.0f);
}

// GetBlockHeight が個別ノードの block_height を取り出す。
TEST(LayoutCacheTest, GetBlockHeight)
{
    LayoutCache cache;
    cache.Resize(3);
    cache.SetBlockHeight(0, 10.0f);
    cache.SetBlockHeight(1, 20.5f);
    cache.SetBlockHeight(2, 30.25f);
    EXPECT_FLOAT_EQ(cache.GetBlockHeight(0), 10.0f);
    EXPECT_FLOAT_EQ(cache.GetBlockHeight(1), 20.5f);
    EXPECT_FLOAT_EQ(cache.GetBlockHeight(2), 30.25f);
}

// Phase C invariant: WRITE 経路 (EstimateNodeHeights / RecomputeYPositions) で entry.text_top が
// Fenwick 派生値 (cache.GetBlockTop(i, margin_top) + GetSpacingAbove(nodes[i], theme)) と
// bit-exact 一致することを検証する。両者は同じ Y 位置を表す 2 つの経路で、Fenwick が SSOT。
TEST(LayoutCacheTest, TextTopMatchesFenwickDerivedValue)
{
    LayoutCache cache;
    cache.Resize(5);

    Theme theme;
    theme.margin_top = 10.0f;
    theme.heading_spacing_above = 8.0f;
    theme.heading_spacing_below = 4.0f;
    theme.heading_spacing_below_h1h2 = 6.0f;
    theme.code_block_spacing_above = 12.0f;
    theme.paragraph_spacing = 5.0f;
    theme.font_size_body = 14.0f;
    theme.font_size_code = 12.0f;
    for (int i = 0; i < 6; ++i) {
        theme.font_size_h[i] = 18.0f - static_cast<float>(i);
    }
    theme.list_item_spacing = 3.0f;
    theme.code_block_padding = 4.0f;
    theme.indent_width = 16.0f;
    theme.hr_thickness = 1.0f;

    std::pmr::vector<Node> nodes;
    nodes.resize(5);
    nodes[0].type = NodeType::Heading;
    nodes[0].heading_level = 1;
    nodes[1].type = NodeType::Paragraph;
    nodes[2].type = NodeType::CodeBlock;
    nodes[3].type = NodeType::ListItem;
    nodes[4].type = NodeType::HorizontalRule;

    mendo::layout::EstimateNodeHeights(nodes, cache, theme);
    for (size_t i = 0; i < cache.size(); ++i) {
        const float fenwick = mendo::layout::TextTopOf(cache, i, nodes[i], theme);
        EXPECT_FLOAT_EQ(cache[i].text_top, fenwick) << "after EstimateNodeHeights, index " << i;
    }

    cache[0].layout_dirty = true;
    auto result = mendo::layout::RecomputeYPositions(nodes, cache, theme);
    (void)result;
    for (size_t i = 0; i < cache.size(); ++i) {
        const float fenwick = mendo::layout::TextTopOf(cache, i, nodes[i], theme);
        EXPECT_FLOAT_EQ(cache[i].text_top, fenwick) << "after RecomputeYPositions, index " << i;
    }
}

// 早期終了経路 (safe_exit_after で abs(text_top - y) < EPSILON ならスキップ) を通った後でも、
// 残りノードの text_top が Fenwick 派生値と整合することを検証する。
// Phase B-2 で実機描画崩れの原因になった「text_top と Fenwick block_height の同期スキップ」が
// 再発しないことを保証する R2 リスク対策。
TEST(LayoutCacheTest, RecomputeYPositionsEarlyExitKeepsFenwickAndTextTopInSync)
{
    LayoutCache cache;
    cache.Resize(8);

    Theme theme;
    theme.margin_top = 5.0f;
    theme.paragraph_spacing = 4.0f;
    theme.font_size_body = 14.0f;
    theme.font_size_code = 12.0f;
    for (int i = 0; i < 6; ++i) {
        theme.font_size_h[i] = 18.0f - static_cast<float>(i);
    }
    theme.list_item_spacing = 3.0f;

    std::pmr::vector<Node> nodes;
    nodes.resize(8);
    for (auto& n : nodes) {
        n.type = NodeType::Paragraph;
    }

    mendo::layout::EstimateNodeHeights(nodes, cache, theme);
    for (size_t i = 0; i < cache.size(); ++i) {
        cache[i].layout_dirty = false;
    }

    // safe_exit_after=2 で、index >= 3 のノードについて text_top と y が一致なら早期終了。
    // EstimateNodeHeights 直後なので一致するはず → 早期終了経路を通る。
    auto result = mendo::layout::RecomputeYPositions(nodes, cache, theme, 0, false, 2);
    (void)result;

    for (size_t i = 0; i < cache.size(); ++i) {
        const float fenwick = mendo::layout::TextTopOf(cache, i, nodes[i], theme);
        EXPECT_FLOAT_EQ(cache[i].text_top, fenwick) << "after early-exit, index " << i;
    }
}

// 新オーバーロード ::mendo::layout::FindFirstVisibleNodeIndex(cache, nodes, theme, count, viewport_top) が
// レガシー版 (block_bottom 述語) と spacing 0 のケースで bit-exact 一致することを検証する。
// spacing がない単純な配置では block_bottom = text_top + height で意味論差が発生しないため、
// すべての viewport_top で同じインデックスが返るはず。
// （spacing > 0 のケースでは ±1 ノード差分が出る可能性があるため別テスト案件）
TEST(LayoutCacheTest, FindFirstVisibleNodeIndexNewOverloadMatchesLegacy)
{
    LayoutCache cache;
    cache.Resize(4);

    Theme theme;
    theme.margin_top = 0.0f;
    theme.heading_spacing_above = 0.0f;
    theme.code_block_spacing_above = 0.0f;
    theme.heading_spacing_below = 0.0f;
    theme.heading_spacing_below_h1h2 = 0.0f;
    theme.paragraph_spacing = 0.0f;
    theme.code_block_padding = 0.0f;
    theme.list_item_spacing = 0.0f;

    std::pmr::vector<Node> nodes;
    nodes.resize(4);
    for (auto& n : nodes) {
        n.type = NodeType::HorizontalRule; // sa = sb = 0
    }

    cache[0].height = 10.0f;
    cache[1].height = 20.0f;
    cache[2].height = 30.0f;
    cache[3].height = 40.0f;
    cache[0].text_top = 0.0f;
    cache[1].text_top = 10.0f;
    cache[2].text_top = 30.0f;
    cache[3].text_top = 60.0f;
    cache.SetBlockHeight(0, 10.0f);
    cache.SetBlockHeight(1, 20.0f);
    cache.SetBlockHeight(2, 30.0f);
    cache.SetBlockHeight(3, 40.0f);

    constexpr float kEps = 0.001f;
    for (float vt : {-5.0f, 0.0f, 5.0f, 10.0f - kEps, 10.0f, 25.0f, 30.0f, 60.0f, 99.0f, 200.0f}) {
        const int legacy = ::FindFirstVisibleNodeIndex(cache, 4, vt);
        const int fenwick = mendo::layout::FindFirstVisibleNodeIndex(cache, nodes, theme, 4, vt);
        EXPECT_EQ(legacy, fenwick) << "viewport_top = " << vt;
    }
}

// FindBlockTopLowerBound は「累積 target を初めて超えるインデックス」を返す。
// PrefixSum: [0, 10, 30, 60] (block heights = [10, 20, 30])。
TEST(LayoutCacheTest, FindBlockTopLowerBound)
{
    LayoutCache cache;
    cache.Resize(3);
    cache.SetBlockHeight(0, 10.0f);
    cache.SetBlockHeight(1, 20.0f);
    cache.SetBlockHeight(2, 30.0f);

    EXPECT_EQ(cache.FindBlockTopLowerBound(0.0f), 0u);
    EXPECT_EQ(cache.FindBlockTopLowerBound(9.99f), 0u);
    EXPECT_EQ(cache.FindBlockTopLowerBound(10.0f), 1u);
    EXPECT_EQ(cache.FindBlockTopLowerBound(29.99f), 1u);
    EXPECT_EQ(cache.FindBlockTopLowerBound(30.0f), 2u);
    EXPECT_EQ(cache.FindBlockTopLowerBound(59.99f), 2u);
    EXPECT_EQ(cache.FindBlockTopLowerBound(60.0f), 3u); // 該当なし
    EXPECT_EQ(cache.FindBlockTopLowerBound(100.0f), 3u);
}
