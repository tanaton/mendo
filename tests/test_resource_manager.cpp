#include <gtest/gtest.h>
#include "resource_manager.h"
#include "app_constants.h"
#include "app_state.h"
#include "document.h"
#include "file_io.h"
#include "layout_cache.h"
#include "viewport_manager.h"
#include "image_loader.h"
#include "mermaid_file_cache.h"
#include "mermaid_renderer_interface.h"
#include "reducer_harness.h"
#include "theme.h"
#include "test_helpers.h"
#include <algorithm>
#include <ranges>
#include <string>
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

// AddRef/Release が何もしないので static のまま共用する。
StubD2D1Bitmap& SharedStubBitmap() noexcept
{
    static StubD2D1Bitmap instance;
    return instance;
}

// IMermaidRenderer のテスト用 mock。各メソッドの呼び出し回数と最後の引数を記録する。
class MockMermaidRenderer : public IMermaidRenderer {
public:
    int request_render_count = 0;
    std::vector<const Node*> requested;
    const Node* last_node = nullptr;
    float last_max_width = 0.0f;
    bool last_dark_mode = false;
    // true のとき RequestRender でキャッシュヒットを模し、同期で bitmap を設定して true を返す。
    bool sync_apply_bitmap = false;
    // true のとき RequestRender でエラーの確定を模し、bitmap なしで true を返す。
    bool sync_error = false;

    bool RequestRender(
        const Node& node,
        DiagramEntry& diagram_entry, float max_width, bool dark_mode) override
    {
        request_render_count++;
        requested.push_back(&node);
        last_node = &node;
        last_max_width = max_width;
        last_dark_mode = dark_mode;
        if (sync_error) {
            diagram_entry.error = { DiagramError::Kind::Render, L"Parse error" };
            return true;
        }
        if (!sync_apply_bitmap) {
            return false;
        }
        diagram_entry.bitmap = &SharedStubBitmap();
        diagram_entry.width = 100.0f;
        diagram_entry.height = 50.0f;
        return true;
    }

    void RequestSvg(std::string_view /*code*/, float /*max_width*/, bool /*dark_mode*/, SvgCallback callback) override
    {
        if (callback) {
            callback(std::wstring{}, false);
        }
    }

    void DropQueued() override
    {
        drop_queued_count++;
    }
    int drop_queued_count = 0;
};

// ResourceManagerHost の各呼び出しを観測するための counter。
struct CallbackTracker {
    int set_timer = 0;
    int kill_timer = 0;
    int recompute_layout = 0;
    layout::HeightChangeRange last_change = {};
    app_timer::Id last_set_timer_id{};
    UINT last_set_timer_ms = 0;
};

struct TestResourceManagerHost final : ResourceManagerHost {
    explicit TestResourceManagerHost(CallbackTracker& tracker) : t(&tracker)
    {}
    CallbackTracker* t;

    void SetTimer(app_timer::Id id, UINT ms) override
    {
        t->set_timer++;
        t->last_set_timer_id = id;
        t->last_set_timer_ms = ms;
    }
    void KillTimer(app_timer::Id) override
    {
        t->kill_timer++;
    }
    void RecomputeLayout(layout::HeightChangeRange changed) override
    {
        t->recompute_layout++;
        t->last_change = changed;
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
    // 画像の適用は doc_dir.empty() で早期 return するため、テスト用ドキュメントパスは
    // parent_path を持つ絶対パスでなければならない。
    static constexpr const wchar_t* kTestDocPath = L"C:\\dir\\test.md";
    static constexpr float kContentWidth = 800.0f;
    static constexpr float kViewportHeight = 600.0f;

    void SetUp() override
    {
        InitReducerState(state_, theme_);
        theme_.indent_width = 20.0f;
        state_.view.panes.SetSidePaneVisible(PaneTarget::File, false);
        state_.view.panes.SetSidePaneVisible(PaneTarget::Toc, false);
        SetContentWidth(kContentWidth);
    }

    // 余白 0 なので、本文の幅はウィンドウの幅と同じ。
    void SetContentWidth(float width)
    {
        SetWindowSize(state_, width, kViewportHeight);
    }

    // Markdown を Document に流し込み、対応する LayoutCache をシードしたうえで
    // ResourceManager を初期化する。テストごとに 1 度だけ呼ぶ。
    void LoadMarkdown(std::string_view md, float block_height = 100.0f, const wchar_t* doc_path = kTestDocPath)
    {
        std::string utf8(md);
        doc_ = Document::FromMarkdown(std::move(utf8), doc_path);
        cache_ = SeedLayoutCache(doc_.GetNodes().size(), block_height);
        rm_.Init(state_, image_loader_, mock_mermaid_, file_cache_, host_);
    }

    Theme theme_{};
    AppState state_;
    MermaidFileCache file_cache_;
    Document& doc_ = state_.document.doc;
    LayoutCache& cache_ = state_.document.layout_cache;
    ViewportManager& viewport_ = state_.view.viewport;
    ImageLoader image_loader_; // Init せずに使う（画像経路は通らないテストのみ）
    MockMermaidRenderer mock_mermaid_;
    CallbackTracker tracker_;
    TestResourceManagerHost host_{ tracker_ };
    ResourceManager rm_;
};

// ---- 画像経路 ----

TEST_F(ResourceManagerTest, LoadImagesAppliesNothingWhenNoImagesInDocument)
{
    LoadMarkdown("# heading\n\nparagraph text\n");
    rm_.LoadImages();
    EXPECT_EQ(tracker_.recompute_layout, 0);
}

TEST_F(ResourceManagerTest, LoadImagesAppliesNothingWhenContentWidthIsZero)
{
    LoadMarkdown("# heading\n\n![alt](image.png)\n");
    SetContentWidth(0.0f);
    rm_.LoadImages();
    EXPECT_EQ(tracker_.recompute_layout, 0);
}

TEST_F(ResourceManagerTest, LoadImagesAppliesNothingForRemoteImages)
{
    // http:// 等のリモート画像は適用対象にならないため、適用数は 0 のまま。
    LoadMarkdown("![alt](https://example.com/foo.png)\n");
    rm_.LoadImages();
    EXPECT_EQ(tracker_.recompute_layout, 0);
}

// ---- 画像経路: リロード時の ApplyCachedImagesForReload (issue #148) ----

TEST_F(ResourceManagerTest, ApplyCachedImagesAllForReloadAppliesHeightFromCache)
{
    LoadMarkdown("# heading\n\n![alt](foo.png)\n");
    image_loader_.InsertCacheEntryForTest(L"C:\\dir\\foo.png", 1000.0f, 800.0f);

    // 適用前: SeedLayoutCache が block_height=100 で初期化している。
    const auto& image_indices = doc_.GetImageNodeIndices();
    ASSERT_EQ(image_indices.size(), 1u);
    const size_t img_idx = image_indices[0];
    EXPECT_FLOAT_EQ(cache_[img_idx].height, 100.0f);

    EXPECT_TRUE(rm_.ApplyCachedImagesForReload({ 0, SIZE_MAX }));

    // content_width=800, image_width=1000 → スケール係数 800/1000、
    // height=800 * 0.8 = 640
    EXPECT_FLOAT_EQ(cache_[img_idx].height, 640.0f);
    EXPECT_FALSE(cache_[img_idx].layout_dirty);
}

// 高さを更新したノードだけを再計算範囲として渡す (先頭からの全件再計算を避ける)。
TEST_F(ResourceManagerTest, LoadImagesPassesChangedNodeRangeToRecompute)
{
    LoadMarkdown("# heading\n\ntext\n\n![alt](foo.png)\n");
    image_loader_.InsertCacheEntryForTest(L"C:\\dir\\foo.png", 400.0f, 300.0f);
    const size_t img_idx = doc_.GetImageNodeIndices()[0];

    rm_.LoadImages();

    EXPECT_EQ(tracker_.recompute_layout, 1);
    EXPECT_EQ(tracker_.last_change.first, img_idx);
    EXPECT_EQ(tracker_.last_change.last, img_idx);
}

// DPI が変わると同じビットマップでも DIP の寸法が変わる。前の DPI で決めた寸法のまま残さない。
TEST_F(ResourceManagerTest, LoadImagesUpdatesSizeOfSameBitmap)
{
    auto& bitmap = SharedStubBitmap();
    LoadMarkdown("![a](one.png)\n");
    image_loader_.InsertCacheEntryForTest(L"C:\\dir\\one.png", 400.0f, 300.0f, &bitmap);
    const size_t img_idx = doc_.GetImageNodeIndices()[0];
    rm_.LoadImages();
    auto* diagram = cache_.FindDiagram(img_idx);
    ASSERT_NE(diagram, nullptr);
    ASSERT_EQ(diagram->bitmap.Get(), &bitmap);

    // 200% のときに入れた寸法。
    diagram->width = 200.0f;
    diagram->height = 150.0f;
    rm_.LoadImages();
    EXPECT_FLOAT_EQ(diagram->width, 400.0f);
    EXPECT_FLOAT_EQ(cache_[img_idx].height, 300.0f);
    EXPECT_EQ(tracker_.recompute_layout, 2);

    rm_.LoadImages();
    EXPECT_EQ(tracker_.recompute_layout, 2) << "寸法が同じなら適用し直さない";
}

TEST_F(ResourceManagerTest, ApplyCachedImagesAllForReloadKeepsHeightWhenWithinContentWidth)
{
    LoadMarkdown("![alt](small.png)\n");
    // content_width=800 より小さい画像はスケール無し。
    image_loader_.InsertCacheEntryForTest(L"C:\\dir\\small.png", 400.0f, 300.0f);

    EXPECT_TRUE(rm_.ApplyCachedImagesForReload({ 0, SIZE_MAX }));

    const size_t img_idx = doc_.GetImageNodeIndices()[0];
    EXPECT_FLOAT_EQ(cache_[img_idx].height, 300.0f);
}

// src は UTF-8。path を char から直接構築すると ACP (CP932 等) 解釈で非 ASCII 名が化け、キャッシュを引けない。
TEST_F(ResourceManagerTest, ApplyCachedImagesResolvesNonAsciiFileName)
{
    LoadMarkdown("![alt](\xE7\x94\xBB\xE5\x83\x8F.png)\n"); // 画像.png
    image_loader_.InsertCacheEntryForTest(L"C:\\dir\\\u753B\u50CF.png", 400.0f, 300.0f);

    EXPECT_TRUE(rm_.ApplyCachedImagesForReload({ 0, SIZE_MAX }));

    const size_t img_idx = doc_.GetImageNodeIndices()[0];
    EXPECT_FLOAT_EQ(cache_[img_idx].height, 300.0f);
}

// md4c は src をパーセントエンコードのまま渡すので、ファイルパスにする前にデコードする。
TEST_F(ResourceManagerTest, ApplyCachedImagesDecodesPercentEncodedPath)
{
    LoadMarkdown("![alt](my%20images/%E7%94%BB%E5%83%8F.png)\n");
    image_loader_.InsertCacheEntryForTest(L"C:\\dir\\my images\\\u753B\u50CF.png", 400.0f, 300.0f);

    EXPECT_TRUE(rm_.ApplyCachedImagesForReload({ 0, SIZE_MAX }));

    const size_t img_idx = doc_.GetImageNodeIndices()[0];
    EXPECT_FLOAT_EQ(cache_[img_idx].height, 300.0f);
}

// UNC の画像はキャッシュにあっても引かない。判定はデコード後のパスにかける。
TEST_F(ResourceManagerTest, ApplyCachedImagesIgnoresUncPaths)
{
    // Markdown の \\ は \ 1 個になるので、UNC の \\ は \\\\ と書く。
    LoadMarkdown("![a](\\\\\\\\attacker\\share\\a.png)\n\n![c](%5C%5Cattacker%5Cshare%5Cc.png)\n");
    image_loader_.InsertCacheEntryForTest(L"\\\\attacker\\share\\a.png", 400.0f, 300.0f);
    image_loader_.InsertCacheEntryForTest(L"\\\\attacker\\share\\c.png", 400.0f, 300.0f);
    ASSERT_EQ(doc_.GetImageNodeIndices().size(), 2u);

    EXPECT_FALSE(rm_.ApplyCachedImagesForReload({ 0, SIZE_MAX }));
}

// 先頭 2 文字がスラッシュなら向きを問わず UNC。デコード後に "//" になる記述も同じ。
// キーは判定を外したときに解決される UNC パスで入れ、引かないことを確かめる。
TEST_F(ResourceManagerTest, ApplyCachedImagesIgnoresSlashUncForms)
{
    LoadMarkdown("![b](//attacker/share/b.png)\n\n![c](%2F%2Fattacker%2Fshare%2Fc.png)\n\n![d](/%5Cattacker/share/d.png)\n");
    ASSERT_EQ(doc_.GetImageNodeIndices().size(), 3u);
    for (const wchar_t* key : { L"\\\\attacker\\share\\b.png", L"\\\\attacker\\share\\c.png", L"\\\\attacker\\share\\d.png" }) {
        image_loader_.InsertCacheEntryForTest(key, 400.0f, 300.0f);
    }
    EXPECT_FALSE(rm_.ApplyCachedImagesForReload({ 0, SIZE_MAX }));
}

// デコードは 1 回だけ。二重エンコードの %255C は文書フォルダ内の "%5C" という名前のファイルになる。
TEST_F(ResourceManagerTest, ApplyCachedImagesDecodesPercentOnlyOnce)
{
    LoadMarkdown("![e](%255C%255Cattacker%255Cshare%255Ce.png)\n");
    image_loader_.InsertCacheEntryForTest(L"\\\\attacker\\share\\e.png", 400.0f, 100.0f);
    image_loader_.InsertCacheEntryForTest(L"C:\\dir\\%5C%5Cattacker%5Cshare%5Ce.png", 400.0f, 300.0f);

    EXPECT_TRUE(rm_.ApplyCachedImagesForReload({ 0, SIZE_MAX }));
    EXPECT_FLOAT_EQ(cache_[doc_.GetImageNodeIndices()[0]].height, 300.0f);
}

// "://" を含む src はリモートとして解決しない。文書フォルダとの結合結果がキャッシュにあっても引かない。
TEST_F(ResourceManagerTest, ApplyCachedImagesIgnoresSchemeEvenIfJoinedPathIsCached)
{
    LoadMarkdown("![alt](https://example.com/foo.png)\n");
    const auto joined = (std::filesystem::path(L"C:\\dir") / L"https://example.com/foo.png").lexically_normal();
    image_loader_.InsertCacheEntryForTest(joined.wstring(), 400.0f, 300.0f);
    EXPECT_FALSE(rm_.ApplyCachedImagesForReload({ 0, SIZE_MAX }));
}

// 文書を \\?\ 付きのパスで開いていても、相対パスの .. で \\?\UNC\host へ抜けさせない。
TEST_F(ResourceManagerTest, ApplyCachedImagesRelativePathCannotEscapeToUncFromDevicePrefixedDoc)
{
    LoadMarkdown("![a](../../UNC/attacker/share/a.png)\n", 100.0f, L"\\\\?\\C:\\dir\\test.md");
    const auto escaped = (std::filesystem::path(L"\\\\?\\C:\\dir") / L"../../UNC/attacker/share/a.png").lexically_normal();
    ASSERT_FALSE(path_util::IsLocalPath(escaped.native())) << escaped.native();
    image_loader_.InsertCacheEntryForTest(escaped.wstring(), 400.0f, 300.0f);
    EXPECT_FALSE(rm_.ApplyCachedImagesForReload({ 0, SIZE_MAX }));
}

// \\?\ 付きの文書でも、文書フォルダの下の相対パスの画像は従来どおり表示する。
TEST_F(ResourceManagerTest, ApplyCachedImagesResolvesRelativePathInDevicePrefixedDoc)
{
    LoadMarkdown("![a](img/../a.png)\n", 100.0f, L"\\\\?\\C:\\dir\\test.md");
    image_loader_.InsertCacheEntryForTest(L"C:\\dir\\a.png", 400.0f, 300.0f);
    EXPECT_TRUE(rm_.ApplyCachedImagesForReload({ 0, SIZE_MAX }));
    EXPECT_FLOAT_EQ(cache_[doc_.GetImageNodeIndices()[0]].height, 300.0f);
}

TEST_F(ResourceManagerTest, ApplyCachedImagesAllForReloadAppliesNothingWhenCacheMisses)
{
    LoadMarkdown("![alt](missing.png)\n");
    // image_loader_ にキャッシュ未挿入 → 0 件。
    EXPECT_FALSE(rm_.ApplyCachedImagesForReload({ 0, SIZE_MAX }));

    const size_t img_idx = doc_.GetImageNodeIndices()[0];
    EXPECT_FLOAT_EQ(cache_[img_idx].height, 100.0f); // SeedLayoutCache の block_height のまま
}

TEST_F(ResourceManagerTest, ApplyCachedImagesAllForReloadAppliesBeyondVisibleRange)
{
    // issue #148 の本質: 可視範囲外の画像も全件走査して height を更新する。
    // 画像を 2 個配置し、2 個目を viewport から大きく外して LoadImages との差を出す。
    LoadMarkdown("![one](one.png)\n\n![two](two.png)\n", /*block_height=*/3000.0f);

    const auto& image_indices = doc_.GetImageNodeIndices();
    ASSERT_EQ(image_indices.size(), 2u);

    image_loader_.InsertCacheEntryForTest(L"C:\\dir\\one.png", 400.0f, 300.0f);
    image_loader_.InsertCacheEntryForTest(L"C:\\dir\\two.png", 400.0f, 250.0f);

    // 先読み範囲は可視 ± PREFETCH_BUFFER_SCREENS 画面。2個目は y=3000 にあり範囲外なので、
    // LoadImages は 1 件のみ適用する。
    rm_.LoadImages();
    EXPECT_EQ(tracker_.last_change.first, image_indices[0]);
    EXPECT_EQ(tracker_.last_change.last, image_indices[0]);

    // 2個目の高さはまだ SeedLayoutCache の block_height=3000 のまま。
    EXPECT_FLOAT_EQ(cache_[image_indices[1]].height, 3000.0f);

    // ApplyCachedImagesForReload は可視範囲に縛られないので残りの 1 件も適用する。
    // 1 個目は LoadImages で diagram.bitmap がセットされていないのでもう一度適用される。
    EXPECT_TRUE(rm_.ApplyCachedImagesForReload({ 0, SIZE_MAX }));
    EXPECT_FLOAT_EQ(cache_[image_indices[1]].height, 250.0f);
    EXPECT_FALSE(cache_[image_indices[1]].layout_dirty);
}

// リロードでは変わった範囲のノードにだけ当てる (引き継いだノードは実測の高さを持っている)。
TEST_F(ResourceManagerTest, ApplyCachedImagesAllForReloadAppliesOnlyGivenRange)
{
    LoadMarkdown("![one](one.png)\n\n![two](two.png)\n", /*block_height=*/3000.0f);
    const auto& image_indices = doc_.GetImageNodeIndices();
    ASSERT_EQ(image_indices.size(), 2u);
    image_loader_.InsertCacheEntryForTest(L"C:\\dir\\one.png", 400.0f, 300.0f);
    image_loader_.InsertCacheEntryForTest(L"C:\\dir\\two.png", 400.0f, 250.0f);

    EXPECT_TRUE(rm_.ApplyCachedImagesForReload({ image_indices[1], image_indices[1] + 1 }));
    EXPECT_FLOAT_EQ(cache_[image_indices[0]].height, 3000.0f);
    EXPECT_FLOAT_EQ(cache_[image_indices[1]].height, 250.0f);
}

TEST_F(ResourceManagerTest, ApplyCachedImagesAllForReloadIgnoresRemoteImages)
{
    LoadMarkdown("![alt](https://example.com/foo.png)\n\n![local](foo.png)\n");
    // キャッシュが空だと早期 return してリモート判定まで届かないので、ローカル画像を 1 件入れておく。
    image_loader_.InsertCacheEntryForTest(L"C:\\dir\\foo.png", 400.0f, 300.0f);
    EXPECT_TRUE(rm_.ApplyCachedImagesForReload({ 0, SIZE_MAX }));
}

TEST_F(ResourceManagerTest, ApplyCachedImagesAllForReloadAppliesNothingWhenContentWidthIsZero)
{
    LoadMarkdown("![alt](foo.png)\n");
    image_loader_.InsertCacheEntryForTest(L"C:\\dir\\foo.png", 1000.0f, 800.0f);
    SetContentWidth(0.0f);
    EXPECT_FALSE(rm_.ApplyCachedImagesForReload({ 0, SIZE_MAX }));
}

// ---- Mermaid 経路 ----

TEST_F(ResourceManagerTest, RequestMermaidRendersInvokesRendererForVisibleDiagram)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    ASSERT_FALSE(doc_.GetDiagramNodeIndices().empty());

    rm_.RequestMermaidRenders();
    EXPECT_EQ(tracker_.recompute_layout, 0); // mock は bitmap をセットしない
    EXPECT_EQ(mock_mermaid_.request_render_count, 1);
    EXPECT_FLOAT_EQ(mock_mermaid_.last_max_width, kContentWidth);
    EXPECT_FALSE(mock_mermaid_.last_dark_mode);
}

TEST_F(ResourceManagerTest, RequestMermaidRendersSkipsBeyondVisibleRange)
{
    // diagram が 2 個。ノード間隔を先読み範囲 (可視 ± PREFETCH_BUFFER_SCREENS 画面) より広くし、
    // 1個目を可視範囲、2個目を範囲外に置く。
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n\n```mermaid\ngraph TD;C-->D\n```\n",
                 /*block_height=*/3000.0f);
    ASSERT_GE(doc_.GetDiagramNodeIndices().size(), 2u);

    rm_.RequestMermaidRenders();
    // 1個目 (y=0, 可視範囲内) のみ Render される
    EXPECT_EQ(mock_mermaid_.request_render_count, 1);
}

TEST_F(ResourceManagerTest, RequestMermaidRendersZeroWidthIsNoOp)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    SetContentWidth(0.0f);
    rm_.RequestMermaidRenders();
    EXPECT_EQ(tracker_.recompute_layout, 0);
    EXPECT_EQ(mock_mermaid_.request_render_count, 0);
}

// エラー確定 (error 非空) の図は再リクエストされない (issue #271: 失敗レンダの無限リトライ防止)。
TEST_F(ResourceManagerTest, RequestMermaidRendersSkipsErroredDiagram)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    ASSERT_FALSE(doc_.GetDiagramNodeIndices().empty());
    cache_.EnsureDiagram(doc_.GetDiagramNodeIndices()[0]).error = { DiagramError::Kind::Render, L"Parse error on line 1" };

    rm_.RequestMermaidRenders();
    EXPECT_EQ(tracker_.recompute_layout, 0);
    EXPECT_EQ(mock_mermaid_.request_render_count, 0);
}

// その場でエラーが確定した図はプレースホルダーの文言が変わるので、画像と同じ再レイアウトで描き直させる。
TEST_F(ResourceManagerTest, FlushPendingResourcesRedrawsSynchronousDiagramError)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    mock_mermaid_.sync_error = true;

    rm_.FlushPendingResources();

    EXPECT_EQ(mock_mermaid_.request_render_count, 1);
    EXPECT_EQ(tracker_.recompute_layout, 1);
}

// リロード前の高さの適用は Y を組み直さないので、変えた範囲を後の再レイアウトに持ち越さない。
TEST_F(ResourceManagerTest, ApplyCachedImagesForReloadDoesNotCarryChangeRange)
{
    LoadMarkdown("![one](one.png)\n\n```mermaid\ngraph TD;A-->B\n```\n");
    image_loader_.InsertCacheEntryForTest(L"C:\\dir\\one.png", 400.0f, 300.0f);
    ASSERT_TRUE(rm_.ApplyCachedImagesForReload({ 0, SIZE_MAX }));

    mock_mermaid_.sync_apply_bitmap = true;
    rm_.RequestMermaidRenders();

    const size_t diagram = doc_.GetDiagramNodeIndices()[0];
    EXPECT_EQ(tracker_.recompute_layout, 1);
    EXPECT_EQ(tracker_.last_change.first, diagram);
}

TEST_F(ResourceManagerTest, FlushPendingResourcesSkipsErroredDiagram)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    ASSERT_FALSE(doc_.GetDiagramNodeIndices().empty());
    cache_.EnsureDiagram(doc_.GetDiagramNodeIndices()[0]).error = { DiagramError::Kind::Render, L"Parse error on line 1" };

    rm_.FlushPendingResources();
    EXPECT_EQ(mock_mermaid_.request_render_count, 0);
}

// 量子化幅の変化でエラーがクリアされ、再試行される (issue #271)。
TEST_F(ResourceManagerTest, InvalidateMermaidForWidthChangeClearsError)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    ASSERT_FALSE(doc_.GetDiagramNodeIndices().empty());
    const size_t diagram_index = doc_.GetDiagramNodeIndices()[0];

    SetContentWidth(800.0f);
    rm_.RequestMermaidRenders();
    ASSERT_EQ(mock_mermaid_.request_render_count, 1);

    cache_.EnsureDiagram(diagram_index).error = { DiagramError::Kind::Render, L"Parse error on line 1" };

    SetContentWidth(1000.0f);
    rm_.RequestMermaidRenders();
    EXPECT_TRUE(cache_.FindDiagram(diagram_index)->error.empty());
    EXPECT_EQ(mock_mermaid_.request_render_count, 2);
}

// 幅の変化で描き直す図は、新しい結果が届くまで旧幅の結果を仮表示として残す (プレースホルダに戻して跳ねさせない)。
TEST_F(ResourceManagerTest, InvalidateMermaidForWidthChangeKeepsBitmapAsStale)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    const size_t diagram_index = doc_.GetDiagramNodeIndices()[0];
    mock_mermaid_.sync_apply_bitmap = true;
    SetContentWidth(800.0f);
    rm_.RequestMermaidRenders();
    auto& diagram = *cache_.FindDiagram(diagram_index);
    ASSERT_TRUE(diagram.bitmap);
    // 表示幅に合わせて縮めた図なので、幅が変われば描き直しになる。
    diagram.width = 2000.0f;

    mock_mermaid_.sync_apply_bitmap = false;
    SetContentWidth(1000.0f);
    rm_.RequestMermaidRenders();

    EXPECT_TRUE(diagram.bitmap) << "描き直しの間も旧結果を表示する";
    EXPECT_TRUE(diagram.stale);
    EXPECT_TRUE(diagram.NeedsRender());
    EXPECT_EQ(mock_mermaid_.request_render_count, 2);
}

// 図のコードを書き換えたリロードでは、旧結果と高さを仮表示として新しいノードへ引き継ぐ。
TEST_F(ResourceManagerTest, CarryOverUnchangedKeepsEditedDiagramAsStale)
{
    LoadMarkdown("# t\n\n```mermaid\ngraph TD;A-->B\n```\n\ntail\n");
    const size_t old_index = doc_.GetDiagramNodeIndices()[0];
    mock_mermaid_.sync_apply_bitmap = true;
    rm_.RequestMermaidRenders();
    ASSERT_TRUE(cache_.FindDiagram(old_index)->bitmap);
    cache_.EnsureDiagram(old_index).png = std::make_shared<const ByteBuffer>(std::vector<uint8_t>(4, uint8_t{ 1 }));
    cache_[old_index].height = 321.0f;

    auto next = Document::FromMarkdown(std::string{ "# t\n\n```mermaid\ngraph TD;A-->C\n```\n\ntail\n" }, kTestDocPath);
    auto next_cache = layout::MakeEstimatedLayoutCache(next.GetNodes(), theme_);
    const size_t new_index = next.GetDiagramNodeIndices()[0];

    EXPECT_EQ(layout::CarryOverUnchanged(doc_.GetNodes(), cache_, next.GetNodes(), next_cache, FindUnchangedEnds(doc_.GetNodes(), next.GetNodes())).carried, next.GetNodes().size());
    const auto* d = next_cache.FindDiagram(new_index);
    ASSERT_NE(d, nullptr);
    EXPECT_TRUE(d->bitmap);
    EXPECT_TRUE(d->stale);
    EXPECT_EQ(d->code_hash, 0u) << "新しいコードでハッシュを引き直させる";
    EXPECT_EQ(d->png, nullptr) << "前のコードの絵をコピー・保存させない";
    EXPECT_FLOAT_EQ(next_cache[new_index].height, 321.0f);
}

// 変わった範囲の図の数が変わったら、どの図の続きか分からないので引き継がない。
TEST_F(ResourceManagerTest, CarryOverUnchangedSkipsDiagramsWhenCountChanges)
{
    LoadMarkdown("# t\n\n```mermaid\ngraph TD;A-->B\n```\n\ntail\n");
    mock_mermaid_.sync_apply_bitmap = true;
    rm_.RequestMermaidRenders();

    auto next = Document::FromMarkdown(std::string{ "# t\n\n```mermaid\ngraph TD;X-->Y\n```\n\n```mermaid\ngraph TD;A-->C\n```\n\ntail\n" }, kTestDocPath);
    auto next_cache = layout::MakeEstimatedLayoutCache(next.GetNodes(), theme_);

    layout::CarryOverUnchanged(doc_.GetNodes(), cache_, next.GetNodes(), next_cache, FindUnchangedEnds(doc_.GetNodes(), next.GetNodes()));
    for (const size_t i : next.GetDiagramNodeIndices()) {
        EXPECT_EQ(next_cache.FindDiagram(i), nullptr) << "node " << i;
    }
}

TEST_F(ResourceManagerTest, RequestMermaidRendersUsesDarkModeFromTheme)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    theme_.dark = true;

    rm_.RequestMermaidRenders();
    EXPECT_TRUE(mock_mermaid_.last_dark_mode);
}

// 複数の図がディスクキャッシュにヒットしても、再レイアウトは per-figure ではなく
// ループ後にまとめて 1 回しか発火しないことを保証する (issue: 初回ロードで N 回フルスイープ)。
TEST_F(ResourceManagerTest, RequestMermaidRendersBatchesRecomputeAcrossCacheHits)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n\n```mermaid\ngraph TD;C-->D\n```\n\n```mermaid\ngraph TD;E-->F\n```\n");
    ASSERT_EQ(doc_.GetDiagramNodeIndices().size(), 3u);
    mock_mermaid_.sync_apply_bitmap = true;

    rm_.RequestMermaidRenders();
    EXPECT_EQ(mock_mermaid_.request_render_count, 3);
    EXPECT_EQ(tracker_.recompute_layout, 1);
}

// 同期適用が 0 件なら再レイアウトしない (ガード正常動作の確認)。
TEST_F(ResourceManagerTest, RequestMermaidRendersSkipsRecomputeWhenNoBitmapApplied)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    ASSERT_FALSE(mock_mermaid_.sync_apply_bitmap);

    rm_.RequestMermaidRenders();
    EXPECT_EQ(tracker_.recompute_layout, 0);
}

// FlushPendingResources 経由では図ごとに再レイアウトせず、画像と合わせて 1 回だけ行う。
// OnBitmapManageTimer がこの経路をトリガする。
TEST_F(ResourceManagerTest, FlushRecomputesOnceForAllDiagrams)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n\n```mermaid\ngraph TD;C-->D\n```\n");
    ASSERT_EQ(doc_.GetDiagramNodeIndices().size(), 2u);
    mock_mermaid_.sync_apply_bitmap = true;

    rm_.OnBitmapManageTimer();

    EXPECT_EQ(tracker_.recompute_layout, 1);
}

TEST_F(ResourceManagerTest, InvalidateMermaidForWidthChangeFiresOnQuantizedWidthShift)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");

    // 1 回目: 幅 800（最初の登録、last_mermaid_content_width_ をシードする）
    SetContentWidth(800.0f);
    rm_.RequestMermaidRenders();

    const int initial_drop = mock_mermaid_.drop_queued_count;

    // 2 回目: QuantizeWidth(800) != QuantizeWidth(1000) → 幅変化検出
    SetContentWidth(1000.0f);
    rm_.RequestMermaidRenders();

    // 量子化幅が変化すると旧幅の描画待ちを捨てる。メモリキャッシュは幅込みのキーなので残す。
    EXPECT_GT(mock_mermaid_.drop_queued_count, initial_drop);
}

// 保持範囲を越えて移動したら、旧位置で積んだ描画待ちを捨ててから要求し直す。
TEST_F(ResourceManagerTest, RequestMermaidRendersDropsQueueAfterLargeJump)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n", /*block_height=*/100000.0f);
    rm_.RequestMermaidRenders();
    EXPECT_EQ(mock_mermaid_.drop_queued_count, 0);

    viewport_.SetScrollY(100.0f);
    rm_.RequestMermaidRenders();
    EXPECT_EQ(mock_mermaid_.drop_queued_count, 0) << "小さな移動では捨てない";

    viewport_.SetScrollY(kViewportHeight * 20.0f);
    rm_.RequestMermaidRenders();
    EXPECT_EQ(mock_mermaid_.drop_queued_count, 1);
}

// 要求キューは FIFO なので、見えている図 → 下 → 上の順に要求する。
TEST_F(ResourceManagerTest, RequestMermaidRendersRequestsVisibleFirst)
{
    std::string md;
    for (int i = 0; i < 40; ++i) {
        md += "```mermaid\ngraph TD;A-->B\n```\n\n";
    }
    LoadMarkdown(md);
    viewport_.SetScrollY(1500.0f);
    rm_.RequestMermaidRenders();

    const auto& nodes = doc_.GetNodes();
    std::vector<size_t> order;
    for (const Node* n : mock_mermaid_.requested) {
        order.push_back(static_cast<size_t>(n - nodes.data()));
    }
    // 高さ 100 で並ぶので、可視範囲 [1500, 2100] は 15..21、±2 画面の範囲は 3..33。
    ASSERT_FALSE(order.empty());
    EXPECT_EQ(order.front(), 15u);
    const auto first_above = std::ranges::find_if(order, [](size_t i) static { return i < 15; });
    const auto last_below = std::ranges::find_if(order | std::views::reverse, [](size_t i) static { return i > 21; });
    ASSERT_NE(first_above, order.end());
    ASSERT_NE(last_below, order.rend());
    EXPECT_LE(std::distance(order.begin(), last_below.base()), std::distance(order.begin(), first_above)) << "上側より先に下側を要求する";
}

// 結果が出た通知で、待っていた図を要求し直して反映する。
TEST_F(ResourceManagerTest, RequestMermaidRendersAgainAppliesResolvedDiagrams)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    rm_.RequestMermaidRenders();
    ASSERT_EQ(tracker_.recompute_layout, 0);

    mock_mermaid_.sync_apply_bitmap = true;
    rm_.RequestMermaidRenders();
    const size_t i = doc_.GetDiagramNodeIndices()[0];
    ASSERT_NE(cache_.FindDiagram(i), nullptr);
    EXPECT_NE(cache_.FindDiagram(i)->bitmap, nullptr);
    EXPECT_EQ(tracker_.recompute_layout, 1);
}

TEST_F(ResourceManagerTest, FlushPendingResourcesRequestsNothingWhenWidthIsZero)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    SetContentWidth(0.0f);
    rm_.FlushPendingResources();
    EXPECT_EQ(mock_mermaid_.request_render_count, 0);
}

// 再レイアウトの Flush の直後に SettleLayout が呼んでも、走査を繰り返さずタイマーに回す。
TEST_F(ResourceManagerTest, FlushPendingResourcesThrottlesFollowingBitmapManage)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    rm_.FlushPendingResources();
    EXPECT_EQ(mock_mermaid_.request_render_count, 1);

    rm_.ScheduleBitmapManage();
    EXPECT_EQ(mock_mermaid_.request_render_count, 1) << "直前の Flush があれば二重に走査しない";
    EXPECT_EQ(tracker_.last_set_timer_id, app_timer::Id::BITMAP_MANAGE);
}

// ---- ビットマップ管理 ----

TEST_F(ResourceManagerTest, EvictOffscreenBitmapsNoOpsWhenViewportHeightIsZero)
{
    // viewport 高さ 0 (初期化中) では保持範囲が潰れるため、遠方の図も含め何も解放しない。
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n\n```mermaid\ngraph TD;C-->D\n```\n",
                 /*block_height=*/10000.0f);
    const auto& indices = doc_.GetDiagramNodeIndices();
    ASSERT_GE(indices.size(), 2u);
    for (const size_t i : indices) {
        cache_.EnsureDiagram(i).png = std::make_shared<const ByteBuffer>(std::vector<uint8_t>(4, uint8_t{ 1 }));
    }
    SetWindowSize(state_, kContentWidth, 0.0f);
    viewport_.SetScrollY(0.0f);

    rm_.EvictOffscreenBitmaps();

    for (const size_t i : indices) {
        EXPECT_NE(cache_.FindDiagram(i)->png, nullptr) << "node " << i;
    }
}

// 保持範囲外のノードも text_layout を捨てるだけで、計測済み (layout_dirty=false) のまま残す。
// text_layout は実物の COM オブジェクトが要るので、layout_dirty の遷移だけを確かめる。
TEST_F(ResourceManagerTest, EvictOffscreenBitmapsKeepsEvictedNodesMeasured)
{
    std::string md;
    for (int i = 0; i < 80; i++) {
        md += "paragraph ";
        md += std::to_string(i);
        md += "\n\n";
    }
    LoadMarkdown(md, /*block_height=*/100.0f);
    viewport_.SetScrollY(0.0f);
    const float keep_bottom = kViewportHeight * (1.0f + decltype(rm_)::LAYOUT_EVICT_BUFFER_SCREENS);
    ASSERT_GT(cache_.Top(cache_.size() - 1), keep_bottom) << "前提: 末尾のノードが保持範囲外にあること";

    rm_.EvictOffscreenBitmaps();

    for (size_t i = 0; i < cache_.size(); i++) {
        EXPECT_FALSE(cache_[i].layout_dirty) << "node " << i;
    }
}

TEST_F(ResourceManagerTest, EvictOffscreenBitmapsReleasesDiagramPngButKeepsSize)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n\n```mermaid\ngraph TD;C-->D\n```\n",
                 /*block_height=*/10000.0f);
    const auto& indices = doc_.GetDiagramNodeIndices();
    ASSERT_GE(indices.size(), 2u);
    for (const size_t i : indices) {
        auto& d = cache_.EnsureDiagram(i);
        d.png = std::make_shared<const ByteBuffer>(std::vector<uint8_t>(4, uint8_t{ 1 }));
        d.width = 320.0f;
        d.height = 240.0f;
    }

    viewport_.SetScrollY(0.0f);
    rm_.EvictOffscreenBitmaps();

    const auto& near_diagram = *cache_.FindDiagram(indices[0]);
    const auto& far_diagram = *cache_.FindDiagram(indices[1]);
    EXPECT_NE(near_diagram.png, nullptr);
    EXPECT_EQ(far_diagram.png, nullptr);
    EXPECT_FLOAT_EQ(far_diagram.width, 320.0f);
    EXPECT_FLOAT_EQ(far_diagram.height, 240.0f);
}

TEST_F(ResourceManagerTest, ScheduleBitmapManageSetsTimer)
{
    LoadMarkdown("# heading\n");
    rm_.ScheduleBitmapManage();
    EXPECT_GE(tracker_.set_timer, 1);
    EXPECT_EQ(tracker_.last_set_timer_id, app_timer::Id::BITMAP_MANAGE);
    EXPECT_EQ(tracker_.last_set_timer_ms, 150u);
}

// スクロール停止のたびに全画面再描画しないよう、何も適用されなければ再レイアウト (と再描画) しない。
TEST_F(ResourceManagerTest, OnBitmapManageTimerSkipsRecomputeWhenNothingApplied)
{
    LoadMarkdown("# heading\n");
    rm_.OnBitmapManageTimer();
    EXPECT_GE(tracker_.kill_timer, 1);
    EXPECT_EQ(tracker_.recompute_layout, 0);
}

// 届いていた画像と同期で決まった図を 1 回の再レイアウトでまとめて反映する。
TEST_F(ResourceManagerTest, FlushPendingResourcesRecomputesOnceForImagesAndDiagrams)
{
    LoadMarkdown("![a](one.png)\n\n![b](two.png)\n\n```mermaid\ngraph TD;A-->B\n```\n");
    image_loader_.InsertCacheEntryForTest(L"C:\\dir\\one.png", 400.0f, 300.0f);
    image_loader_.InsertCacheEntryForTest(L"C:\\dir\\two.png", 400.0f, 250.0f);
    mock_mermaid_.sync_apply_bitmap = true;

    rm_.FlushPendingResources();

    EXPECT_EQ(tracker_.recompute_layout, 1);
    EXPECT_EQ(mock_mermaid_.request_render_count, 1);
}

// 取り込んだデコード結果が無ければ、キャッシュにある画像も適用し直さない。
TEST_F(ResourceManagerTest, OnImagesDecodedSkipsWhenNothingDecoded)
{
    LoadMarkdown("![a](one.png)\n");
    image_loader_.InsertCacheEntryForTest(L"C:\\dir\\one.png", 400.0f, 300.0f);

    rm_.OnImagesDecoded();

    EXPECT_EQ(tracker_.recompute_layout, 0);
    rm_.LoadImages();
    EXPECT_EQ(tracker_.recompute_layout, 1);
}

// ---- ファイル切替時クリーンアップ ----

// 解決済みパスは src をキーに持ち、文書フォルダが変わったら捨てる。同じ index が別画像を指しても、
// 別フォルダの同じ src を指しても旧パスでキャッシュを引かない。
TEST_F(ResourceManagerTest, ResolvedPathFollowsSrcAndDirectory)
{
    LoadMarkdown("![alt](old.png)\n");
    EXPECT_FALSE(rm_.ApplyCachedImagesForReload({ 0, SIZE_MAX }));

    LoadMarkdown("![alt](new.png)\n");
    image_loader_.InsertCacheEntryForTest(L"C:\\dir\\new.png", 400.0f, 300.0f);
    EXPECT_TRUE(rm_.ApplyCachedImagesForReload({ 0, SIZE_MAX }));

    image_loader_.InsertCacheEntryForTest(L"C:\\other\\same.png", 400.0f, 300.0f);
    LoadMarkdown("![alt](same.png)\n", 100.0f, L"C:\\dir\\doc.md");
    EXPECT_FALSE(rm_.ApplyCachedImagesForReload({ 0, SIZE_MAX }));
    LoadMarkdown("![alt](same.png)\n", 100.0f, L"C:\\other\\doc.md");
    EXPECT_TRUE(rm_.ApplyCachedImagesForReload({ 0, SIZE_MAX }));
}
