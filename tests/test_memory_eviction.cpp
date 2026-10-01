#include <gtest/gtest.h>
#include "document_test_helpers.h"
#include "image_loader.h"
#include "mock_text_measurer.h"

// ============================================================
// ImageLoader キャッシュテスト
// ============================================================

class ImageLoaderCacheTest : public ::testing::Test {
protected:
    ImageLoader loader_;

    void SetUp() override
    {
        // キャッシュ動作テストでは GetCachedImage / InsertCacheEntry のみ使用。
        // レンダーターゲットや WIC は不要。
        loader_.Init(nullptr);
    }
};

TEST_F(ImageLoaderCacheTest, GetCachedImageReturnsFalseForUnknownPath)
{
    DiagramEntry out;
    EXPECT_FALSE(loader_.GetCachedImage(L"nonexistent.png", out));
}

TEST_F(ImageLoaderCacheTest, ClearCacheRemovesAllEntries)
{
    // キャッシュをクリアしてもクラッシュしないこと
    loader_.ClearCache();
    DiagramEntry out;
    EXPECT_FALSE(loader_.GetCachedImage(L"any.png", out));
}

TEST_F(ImageLoaderCacheTest, EvictsLeastRecentlyInsertedWhenExceedingMaxEntries)
{
    // 新規 Insert は先頭に入り、容量超過時は末尾 (= 最も古く Insert された) が捨てられる。
    const size_t max_entries = 128; // MAX_CACHE_ENTRIES

    for (size_t i = 0; i < max_entries; i++) {
        loader_.InsertCacheEntry(L"img_" + std::to_wstring(i) + L".png", 100.0f, 100.0f);
    }
    EXPECT_EQ(loader_.CacheSize(), max_entries);

    // 1 つ追加すると最も古く Insert された img_0 が捨てられる
    loader_.InsertCacheEntry(L"overflow.png", 100.0f, 100.0f);
    EXPECT_EQ(loader_.CacheSize(), max_entries);

    DiagramEntry out;
    EXPECT_FALSE(loader_.GetCachedImage(L"img_0.png", out));
    EXPECT_TRUE(loader_.GetCachedImage(L"img_127.png", out));
    EXPECT_TRUE(loader_.GetCachedImage(L"overflow.png", out));
}

TEST_F(ImageLoaderCacheTest, LatestInsertsWinAcrossOverflows)
{
    // 連続 overflow が起きると古い Insert から順に末尾から捨てられていく。
    const size_t max_entries = 128;

    for (size_t i = 0; i < max_entries; i++) {
        loader_.InsertCacheEntry(L"img_" + std::to_wstring(i) + L".png", 100.0f, 100.0f);
    }

    // 10 回 overflow を起こす → img_0..img_9 が末尾から順に捨てられる
    for (int i = 0; i < 10; i++) {
        loader_.InsertCacheEntry(L"overflow_" + std::to_wstring(i) + L".png", 100.0f, 100.0f);
    }
    EXPECT_EQ(loader_.CacheSize(), max_entries);

    DiagramEntry out;
    EXPECT_FALSE(loader_.GetCachedImage(L"img_0.png", out));
    EXPECT_FALSE(loader_.GetCachedImage(L"img_9.png", out));
    EXPECT_TRUE(loader_.GetCachedImage(L"img_10.png", out));
    EXPECT_TRUE(loader_.GetCachedImage(L"img_127.png", out));
    EXPECT_TRUE(loader_.GetCachedImage(L"overflow_9.png", out));
}

// ============================================================
// ProcessDirtyBatch ビューポート制限テスト
// ============================================================

class ProcessDirtyBatchViewportTest : public MockLayoutTestBase {};

TEST_F(ProcessDirtyBatchViewportTest, SkipsFarOffscreenNodes)
{
    // 初回レイアウトで Y 位置を確定
    auto [nodes, cache] = ParseAndLayout(MakeParagraphs(100));
    ASSERT_GT(nodes.size(), 50u);

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
        if (cache.Top(i) > 1200.0f && cache[i].layout_dirty) {
            found_far_dirty = true;
            break;
        }
    }
    EXPECT_TRUE(found_far_dirty) << "ビューポート遠方のノードは未処理のままであるべき";
}

TEST_F(ProcessDirtyBatchViewportTest, WithoutViewportLimitProcessesAll)
{
    auto [nodes, cache] = ParseAndLayout(MakeParagraphs(50));

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
    auto [nodes, cache] = ParseAndLayout(MakeParagraphs(100));

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
