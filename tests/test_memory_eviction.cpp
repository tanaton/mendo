#include <gtest/gtest.h>
#include <memory_resource>
#include "image_loader.h"
#include "layout_cache.h"
#include "layout.h"
#include "parser.h"
#include "mock_text_measurer.h"

// ============================================================
// ImageLoader LRU キャッシュテスト
// ============================================================

class ImageLoaderLruTest : public ::testing::Test {
protected:
    ImageLoader loader_;

    void SetUp() override
    {
        // キャッシュの LRU 動作テストでは GetCachedImage / RemoveCached のみ使用。
        // レンダーターゲットや WIC は不要。
        loader_.Init(nullptr);
    }
};

TEST_F(ImageLoaderLruTest, GetCachedImageReturnsFalseForUnknownPath)
{
    DiagramEntry out;
    EXPECT_FALSE(loader_.GetCachedImage(L"nonexistent.png", out));
}

TEST_F(ImageLoaderLruTest, ClearCacheRemovesAllEntries)
{
    // キャッシュをクリアしてもクラッシュしないこと
    loader_.ClearCache();
    DiagramEntry out;
    EXPECT_FALSE(loader_.GetCachedImage(L"any.png", out));
}

TEST_F(ImageLoaderLruTest, RemoveCachedDoesNotCrashOnMissingKey)
{
    // 存在しないキーの削除でクラッシュしないこと
    loader_.RemoveCached(L"nonexistent.png");
}

// ============================================================
// LayoutCache ビューポート外 eviction テスト
// ============================================================

TEST(LayoutCacheEviction, EvictedEntriesPreservePositionAndHeight)
{
    // eviction 後も y_position と height が保持されることを確認
    LayoutCache cache;
    cache.Resize(5);

    for (size_t i = 0; i < 5; i++) {
        cache[i].y_position = static_cast<float>(i * 200);
        cache[i].height = 100.0f;
        cache[i].layout_dirty = false;
        cache[i].effects_applied = true;
    }

    // ノード 0, 1 を eviction 対象としてシミュレート
    cache[0].text_layout.Reset();
    cache[0].layout_dirty = true;
    cache[0].effects_applied = false;
    cache[1].text_layout.Reset();
    cache[1].layout_dirty = true;
    cache[1].effects_applied = false;

    // y_position と height は保持される
    EXPECT_FLOAT_EQ(cache[0].y_position, 0.0f);
    EXPECT_FLOAT_EQ(cache[0].height, 100.0f);
    EXPECT_FLOAT_EQ(cache[1].y_position, 200.0f);
    EXPECT_FLOAT_EQ(cache[1].height, 100.0f);

    // layout_dirty が設定される
    EXPECT_TRUE(cache[0].layout_dirty);
    EXPECT_TRUE(cache[1].layout_dirty);
    EXPECT_FALSE(cache[0].effects_applied);

    // 非 eviction ノードは影響を受けない
    EXPECT_FALSE(cache[2].layout_dirty);
    EXPECT_TRUE(cache[2].effects_applied);
}

TEST(LayoutCacheEviction, DiagramEvictionPreservesWidthHeight)
{
    // DiagramEntry のビットマップを解放しても width/height が保持されることを確認
    LayoutCache cache;
    cache.Resize(3);

    cache.GetDiagram(0).width = 400.0f;
    cache.GetDiagram(0).height = 300.0f;
    cache.GetDiagram(1).width = 800.0f;
    cache.GetDiagram(1).height = 600.0f;

    // ビットマップのみ解放
    cache.GetDiagram(0).bitmap.Reset();
    cache.GetDiagram(1).bitmap.Reset();

    EXPECT_FLOAT_EQ(cache.GetDiagram(0).width, 400.0f);
    EXPECT_FLOAT_EQ(cache.GetDiagram(0).height, 300.0f);
    EXPECT_FLOAT_EQ(cache.GetDiagram(1).width, 800.0f);
    EXPECT_FLOAT_EQ(cache.GetDiagram(1).height, 600.0f);
    EXPECT_EQ(cache.GetDiagram(0).bitmap.Get(), nullptr);
}

// ============================================================
// ProcessDirtyBatch ビューポート制限テスト
// ============================================================

class ProcessDirtyBatchViewportTest : public ::testing::Test {
protected:
    MockTextMeasurer mock_;
    LayoutEngine engine_;
    Theme theme_;

    void SetUp() override
    {
        theme_ = GetLightTheme();
        ASSERT_TRUE(engine_.Init(&mock_, theme_));
    }
};

TEST_F(ProcessDirtyBatchViewportTest, SkipsFarOffscreenNodes)
{
    // 100 ノードのドキュメントを作成
    std::string md;
    for (int i = 0; i < 100; i++) {
        md += "paragraph " + std::to_string(i) + "\n\n";
    }
    auto nodes = ParseMarkdown(md);
    ASSERT_GT(nodes.size(), 50u);

    LayoutCache cache;
    cache.Resize(nodes.size());

    // 初回レイアウトで Y 位置を確定
    engine_.ComputeLayout(nodes, cache, 800.0f);

    // 全ノードの layout_dirty を true に設定（eviction をシミュレート）
    for (size_t i = 0; i < nodes.size(); i++) {
        cache[i].layout_dirty = true;
        cache[i].text_layout.Reset();
    }

    // ビューポートを先頭に設定（top=0, height=200）
    // バッファ: ±5画面 = ±1000px → [−1000, 1200] が処理範囲
    const float viewport_top = 0.0f;
    const float viewport_height = 200.0f;

    // ビューポート制限付きで ProcessDirtyBatch を実行
    engine_.ProcessDirtyBatch(nodes, cache, 800.0f, 10000, 0,
        viewport_top, viewport_height);

    // ビューポート付近のノードはダーティでなくなっているはず
    EXPECT_FALSE(cache[0].layout_dirty) << "ビューポート内のノードは処理されるべき";

    // ビューポートから遠いノード（y > 1200）はダーティのまま
    bool found_far_dirty = false;
    for (size_t i = 0; i < nodes.size(); i++) {
        if (cache[i].y_position > 1200.0f && cache[i].layout_dirty) {
            found_far_dirty = true;
            break;
        }
    }
    EXPECT_TRUE(found_far_dirty) << "ビューポート遠方のノードは未処理のままであるべき";
}

TEST_F(ProcessDirtyBatchViewportTest, WithoutViewportLimitProcessesAll)
{
    std::string md;
    for (int i = 0; i < 50; i++) {
        md += "paragraph " + std::to_string(i) + "\n\n";
    }
    auto nodes = ParseMarkdown(md);

    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    for (size_t i = 0; i < nodes.size(); i++) {
        cache[i].layout_dirty = true;
        cache[i].text_layout.Reset();
    }

    // ビューポート制限なし（デフォルト: viewport_top=-1, viewport_height=-1）
    engine_.ProcessDirtyBatch(nodes, cache, 800.0f, 10000, 0);

    // 全ノードが処理されるべき
    for (size_t i = 0; i < nodes.size(); i++) {
        EXPECT_FALSE(cache[i].layout_dirty) << "ノード " << i << " は処理されるべき";
    }
}

TEST_F(ProcessDirtyBatchViewportTest, HasDirtyNodesFalseAfterNearbyProcessed)
{
    std::string md;
    for (int i = 0; i < 100; i++) {
        md += "paragraph " + std::to_string(i) + "\n\n";
    }
    auto nodes = ParseMarkdown(md);

    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    for (size_t i = 0; i < nodes.size(); i++) {
        cache[i].layout_dirty = true;
        cache[i].text_layout.Reset();
    }

    // ビューポート制限付きで全バッチ処理
    bool more = true;
    int iterations = 0;
    while (more && iterations < 100) {
        more = engine_.ProcessDirtyBatch(nodes, cache, 800.0f, 10000, 0, 0.0f, 200.0f);
        iterations++;
    }

    // 付近のダーティノードが処理され、has_dirty_nodes が false になるべき
    EXPECT_FALSE(engine_.HasDirtyNodes())
        << "ビューポート付近の全ダーティノードが処理されたら false であるべき";
}

// ============================================================
// FindFirstVisibleNodeIndex テスト（eviction の基盤）
// ============================================================

TEST(FindFirstVisibleNodeIndex, ReturnsZeroForEmptyCache)
{
    LayoutCache cache;
    cache.Resize(0);
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 0, 0.0f), 0);
}

TEST(FindFirstVisibleNodeIndex, FindsCorrectNodeInMiddle)
{
    LayoutCache cache;
    cache.Resize(10);
    for (size_t i = 0; i < 10; i++) {
        cache[i].y_position = static_cast<float>(i * 100);
        cache[i].height = 80.0f;
    }

    // viewport_top=450: ノード 4 (y=400, h=80) の下端=480 > 450
    int idx = FindFirstVisibleNodeIndex(cache, 10, 450.0f);
    EXPECT_EQ(idx, 4) << "viewport_top=450 の最初の可視ノードはインデックス 4";
}

TEST(FindFirstVisibleNodeIndex, ReturnsNodeCountWhenAllAbove)
{
    LayoutCache cache;
    cache.Resize(3);
    for (size_t i = 0; i < 3; i++) {
        cache[i].y_position = static_cast<float>(i * 100);
        cache[i].height = 80.0f;
    }

    // 全ノードがビューポートの上にある場合
    int idx = FindFirstVisibleNodeIndex(cache, 3, 500.0f);
    EXPECT_EQ(idx, 3);
}
