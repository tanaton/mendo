#include <gtest/gtest.h>
#include "layout_cache.h"

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
    cache[0].y_position = 100.0f;
    cache[0].height = 50.0f;
    cache[1].y_position = 150.0f;
    cache[1].height = 30.0f;

    cache.InvalidateAllLayouts();

    // 位置は保持されること
    EXPECT_FLOAT_EQ(cache[0].y_position, 100.0f);
    EXPECT_FLOAT_EQ(cache[0].height, 50.0f);
    EXPECT_FLOAT_EQ(cache[1].y_position, 150.0f);
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

// SetBlockHeight 経由で Fenwick が更新され、GetBlockTopFromFenwick /
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

    EXPECT_FLOAT_EQ(cache.GetBlockTopFromFenwick(0, kMarginTop), 12.0f);
    EXPECT_FLOAT_EQ(cache.GetBlockTopFromFenwick(1, kMarginTop), 12.0f + 30.0f);
    EXPECT_FLOAT_EQ(cache.GetBlockTopFromFenwick(2, kMarginTop), 12.0f + 70.0f);
    EXPECT_FLOAT_EQ(cache.GetBlockTopFromFenwick(3, kMarginTop), 12.0f + 95.0f);
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
        EXPECT_FLOAT_EQ(cache.GetBlockTopFromFenwick(i, 0.0f), 0.0f) << "index " << i;
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
    EXPECT_FLOAT_EQ(cache.GetBlockTopFromFenwick(2, 0.0f), 30.0f);
    EXPECT_FLOAT_EQ(cache.GetBlockTopFromFenwick(3, 0.0f), 60.0f); // prefix sum until index 3 = 10+20+30
    EXPECT_FLOAT_EQ(cache.GetBlockTopFromFenwick(4, 0.0f), 60.0f); // 4 番目も 0 値追加なので同じ

    // 新規末尾に値を入れても prefix は壊れない
    cache.SetBlockHeight(3, 40.0f);
    cache.SetBlockHeight(4, 50.0f);
    EXPECT_FLOAT_EQ(cache.GetTotalHeightFromFenwick(0.0f), 150.0f);
    EXPECT_FLOAT_EQ(cache.GetBlockTopFromFenwick(2, 0.0f), 30.0f);
}

// GetBlockTop は GetBlockTopFromFenwick のエイリアス。
TEST(LayoutCacheTest, GetBlockTopAlias)
{
    LayoutCache cache;
    cache.Resize(3);
    cache.SetBlockHeight(0, 10.0f);
    cache.SetBlockHeight(1, 20.0f);
    cache.SetBlockHeight(2, 30.0f);

    constexpr float kMargin = 5.0f;
    for (size_t i = 0; i <= 3; ++i) {
        EXPECT_FLOAT_EQ(cache.GetBlockTop(i, kMargin),
                        cache.GetBlockTopFromFenwick(i, kMargin)) << "index " << i;
    }
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
