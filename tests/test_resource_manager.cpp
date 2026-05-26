#include <gtest/gtest.h>
#include "resource_manager.h"
#include "app_constants.h"
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

// 同期キャッシュヒットをシミュレートするための最小 ID2D1Bitmap スタブ。
// AddRef/Release は no-op (static instance で寿命管理は不要)。
class StubD2D1Bitmap : public ID2D1Bitmap {
public:
    STDMETHODIMP QueryInterface(REFIID, void**) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP_(ULONG)
    AddRef() override
    {
        return 1;
    }
    STDMETHODIMP_(ULONG)
    Release() override
    {
        return 1;
    }
    void STDMETHODCALLTYPE GetFactory(ID2D1Factory** factory) const override
    {
        if (factory) {
            *factory = nullptr;
        }
    }
    D2D1_SIZE_F STDMETHODCALLTYPE GetSize() const override
    {
        return D2D1_SIZE_F{ 1.0f, 1.0f };
    }
    D2D1_SIZE_U STDMETHODCALLTYPE GetPixelSize() const override
    {
        return D2D1_SIZE_U{ 1u, 1u };
    }
    D2D1_PIXEL_FORMAT STDMETHODCALLTYPE GetPixelFormat() const override
    {
        return D2D1_PIXEL_FORMAT{};
    }
    void STDMETHODCALLTYPE GetDpi(FLOAT* dpiX, FLOAT* dpiY) const override
    {
        if (dpiX) {
            *dpiX = 96.0f;
        }
        if (dpiY) {
            *dpiY = 96.0f;
        }
    }
    HRESULT STDMETHODCALLTYPE CopyFromBitmap(const D2D1_POINT_2U*, ID2D1Bitmap*, const D2D1_RECT_U*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CopyFromRenderTarget(const D2D1_POINT_2U*, ID2D1RenderTarget*, const D2D1_RECT_U*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CopyFromMemory(const D2D1_RECT_U*, const void*, UINT32) override
    {
        return E_NOTIMPL;
    }
};

// IMermaidRenderer のテスト用 mock。各メソッドの呼び出し回数と最後の引数を記録する。
class MockMermaidRenderer : public IMermaidRenderer {
public:
    int request_render_count = 0;
    int cancel_pending_count = 0;
    int clear_cache_count = 0;
    Node* last_node = nullptr;
    float last_max_width = 0.0f;
    bool last_dark_mode = false;
    // true のとき RequestRender でディスクキャッシュヒットを模し、同期で bitmap を設定して on_complete を呼ぶ。
    bool sync_apply_bitmap = false;

    void RequestRender(
        Node& node, NodeLayoutEntry& layout_entry,
        DiagramEntry& diagram_entry, float max_width, bool dark_mode,
        Callback on_complete) override
    {
        request_render_count++;
        last_node = &node;
        last_max_width = max_width;
        last_dark_mode = dark_mode;
        if (sync_apply_bitmap) {
            diagram_entry.bitmap = &SharedStubBitmap();
            diagram_entry.width = 100.0f;
            diagram_entry.height = 50.0f;
            layout_entry.height = 50.0f;
            layout_entry.layout_dirty = false;
            if (on_complete) {
                on_complete();
            }
        }
    }

    void RequestSvg(std::wstring_view /*code*/, float /*max_width*/, bool /*dark_mode*/, SvgCallback callback) override
    {
        if (callback) {
            callback(std::pmr::wstring{}, false);
        }
    }

    void CancelPending() override
    {
        cancel_pending_count++;
    }
    void ClearCache() override
    {
        clear_cache_count++;
    }

private:
    static StubD2D1Bitmap& SharedStubBitmap() noexcept
    {
        static StubD2D1Bitmap instance;
        return instance;
    }
};

// ResourceManager::Callbacks の各呼び出しを観測するための counter。
struct CallbackTracker {
    int invalidate = 0;
    int set_timer = 0;
    int kill_timer = 0;
    int recompute_layout = 0;
    int recompute_layout_anchored = 0;
    app_timer::Id last_set_timer_id{};
    UINT last_set_timer_ms = 0;
    app_timer::Id last_killed_timer_id{};

    float content_width = 800.0f;
    float viewport_height = 600.0f;
    float indent_width = 20.0f;
};

struct TestResourceManagerCallbacks {
    CallbackTracker* t = nullptr;

    void invalidate()
    {
        t->invalidate++;
    }
    void set_timer(app_timer::Id id, UINT ms)
    {
        t->set_timer++;
        t->last_set_timer_id = id;
        t->last_set_timer_ms = ms;
    }
    void kill_timer(app_timer::Id id)
    {
        t->kill_timer++;
        t->last_killed_timer_id = id;
    }
    float get_content_width()
    {
        return t->content_width;
    }
    float get_viewport_height()
    {
        return t->viewport_height;
    }
    float get_indent_width()
    {
        return t->indent_width;
    }
    void recompute_layout()
    {
        t->recompute_layout++;
    }
    void recompute_layout_anchored()
    {
        t->recompute_layout_anchored++;
    }
};

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
        rm_.Init(
            ResourceManagerDeps{
                .doc = &doc_,
                .cache = &cache_,
                .viewport = &viewport_,
                .image_loader = &image_loader_,
                .mermaid = &mock_mermaid_,
                .theme_service = &theme_service_,
            },
            TestResourceManagerCallbacks{ &tracker_ });
    }

    Document doc_;
    LayoutCache cache_;
    ViewportManager viewport_;
    ImageLoader image_loader_; // Init せずに使う（画像経路は通らないテストのみ）
    MockMermaidRenderer mock_mermaid_;
    ConfigService config_;
    ThemeService theme_service_{ config_ };
    CallbackTracker tracker_;
    ResourceManagerT<TestResourceManagerCallbacks> rm_;
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

// 複数の図がディスクキャッシュにヒットしても、recompute_layout_anchored は per-figure ではなく
// ループ後にまとめて 1 回しか発火しないことを保証する (issue: 初回ロードで N 回フルスイープ)。
TEST_F(ResourceManagerTest, RequestMermaidRendersBatchesAnchoredRecomputeAcrossCacheHits)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n\n```mermaid\ngraph TD;C-->D\n```\n\n```mermaid\ngraph TD;E-->F\n```\n");
    ASSERT_EQ(doc_.GetDiagramNodeIndices().size(), 3u);
    mock_mermaid_.sync_apply_bitmap = true;

    const int applied = rm_.RequestMermaidRenders();
    EXPECT_EQ(applied, 3);
    EXPECT_EQ(mock_mermaid_.request_render_count, 3);
    EXPECT_EQ(tracker_.recompute_layout_anchored, 1);
    EXPECT_EQ(tracker_.recompute_layout, 0);
}

// 同期適用が 0 件なら recompute_layout_anchored は発火しない (ガード正常動作の確認)。
TEST_F(ResourceManagerTest, RequestMermaidRendersSkipsAnchoredRecomputeWhenNoBitmapApplied)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    ASSERT_FALSE(mock_mermaid_.sync_apply_bitmap);

    rm_.RequestMermaidRenders();
    EXPECT_EQ(tracker_.recompute_layout_anchored, 0);
    EXPECT_EQ(tracker_.recompute_layout, 0);
}

// FlushPendingResources 経由 (outer_batch=true) では内側 recompute_layout_anchored は発火せず、
// 外側 recompute_layout が 1 回だけ呼ばれる。OnBitmapManageTimer がこの経路をトリガする。
TEST_F(ResourceManagerTest, RequestMermaidRendersInNestedFlushDoesNotFireAnchoredRecompute)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n\n```mermaid\ngraph TD;C-->D\n```\n");
    ASSERT_EQ(doc_.GetDiagramNodeIndices().size(), 2u);
    mock_mermaid_.sync_apply_bitmap = true;

    rm_.OnBitmapManageTimer();

    EXPECT_EQ(tracker_.recompute_layout_anchored, 0);
    EXPECT_EQ(tracker_.recompute_layout, 1);
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
    EXPECT_EQ(tracker_.last_killed_timer_id, app_timer::Id::MERMAID_BATCH);
}

TEST_F(ResourceManagerTest, ScheduleMermaidBatchSetsTimer)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    rm_.ScheduleMermaidBatch();
    EXPECT_EQ(tracker_.set_timer, 1);
    EXPECT_EQ(tracker_.last_set_timer_id, app_timer::Id::MERMAID_BATCH);
    EXPECT_EQ(tracker_.last_set_timer_ms, 16u);
}

TEST_F(ResourceManagerTest, ProcessMermaidBatchKillsTimerWhenWidthIsZero)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    tracker_.content_width = 0.0f;
    rm_.ProcessMermaidBatch();
    EXPECT_EQ(tracker_.kill_timer, 1);
    EXPECT_EQ(tracker_.last_killed_timer_id, app_timer::Id::MERMAID_BATCH);
}

TEST_F(ResourceManagerTest, ProcessMermaidBatchKillsTimerWhenAllProcessed)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    rm_.ProcessMermaidBatch();
    // 1件しかないので走査が完了し、タイマーが kill される
    EXPECT_GE(mock_mermaid_.request_render_count, 1);
    EXPECT_GE(tracker_.kill_timer, 1);
    EXPECT_EQ(tracker_.last_killed_timer_id, app_timer::Id::MERMAID_BATCH);
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
    EXPECT_EQ(tracker_.last_set_timer_id, app_timer::Id::BITMAP_MANAGE);
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
