#include <gtest/gtest.h>
#include "layout_cache.h"

TEST(LayoutCacheTest, InvalidateAllLayouts) {
    LayoutCache cache;
    cache.Resize(3);

    // レイアウトが設定された状態をシミュレート
    cache[0].effects_applied = true;
    cache[1].effects_applied = true;
    cache[2].effects_applied = true;
    cache[0].inline_code_bgs.emplace_back(0.0f, 0.0f, 10.0f, 10.0f);
    cache[1].inline_code_bgs.emplace_back(0.0f, 0.0f, 20.0f, 20.0f);

    cache.InvalidateAllLayouts();

    for (size_t i = 0; i < 3; ++i) {
        EXPECT_FALSE(cache[i].effects_applied) << "index " << i;
        EXPECT_TRUE(cache[i].inline_code_bgs.empty()) << "index " << i;
        // text_layoutはComPtrで、Reset()するとnullになる
        EXPECT_EQ(cache[i].text_layout.Get(), nullptr) << "index " << i;
    }
}

TEST(LayoutCacheTest, InvalidateAllLayoutsPreservesPositions) {
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

TEST(LayoutCacheTest, MarkAllDirty) {
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

TEST(LayoutCacheTest, InvalidateEmptyCache) {
    LayoutCache cache;
    cache.Resize(0);

    // クラッシュしないこと
    cache.InvalidateAllLayouts();
    cache.MarkAllDirty();
}

// ズーム時に使用されるInvalidateAllLayoutsがダイアグラムのビットマップ/サイズを
// 保持することを検証する（ズーム→復帰でMermaid図が消える問題の再発防止）。
TEST(LayoutCacheTest, InvalidateAllLayoutsPreservesDiagramEntries) {
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
