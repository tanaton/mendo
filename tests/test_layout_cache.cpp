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

// テーマ変更等の全ダイアグラム無効化でエラー状態もクリアされ、
// 再試行の契機になることを検証する (issue #271)。
TEST(LayoutCacheTest, InvalidateAllDiagramBitmapsClearsError)
{
    LayoutCache cache;
    cache.Resize(2);

    cache.GetDiagram(0).error = L"Parse error on line 1";
    cache.GetDiagram(1).error = L"Parse error on line 3";

    cache.InvalidateAllDiagramBitmaps();

    EXPECT_TRUE(cache.GetDiagram(0).error.empty());
    EXPECT_TRUE(cache.GetDiagram(1).error.empty());
}

// 早期終了経路 (safe_exit_after で abs(text_top - y) < EPSILON ならスキップ) を通った後でも、
// 残りノードの text_top がフル計算と一致することを検証する。
TEST(LayoutCacheTest, RecomputeYPositionsEarlyExitKeepsTextTop)
{
    LayoutCache cache;
    cache.Resize(8);

    Theme theme{};
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
    std::vector<float> expected;
    for (size_t i = 0; i < cache.size(); ++i) {
        cache[i].layout_dirty = false;
        expected.push_back(cache.Top(i));
    }

    // safe_exit_after=2 で、index >= 3 のノードについて text_top と y が一致なら早期終了。
    // EstimateNodeHeights 直後なので一致するはず → 早期終了経路を通る。
    mendo::layout::RecomputeYPositions(nodes, cache, theme, 0, false, 2);

    for (size_t i = 0; i < cache.size(); ++i) {
        EXPECT_FLOAT_EQ(cache.Top(i), expected[i]) << "after early-exit, index " << i;
    }
}

