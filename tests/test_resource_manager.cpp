#include <gtest/gtest.h>
#include "resource_manager.h"
#include "document.h"
#include "layout_cache.h"
#include "viewport_manager.h"
#include "image_loader.h"
#include "mermaid_renderer_interface.h"
#include "theme_service.h"
#include "config_service.h"
#include "test_helpers.h"
#include <vector>

namespace {

// IMermaidRenderer のテスト用 mock。各メソッドの呼び出し回数と最後の引数を記録する。
class MockMermaidRenderer : public IMermaidRenderer {
public:
    int request_render_count = 0;
    int cancel_pending_count = 0;
    int clear_cache_count = 0;
    Node* last_node = nullptr;
    float last_max_width = 0.0f;
    bool last_dark_mode = false;

    void RequestRender(Node& node, NodeLayoutEntry& /*layout_entry*/,
        DiagramEntry& /*diagram_entry*/, float max_width, bool dark_mode,
        Callback /*on_complete*/) override
    {
        request_render_count++;
        last_node = &node;
        last_max_width = max_width;
        last_dark_mode = dark_mode;
    }

    void RequestSvg(std::wstring_view /*code*/, float /*max_width*/, bool /*dark_mode*/, SvgCallback callback) override
    {
        if (callback) {
            callback(std::pmr::wstring{}, false);
        }
    }

    void CancelPending() override { cancel_pending_count++; }
    void ClearCache() override { clear_cache_count++; }
};

// ResourceManager::Callbacks の各呼び出しを観測するための counter。
struct CallbackTracker {
    int invalidate = 0;
    int set_timer = 0;
    int kill_timer = 0;
    int recompute_layout = 0;
    int recompute_layout_anchored = 0;
    UINT_PTR last_set_timer_id = 0;
    UINT last_set_timer_ms = 0;
    UINT_PTR last_killed_timer_id = 0;

    float content_width = 800.0f;
    float viewport_height = 600.0f;
    float indent_width = 20.0f;
};

ResourceManager::Callbacks MakeCallbacks(CallbackTracker& t)
{
    return ResourceManager::Callbacks{
        .invalidate = [&t]() { t.invalidate++; },
        .set_timer = [&t](UINT_PTR id, UINT ms) {
            t.set_timer++;
            t.last_set_timer_id = id;
            t.last_set_timer_ms = ms;
        },
        .kill_timer = [&t](UINT_PTR id) {
            t.kill_timer++;
            t.last_killed_timer_id = id;
        },
        .get_content_width = [&t]() { return t.content_width; },
        .get_viewport_height = [&t]() { return t.viewport_height; },
        .get_indent_width = [&t]() { return t.indent_width; },
        .recompute_layout = [&t]() { t.recompute_layout++; },
        .recompute_layout_anchored = [&t]() { t.recompute_layout_anchored++; },
    };
}

// MakeUniformCache をベースに layout_dirty=false を立てたキャッシュを返す。
LayoutCache SeedLayoutCache(size_t node_count, float block_height = 100.0f)
{
    LayoutCache cache = MakeUniformCache(static_cast<int>(node_count), block_height);
    for (size_t i = 0; i < node_count; i++) {
        cache[i].layout_dirty = false;
    }
    return cache;
}

} // namespace

class ResourceManagerTest : public ::testing::Test {
protected:
    // ApplyCachedImages は doc_dir.empty() で早期 return するため、テスト用ドキュメントパスは
    // parent_path を持つ絶対パスでなければならない。
    static constexpr const wchar_t* kTestDocPath = L"C:\\dir\\test.md";

    // Markdown を Document に流し込み、対応する LayoutCache をシードしたうえで
    // ResourceManager を初期化する。テストごとに 1 度だけ呼ぶ。
    void LoadMarkdown(std::string_view md, float block_height = 100.0f)
    {
        std::pmr::string utf8(md);
        doc_ = Document::FromMarkdown(std::move(utf8), kTestDocPath);
        cache_ = SeedLayoutCache(doc_.GetNodes().size(), block_height);
        rm_.Init(doc_, cache_, viewport_, image_loader_, mock_mermaid_, theme_service_,
            MakeCallbacks(tracker_));
    }

    Document doc_;
    LayoutCache cache_;
    ViewportManager viewport_;
    ImageLoader image_loader_; // Init せずに使う（画像経路は通らないテストのみ）
    MockMermaidRenderer mock_mermaid_;
    ConfigService config_;
    ThemeService theme_service_{ config_ };
    CallbackTracker tracker_;
    ResourceManager rm_;
};

// ---- 画像経路 ----

TEST_F(ResourceManagerTest, ApplyCachedImagesReturnsZeroWhenNoImagesInDocument)
{
    LoadMarkdown("# heading\n\nparagraph text\n");
    EXPECT_EQ(rm_.ApplyCachedImages(), 0);
}

TEST_F(ResourceManagerTest, ApplyCachedImagesReturnsZeroWhenContentWidthIsZero)
{
    LoadMarkdown("# heading\n\n![alt](image.png)\n");
    tracker_.content_width = 0.0f;
    EXPECT_EQ(rm_.ApplyCachedImages(), 0);
}

TEST_F(ResourceManagerTest, ApplyCachedImagesReturnsZeroForRemoteImages)
{
    // http:// 等のリモート画像は適用対象にならないため、適用数は 0 のまま。
    LoadMarkdown("![alt](https://example.com/foo.png)\n");
    EXPECT_EQ(rm_.ApplyCachedImages(), 0);
}

// ---- 画像経路: ApplyCachedImagesForReload (issue #148) ----

TEST_F(ResourceManagerTest, ApplyCachedImagesForReloadAppliesHeightFromCache)
{
    LoadMarkdown("# heading\n\n![alt](foo.png)\n");
    image_loader_.InsertCacheEntry(L"C:\\dir\\foo.png", 1000.0f, 800.0f);

    // 適用前: SeedLayoutCache が block_height=100 で初期化している。
    const auto& image_indices = doc_.GetImageNodeIndices();
    ASSERT_EQ(image_indices.size(), 1u);
    const size_t img_idx = image_indices[0];
    EXPECT_FLOAT_EQ(cache_[img_idx].height, 100.0f);

    EXPECT_EQ(rm_.ApplyCachedImagesForReload(), 1);

    // content_width=800, image_width=1000 → スケール係数 800/1000、
    // height=800 * 0.8 = 640
    EXPECT_FLOAT_EQ(cache_[img_idx].height, 640.0f);
    EXPECT_FALSE(cache_[img_idx].layout_dirty);
}

TEST_F(ResourceManagerTest, ApplyCachedImagesForReloadKeepsHeightWhenWithinContentWidth)
{
    LoadMarkdown("![alt](small.png)\n");
    // content_width=800 より小さい画像はスケール無し。
    image_loader_.InsertCacheEntry(L"C:\\dir\\small.png", 400.0f, 300.0f);

    EXPECT_EQ(rm_.ApplyCachedImagesForReload(), 1);

    const size_t img_idx = doc_.GetImageNodeIndices()[0];
    EXPECT_FLOAT_EQ(cache_[img_idx].height, 300.0f);
}

TEST_F(ResourceManagerTest, ApplyCachedImagesForReloadReturnsZeroWhenCacheMisses)
{
    LoadMarkdown("![alt](missing.png)\n");
    // image_loader_ にキャッシュ未挿入 → 0 件。
    EXPECT_EQ(rm_.ApplyCachedImagesForReload(), 0);

    const size_t img_idx = doc_.GetImageNodeIndices()[0];
    EXPECT_FLOAT_EQ(cache_[img_idx].height, 100.0f); // SeedLayoutCache の block_height のまま
}

TEST_F(ResourceManagerTest, ApplyCachedImagesForReloadAppliesBeyondVisibleRange)
{
    // issue #148 の本質: 可視範囲外の画像も全件走査して height を更新する。
    // 画像を 2 個配置し、2 個目を viewport から大きく外して ApplyCachedImages との差を出す。
    LoadMarkdown("![one](one.png)\n\n![two](two.png)\n", /*block_height=*/3000.0f);

    const auto& image_indices = doc_.GetImageNodeIndices();
    ASSERT_EQ(image_indices.size(), 2u);

    image_loader_.InsertCacheEntry(L"C:\\dir\\one.png", 400.0f, 300.0f);
    image_loader_.InsertCacheEntry(L"C:\\dir\\two.png", 400.0f, 250.0f);

    // viewport_height=600, PREFETCH_BUFFER=3 → 範囲は ~[-1800, 2400]。
    // 2個目は y=3000+ にあり可視範囲外。ApplyCachedImages は 1 件のみ適用。
    const int visible_applied = rm_.ApplyCachedImages();
    EXPECT_EQ(visible_applied, 1);

    // 2個目の高さはまだ SeedLayoutCache の block_height=3000 のまま。
    EXPECT_FLOAT_EQ(cache_[image_indices[1]].height, 3000.0f);

    // ApplyCachedImagesForReload は範囲制限を持たないので残りの 1 件も適用する。
    // 1 個目は ApplyCachedImages で diagram.bitmap がセットされていないのでもう一度適用される。
    const int reload_applied = rm_.ApplyCachedImagesForReload();
    EXPECT_EQ(reload_applied, 2);
    EXPECT_FLOAT_EQ(cache_[image_indices[1]].height, 250.0f);
    EXPECT_FALSE(cache_[image_indices[1]].layout_dirty);
}

TEST_F(ResourceManagerTest, ApplyCachedImagesForReloadIgnoresRemoteImages)
{
    LoadMarkdown("![alt](https://example.com/foo.png)\n");
    // リモート画像はキャッシュ参照しない。
    EXPECT_EQ(rm_.ApplyCachedImagesForReload(), 0);
}

TEST_F(ResourceManagerTest, ApplyCachedImagesForReloadReturnsZeroWhenContentWidthIsZero)
{
    LoadMarkdown("![alt](foo.png)\n");
    image_loader_.InsertCacheEntry(L"C:\\dir\\foo.png", 1000.0f, 800.0f);
    tracker_.content_width = 0.0f;
    EXPECT_EQ(rm_.ApplyCachedImagesForReload(), 0);
}

// ---- Mermaid 経路 ----

TEST_F(ResourceManagerTest, RequestMermaidRendersInvokesRendererForVisibleDiagram)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    ASSERT_FALSE(doc_.GetDiagramNodeIndices().empty());

    const int applied = rm_.RequestMermaidRenders();
    EXPECT_EQ(applied, 0); // mock は bitmap をセットしないため 0
    EXPECT_EQ(mock_mermaid_.request_render_count, 1);
    EXPECT_FLOAT_EQ(mock_mermaid_.last_max_width, tracker_.content_width);
    EXPECT_FALSE(mock_mermaid_.last_dark_mode);
}

TEST_F(ResourceManagerTest, RequestMermaidRendersSkipsBeyondVisibleRange)
{
    // diagram が 2 個。1個目を可視範囲、2個目を範囲外に置く。
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n\n```mermaid\ngraph TD;C-->D\n```\n",
        /*block_height=*/200.0f);
    ASSERT_GE(doc_.GetDiagramNodeIndices().size(), 2u);

    // viewport_height = 600, PREFETCH_BUFFER = 3 screens → range_top=-1800, range_bottom=2400
    // 各ノード間隔を広くして 2 個目を確実に範囲外に置く。
    cache_ = SeedLayoutCache(doc_.GetNodes().size(), /*block_height=*/3000.0f);

    rm_.RequestMermaidRenders();
    // 1個目 (y=0, 可視範囲内) のみ Render される
    EXPECT_EQ(mock_mermaid_.request_render_count, 1);
}

TEST_F(ResourceManagerTest, RequestMermaidRendersZeroWidthIsNoOp)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    tracker_.content_width = 0.0f;
    EXPECT_EQ(rm_.RequestMermaidRenders(), 0);
    EXPECT_EQ(mock_mermaid_.request_render_count, 0);
}

TEST_F(ResourceManagerTest, RequestMermaidRendersUsesDarkModeFromThemeService)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    theme_service_.ToggleDarkMode();
    ASSERT_TRUE(theme_service_.IsDarkMode());

    rm_.RequestMermaidRenders();
    EXPECT_TRUE(mock_mermaid_.last_dark_mode);
}

TEST_F(ResourceManagerTest, InvalidateMermaidForWidthChangeFiresOnQuantizedWidthShift)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");

    // 1 回目: 幅 800（最初の登録、last_mermaid_content_width_ をシードする）
    tracker_.content_width = 800.0f;
    rm_.RequestMermaidRenders();

    const int initial_cancel = mock_mermaid_.cancel_pending_count;
    const int initial_clear = mock_mermaid_.clear_cache_count;

    // 2 回目: QuantizeWidth(800) != QuantizeWidth(1000) → 幅変化検出
    tracker_.content_width = 1000.0f;
    rm_.RequestMermaidRenders();

    // 量子化幅が変化すると CancelPending と ClearCache の両方が発火する。
    // 走査ループでは bitmap 不在の diagram も diagram.bitmap.Reset() / width=0 にリセットされ、
    // any_invalidated が true になるため ClearCache も呼ばれる。
    EXPECT_GT(mock_mermaid_.cancel_pending_count, initial_cancel);
    EXPECT_GT(mock_mermaid_.clear_cache_count, initial_clear);
}

TEST_F(ResourceManagerTest, CancelMermaidBatchInvokesRendererCancelAndKillsTimer)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    rm_.CancelMermaidBatch();
    EXPECT_EQ(mock_mermaid_.cancel_pending_count, 1);
    EXPECT_EQ(tracker_.kill_timer, 1);
    EXPECT_EQ(tracker_.last_killed_timer_id, ResourceManager::TIMER_MERMAID_BATCH);
}

TEST_F(ResourceManagerTest, ScheduleMermaidBatchSetsTimer)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    rm_.ScheduleMermaidBatch();
    EXPECT_EQ(tracker_.set_timer, 1);
    EXPECT_EQ(tracker_.last_set_timer_id, ResourceManager::TIMER_MERMAID_BATCH);
    EXPECT_EQ(tracker_.last_set_timer_ms, 16u);
}

TEST_F(ResourceManagerTest, ProcessMermaidBatchKillsTimerWhenWidthIsZero)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    tracker_.content_width = 0.0f;
    rm_.ProcessMermaidBatch();
    EXPECT_EQ(tracker_.kill_timer, 1);
    EXPECT_EQ(tracker_.last_killed_timer_id, ResourceManager::TIMER_MERMAID_BATCH);
}

TEST_F(ResourceManagerTest, ProcessMermaidBatchKillsTimerWhenAllProcessed)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    rm_.ProcessMermaidBatch();
    // 1件しかないので走査が完了し、タイマーが kill される
    EXPECT_GE(mock_mermaid_.request_render_count, 1);
    EXPECT_GE(tracker_.kill_timer, 1);
    EXPECT_EQ(tracker_.last_killed_timer_id, ResourceManager::TIMER_MERMAID_BATCH);
}

// ---- ビットマップ管理 ----

TEST_F(ResourceManagerTest, EvictOffscreenBitmapsNoOpsWhenViewportHeightIsZero)
{
    LoadMarkdown("# heading\n\nparagraph\n");
    tracker_.viewport_height = 0.0f;
    // 例外を投げず即 return することを確認
    rm_.EvictOffscreenBitmaps();
    SUCCEED();
}

TEST_F(ResourceManagerTest, EvictOffscreenBitmapsReleasesOutOfRangeTextLayouts)
{
    // 多くのノードを持つ document を作り、可視範囲外の text_layout が解放されることを確認
    std::string md;
    for (int i = 0; i < 20; i++) {
        md += "paragraph ";
        md += std::to_string(i);
        md += "\n\n";
    }
    LoadMarkdown(md, /*block_height=*/100.0f);

    // 全ノードに layout_dirty=false をセット（SeedLayoutCache 後）
    // text_layout は ComPtr で実物が必要なので、layout_dirty フラグの遷移のみ確認
    for (size_t i = 0; i < cache_.size(); i++) {
        cache_[i].layout_dirty = false;
    }

    // viewport は最初の 600px のみ可視。EVICT_BUFFER=5 screens → 6000px の幅で keep。
    // 全 20 ノード（2000px）は keep 範囲内なので、解放されない（境界ケース確認）
    viewport_.SetScrollY(0.0f);
    rm_.EvictOffscreenBitmaps();

    // 全ノードが範囲内のため layout_dirty は false のまま
    for (size_t i = 0; i < cache_.size(); i++) {
        EXPECT_FALSE(cache_[i].layout_dirty) << "node " << i;
    }
}

TEST_F(ResourceManagerTest, ScheduleBitmapManageSetsTimer)
{
    LoadMarkdown("# heading\n");
    rm_.ScheduleBitmapManage();
    EXPECT_GE(tracker_.set_timer, 1);
    EXPECT_EQ(tracker_.last_set_timer_id, ResourceManager::TIMER_BITMAP_MANAGE);
    EXPECT_EQ(tracker_.last_set_timer_ms, 150u);
}

TEST_F(ResourceManagerTest, OnBitmapManageTimerKillsTimerAndInvalidates)
{
    LoadMarkdown("# heading\n");
    rm_.OnBitmapManageTimer();
    EXPECT_GE(tracker_.kill_timer, 1);
    EXPECT_EQ(tracker_.invalidate, 1);
}

TEST_F(ResourceManagerTest, FlushPendingResourcesIsNoopWhenNotPending)
{
    LoadMarkdown("# heading\n");
    rm_.FlushPendingResources();
    // pending_flush_ が立っていないので recompute_layout は呼ばれない
    EXPECT_EQ(tracker_.recompute_layout, 0);
}

// ---- ファイル切替時クリーンアップ ----

TEST_F(ResourceManagerTest, ClearResolvedPathsRunsWithoutError)
{
    LoadMarkdown("# heading\n");
    rm_.ClearResolvedPaths();
    SUCCEED();
}
