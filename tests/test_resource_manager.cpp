#include <gtest/gtest.h>
#include "resource_manager.h"
#include "app_constants.h"
#include "app_state.h"
#include "document.h"
#include "draw_command.h"
#include "file_io.h"
#include "inline_math.h"
#include "layout_cache.h"
#include "viewport_manager.h"
#include "image_loader.h"
#include "mermaid_file_cache.h"
#include "mermaid_renderer_interface.h"
#include "mermaid_util.h"
#include "reducer_harness.h"
#include "theme.h"
#include "test_helpers.h"
#include "task_scheduler.h"
#include "wic_util.h"
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

namespace {

// IMermaidRenderer のテスト用 mock。各メソッドの呼び出し回数と最後の引数を記録する。
class MockMermaidRenderer : public IMermaidRenderer {
public:
    std::vector<const Node*> requested;
    float last_max_width = 0.0f;
    bool last_dark_mode = false;
    // true のとき RequestRender でキャッシュヒットを模し、同期で描画命令を設定して true を返す。
    bool sync_apply_bitmap = false;
    // true のとき RequestRender でエラーの確定を模し、描画命令なしで true を返す。
    bool sync_error = false;

    bool RequestRender(
        const Node& node,
        DiagramEntry& diagram_entry, float max_width, bool dark_mode) override
    {
        requested.push_back(&node);
        last_max_width = max_width;
        last_dark_mode = dark_mode;
        if (sync_error) {
            diagram_entry.error = { DiagramError::Kind::Render, L"Parse error" };
            return true;
        }
        if (!sync_apply_bitmap) {
            return false;
        }
        diagram_entry.commands = &SharedStubCommandList();
        diagram_entry.width = 100.0f;
        diagram_entry.height = 50.0f;
        return true;
    }

    void DropQueued() override
    {
        drop_queued_count++;
    }
    int drop_queued_count = 0;
};

// ResourceManagerHost の各呼び出しを記録する。
struct TestResourceManagerHost final : ResourceManagerHost {
    int kill_timer = 0;
    int recompute_layout = 0;
    IndexRange last_change = {};
    std::optional<app_timer::Id> last_set_timer_id;
    bool loading_other = false;

    bool IsLoadingOtherDocument() const override
    {
        return loading_other;
    }
    void SetTimer(app_timer::Id id, UINT) override
    {
        last_set_timer_id = id;
    }
    void KillTimer(app_timer::Id) override
    {
        kill_timer++;
    }
    void RecomputeLayout(IndexRange changed) override
    {
        recompute_layout++;
        last_change = changed;
    }

    int inline_updates = 0;
    bool last_pending_resolved = false;
    bool last_sizes_changed = false;
    // App の組み直しの代わり。
    std::function<void(bool pending_resolved)> on_inline_update;
    void OnInlineImagesUpdated(bool pending_resolved, bool sizes_changed) override
    {
        inline_updates++;
        last_pending_resolved = pending_resolved;
        last_sizes_changed = sizes_changed;
        if (on_inline_update) {
            on_inline_update(pending_resolved);
        }
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

class ResourceManagerTest : public ComApartmentTest {
protected:
    // 画像の適用は doc_dir.empty() で早期 return するため、テスト用ドキュメントパスは
    // parent_path を持つ絶対パスでなければならない。
    static constexpr const wchar_t* kTestDocPath = L"C:\\dir\\test.md";
    static constexpr float kContentWidth = 800.0f;
    static constexpr float kViewportHeight = 600.0f;

    void SetUp() override
    {
        InitReducerState(state_, theme_);
        // Init しないので図のキャッシュは索引を引くだけで書かない。無いフォルダなら空の索引になるので、作らずに済む。
        // 既定の空パスのままだと作業フォルダの index.bin を読む。
        file_cache_.SetCacheDir(std::filesystem::temp_directory_path() / L"mendo_test_absent" / L"mermaid");
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

    // Markdown を Document に流し込み、対応する LayoutCache をシードする。
    void LoadMarkdown(std::string_view md, float block_height = 100.0f, const wchar_t* doc_path = kTestDocPath)
    {
        std::string utf8(md);
        doc_ = Document::FromMarkdown(std::move(utf8), doc_path);
        cache_ = SeedLayoutCache(doc_.GetNodes().size(), block_height);
        // 別の文書を開いた App と同じく、画像の表は空にする。
        rm_.ForgetInlineImages();
    }

    InlineImageEntry Entry(std::string_view src)
    {
        return inline_images_.Find(InlineImageKey(src));
    }

    bool HasBitmap(std::string_view src)
    {
        return inline_drawings_.FindImage(InlineImageKey(src)) != nullptr;
    }
    // 描く側に、読んだことにしたビットマップを持たせる。
    void GiveBitmaps(std::initializer_list<std::string_view> srcs)
    {
        for (const std::string_view src : srcs) {
            *inline_drawings_.images.TryEmplace(InlineImageKey(src)).first = &SharedStubBitmap();
        }
    }

    // 文書の最初の表を、どの行も高さ row_height で組んだことにする。表のノードの高さも合わせ、後ろのノードをずらす。
    void SeedTableLayout(float row_height)
    {
        const size_t i = doc_.GetTableNodeIndices().front();
        const NodeTableData& tbl = *doc_.GetNodes()[i].table_data();
        auto& tl = cache_[i].set_table_layout(std::make_unique<TableLayoutData>(tbl.row_count, tbl.col_count, false));
        std::ranges::fill(tl.row_heights, row_height);
        std::ranges::fill(tl.col_widths, 100.0f);
        tl.CommitGeometry(kContentWidth);
        cache_.ShiftTops(i + 1, tl.TotalHeight() - cache_[i].height);
        cache_[i].height = tl.TotalHeight();
    }

    Theme theme_{};
    AppState state_;
    MermaidFileCache file_cache_;
    Document& doc_ = state_.document.doc;
    LayoutCache& cache_ = state_.document.layout_cache;
    ViewportManager& viewport_ = state_.view.viewport;
    // Init しないので読み込みは起動しない。読み込んだ結果は InsertImageForTest で入れる (本物で読むのは ResourceManagerImageFileTest)。
    ImageLoader image_loader_;
    MockMermaidRenderer mock_mermaid_;
    TestResourceManagerHost host_;
    InlineImageTable inline_images_;
    InlineDrawings inline_drawings_;
    ResourceManager rm_{ state_, image_loader_, mock_mermaid_, file_cache_, inline_images_, inline_drawings_, host_ };
};

// ---- 画像経路 ----

TEST_F(ResourceManagerTest, LoadImagesAppliesNothingWhenNoImagesInDocument)
{
    LoadMarkdown("# heading\n\nparagraph text\n");
    rm_.LoadImages();
    EXPECT_EQ(host_.inline_updates, 0);
}

// 本文の幅が 0 (最小化など) のうちは、画像も図も適用・要求しない。
TEST_F(ResourceManagerTest, NothingAppliedWhenContentWidthIsZero)
{
    LoadMarkdown("# heading\n\n![alt](foo.png)\n\n```mermaid\ngraph TD;A-->B\n```\n");
    image_loader_.InsertImageForTest(L"C:\\dir\\foo.png", 1000.0f, 800.0f);
    SetContentWidth(0.0f);
    rm_.LoadImages();
    EXPECT_FALSE(rm_.ApplyCachedImageSizes({ 0, SIZE_MAX }));
    rm_.RequestMermaidRenders();
    EXPECT_EQ(host_.inline_updates, 0);
    EXPECT_EQ(host_.recompute_layout, 0);
    EXPECT_EQ(mock_mermaid_.requested.size(), 0u);
}

// 本物の ImageLoader で画像ファイルを読む。
class ResourceManagerImageFileTest : public ResourceManagerTest {
protected:
    void SetUp() override
    {
        ResourceManagerTest::SetUp();
        wic_ = wic_util::CreateWicFactory("ResourceManagerImageFileTest");
        ASSERT_TRUE(wic_);
        Microsoft::WRL::ComPtr<ID2D1Factory> d2d;
        ASSERT_HRESULT_SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()));
        Microsoft::WRL::ComPtr<IWICBitmap> target;
        ASSERT_HRESULT_SUCCEEDED(wic_->CreateBitmap(1, 1, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &target));
        ASSERT_HRESULT_SUCCEEDED(d2d->CreateWicBitmapRenderTarget(target.Get(), D2D1::RenderTargetProperties(), &rt_));
        image_loader_.Init(wic_.Get(), nullptr, 0, scheduler_);
        image_loader_.SetRenderTarget(rt_.Get());
    }
    void TearDown() override
    {
        scheduler_.Shutdown();
        image_loader_.ClearImagesForTest();
        inline_drawings_.images = {};
    }

    // 1x1 の PNG。
    void WritePng(const std::filesystem::path& path)
    {
        static constexpr unsigned char kPng[] = {
            0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
            0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF,
            0xC0, 0x00, 0x00, 0x03, 0x01, 0x01, 0x00, 0xC9, 0xFE, 0x92, 0xEF, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
        };
        ASSERT_TRUE(WriteAllBytes(path, kPng, sizeof(kPng), false));
    }

    // LoadImageTable の image 枚目の画像。
    static std::string Src(int image)
    {
        return std::format("i{}.png", image);
    }
    std::wstring ImagePath(int image) const
    {
        return (dir_.path() / std::format(L"i{}.png", image)).wstring();
    }

    // 1 行に 1 枚ずつ、別のファイルの画像を置いた表。
    void LoadImageTable(int rows)
    {
        std::string md = "| i |\n|---|\n";
        for (int i = 0; i < rows; ++i) {
            WritePng(ImagePath(i));
            md += std::format("| ![i]({}) |\n", Src(i));
        }
        LoadMarkdown(md, 100.0f, (dir_.path() / L"test.md").wstring().c_str());
    }

    // 読み込みが止まるまで、要求と取り込みを繰り返す。止まれば true。
    bool SettleLoads()
    {
        for (int rounds = 0; rounds < 10; ++rounds) {
            rm_.LoadImages();
            if (!DrainScheduler(scheduler_)) {
                return false;
            }
            if (!image_loader_.ProcessCompletedDecodes(kNoDeadline)) {
                return true;
            }
        }
        return false;
    }

    ScopedTempDir dir_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> rt_;
    TaskScheduler scheduler_{ 2 };
};

// 画像の多い表 (128 種類のキャッシュがあった頃は読み込みが互いを追い出して読み直し続けた) でも、描くビットマップを持つ画像は
// 読み直さず、読み込みが止まる。受け取ったビットマップは描く側へ渡したら ImageLoader に残さない。
TEST_F(ResourceManagerImageFileTest, ManyImagesInTableSettle)
{
    constexpr int kImages = 140;
    LoadImageTable(kImages);
    // 全部の行を先読み範囲 (y < 1800) に入れる。
    SeedTableLayout(10.0f);

    EXPECT_TRUE(SettleLoads()) << "読み込みが止まらない";
    for (int i = 0; i < kImages; ++i) {
        EXPECT_EQ(Entry(Src(i)).state, InlineImageState::Ready) << i;
        EXPECT_TRUE(HasBitmap(Src(i))) << i;
    }
    EXPECT_EQ(image_loader_.TakeDelivered().Find(ImagePath(0)), nullptr);
}

// 表は 1 ノードなので、先読み範囲の行の画像だけビットマップを読み、ほかの行の画像は寸法だけ読んで組めるようにする
// (表全体だと、スクリーンショット 40 枚の表で 1 行が範囲にかかるだけで全部を持った)。スクロールで範囲に入った行は読み、
// 外れた行のビットマップは捨てる。寸法は残るので組み直さない。
TEST_F(ResourceManagerImageFileTest, TableReadsBitmapsOnlyForRowsInRange)
{
    constexpr int kRows = 40;
    LoadImageTable(kRows);
    // 行の間隔は罫線込みで 251。先読み範囲は y < 1800 なので、ヘッダと画像の 7 行 (i0〜i6) が入る。
    SeedTableLayout(250.0f);

    ASSERT_TRUE(SettleLoads());
    for (int i = 0; i < kRows; ++i) {
        EXPECT_EQ(Entry(Src(i)).state, InlineImageState::Ready) << i;
        EXPECT_EQ(HasBitmap(Src(i)), i < 7) << i;
    }

    // 範囲は [3800, 6800)。ヘッダを除く 15〜27 行目 (i14〜i26) が入る。
    viewport_.SetScrollY(5000.0f);
    rm_.EvictOffscreenBitmaps();
    const int updates = host_.inline_updates;
    ASSERT_TRUE(SettleLoads());
    for (int i = 0; i < kRows; ++i) {
        EXPECT_EQ(HasBitmap(Src(i)), i >= 14 && i <= 26) << i;
    }
    EXPECT_GT(host_.inline_updates, updates);
    EXPECT_FALSE(host_.last_pending_resolved) << "寸法は変わらないので組み直さない";
    EXPECT_FALSE(host_.last_sizes_changed);
}

// 寸法が揃って組み直した表は、同じ取り込みの中で範囲の行の画像を頼む (BITMAP_MANAGE を待たない)。
TEST_F(ResourceManagerImageFileTest, TableSettledBySizesRequestsBitmapsRightAway)
{
    constexpr int kRows = 3;
    LoadImageTable(kRows);
    rm_.LoadImages();
    ASSERT_TRUE(DrainScheduler(scheduler_));
    ASSERT_TRUE(image_loader_.ProcessCompletedDecodes(kNoDeadline));

    host_.on_inline_update = [&](bool pending_resolved) {
        if (pending_resolved) {
            SeedTableLayout(100.0f);
        }
    };
    rm_.LoadImages();
    ASSERT_TRUE(host_.last_pending_resolved);
    ASSERT_TRUE(DrainScheduler(scheduler_));
    EXPECT_TRUE(image_loader_.ProcessCompletedDecodes(kNoDeadline)) << "ビットマップを頼んである";
    rm_.LoadImages();
    for (int i = 0; i < kRows; ++i) {
        EXPECT_TRUE(HasBitmap(Src(i))) << i;
    }
}

// 範囲を出た行の画像は、読み始める前なら要求を取り消す (速いスクロールで溜まった要求を読み続けない)。
TEST_F(ResourceManagerImageFileTest, RowsLeavingRangeCancelPendingBitmaps)
{
    LoadImageTable(40);
    // 行の間隔は罫線込みで 41。全部の行が先読み範囲 (y < 1800) に入る。
    SeedTableLayout(40.0f);
    // 保持範囲より短い移動なので、待ちをまとめて捨てない (DropQueuesIfJumped)。先読み範囲の上端が 150 になり、
    // i0 (41〜82) と i1 (82〜123) の行が外れる。
    constexpr float kScroll = kViewportHeight * (ResourceManager::PREFETCH_BUFFER_SCREENS + ResourceManager::EVICT_BUFFER_SCREENS) / 2.0f;
    static_assert(kScroll - kViewportHeight * ResourceManager::PREFETCH_BUFFER_SCREENS > 123.0f);
    {
        WorkerGate gate(scheduler_, 2);
        rm_.LoadImages();
        viewport_.SetScrollY(kScroll);
        rm_.LoadImages();
    }
    ASSERT_TRUE(DrainScheduler(scheduler_));
    ASSERT_TRUE(image_loader_.ProcessCompletedDecodes(kNoDeadline));
    const auto delivered = image_loader_.TakeDelivered();
    EXPECT_EQ(delivered.Find(ImagePath(0)), nullptr);
    EXPECT_EQ(delivered.Find(ImagePath(1)), nullptr);
    EXPECT_NE(delivered.Find(ImagePath(2)), nullptr);
}

// 組む前の表と、寸法を待って仮に組んだ表は、行の位置が決まらない (画像を幅 0 で組んで行が詰まっている) ので寸法だけ読む。
// 組み終えたら範囲の行の画像を読む。
TEST_F(ResourceManagerImageFileTest, UnsettledTableReadsOnlySizes)
{
    constexpr int kRows = 3;
    LoadImageTable(kRows);

    ASSERT_TRUE(SettleLoads());
    EXPECT_TRUE(host_.last_pending_resolved);
    const InlineImageEntry ready{
        InlineImageState::Ready, { 1.0f, 1.0f }
    };
    for (int i = 0; i < kRows; ++i) {
        EXPECT_EQ(Entry(Src(i)), ready) << i;
        EXPECT_FALSE(HasBitmap(Src(i))) << i;
    }

    SeedTableLayout(100.0f);
    cache_[0].pending_inline = true;
    ASSERT_TRUE(SettleLoads());
    EXPECT_FALSE(HasBitmap(Src(0)));

    cache_[0].pending_inline = false;
    ASSERT_TRUE(SettleLoads());
    for (int i = 0; i < kRows; ++i) {
        EXPECT_TRUE(HasBitmap(Src(i))) << i;
    }
}

// 別の文書を開いて戻っても、読んだ画像の寸法はパスで引けるので、組む前に入れられる。ビットマップは持たないので読み直す。
TEST_F(ResourceManagerImageFileTest, ReturningDocumentKeepsSizesAndReloadsBitmap)
{
    WritePng(dir_.path() / L"a.png");
    const std::wstring doc_path = (dir_.path() / L"test.md").wstring();
    LoadMarkdown("![a](a.png)\n", 100.0f, doc_path.c_str());
    rm_.LoadImages();
    ASSERT_TRUE(DrainScheduler(scheduler_));
    ASSERT_TRUE(image_loader_.ProcessCompletedDecodes(kNoDeadline));
    rm_.LoadImages();
    ASSERT_NE(inline_drawings_.FindImage(InlineImageKey("a.png")), nullptr);

    LoadMarkdown("other\n", 100.0f, (dir_.path() / L"other.md").wstring().c_str());
    LoadMarkdown("![a](a.png)\n", 100.0f, doc_path.c_str());
    EXPECT_TRUE(rm_.ApplyCachedImageSizes({ 0, SIZE_MAX }));
    EXPECT_EQ(Entry("a.png"), (InlineImageEntry{ InlineImageState::Ready, { 1.0f, 1.0f } }));
    EXPECT_EQ(inline_drawings_.FindImage(InlineImageKey("a.png")), nullptr);

    rm_.LoadImages();
    ASSERT_TRUE(DrainScheduler(scheduler_));
    EXPECT_TRUE(image_loader_.ProcessCompletedDecodes(kNoDeadline)) << "ビットマップを読み直す";
    const int updates = host_.inline_updates;
    rm_.LoadImages();
    EXPECT_NE(inline_drawings_.FindImage(InlineImageKey("a.png")), nullptr);
    EXPECT_EQ(host_.inline_updates, updates + 1);
    EXPECT_FALSE(host_.last_pending_resolved) << "寸法は変わらないので組み直さない";
    EXPECT_FALSE(host_.last_sizes_changed);
}

// ---- 画像経路: ApplyCachedImageSizes (issue #148) ----

// 組む前に読んだ画像の寸法を入れる。縮めるのは組むときなので、寸法は原寸 (DIP) のまま。ビットマップは LoadImages が渡す。
TEST_F(ResourceManagerTest, ApplyCachedImageSizesSetsSizeOnly)
{
    auto& bitmap = SharedStubBitmap();
    LoadMarkdown("# heading\n\n![alt](foo.png)\n");
    image_loader_.InsertImageForTest(L"C:\\dir\\foo.png", 1000.0f, 800.0f, &bitmap);

    EXPECT_TRUE(rm_.ApplyCachedImageSizes({ 0, SIZE_MAX }));
    EXPECT_EQ(Entry("foo.png"), (InlineImageEntry{ InlineImageState::Ready, { 1000.0f, 800.0f } }));
    EXPECT_EQ(inline_drawings_.FindImage(InlineImageKey("foo.png")), nullptr);
    EXPECT_EQ(host_.inline_updates, 0) << "組む前に呼ぶので組み直させない";

    rm_.LoadImages();
    EXPECT_EQ(inline_drawings_.FindImage(InlineImageKey("foo.png")), &bitmap);
}

// 寸法を待って仮に組んだノードを組み直させる。表が変わらなければ知らせない。
TEST_F(ResourceManagerTest, LoadImagesResolvesPendingImages)
{
    LoadMarkdown("# heading\n\ntext ![a](one.png) and ![b](two.png)\n");
    image_loader_.InsertImageForTest(L"C:\\dir\\one.png", 400.0f, 300.0f);

    rm_.LoadImages();
    EXPECT_EQ(host_.inline_updates, 1);
    EXPECT_TRUE(host_.last_pending_resolved);
    EXPECT_FALSE(host_.last_sizes_changed);
    EXPECT_EQ(Entry("one.png").state, InlineImageState::Ready);
    EXPECT_EQ(Entry("two.png").state, InlineImageState::Loading);

    rm_.LoadImages();
    EXPECT_EQ(host_.inline_updates, 1) << "変わらなければ知らせない";
}

// DPI が変わると同じビットマップでも DIP の寸法が変わる。組んだ寸法が変わるので、画像を持つノードを組み直させる。
TEST_F(ResourceManagerTest, LoadImagesUpdatesSizeOfSameBitmap)
{
    auto& bitmap = SharedStubBitmap();
    LoadMarkdown("![a](one.png)\n");
    image_loader_.InsertImageForTest(L"C:\\dir\\one.png", 400.0f, 300.0f, &bitmap);
    // 200% のときに入れた寸法。
    inline_images_.Exchange(InlineImageKey("one.png"), { InlineImageState::Ready, { 200.0f, 150.0f } });

    rm_.LoadImages();
    EXPECT_EQ(Entry("one.png").size, (InlineImageSize{ 400.0f, 300.0f }));
    EXPECT_EQ(host_.inline_updates, 1);
    EXPECT_TRUE(host_.last_sizes_changed);
    EXPECT_FALSE(host_.last_pending_resolved);
}

// 画像の src を寸法の表のパスへ解決する規則。引いてはいけない行のパスは、判定を外したときに解決されるパスで入れる。
TEST_F(ResourceManagerTest, ApplyCachedImageSizesResolvesSrc)
{
    const auto joined = [](const wchar_t* dir, const wchar_t* rel) {
        return (std::filesystem::path(dir) / rel).lexically_normal().wstring();
    };
    const std::wstring escaped = joined(L"\\\\?\\C:\\dir", L"../../UNC/attacker/share/a.png");
    ASSERT_FALSE(path_util::IsLocalPath(escaped)) << escaped;

    struct Entry {
        std::wstring key;
        float height;
    };
    struct Case {
        const char* label;
        std::string_view md;
        const wchar_t* doc_path;
        size_t images;
        std::vector<Entry> entries;
        // 最初の画像に入る高さ。nullopt なら寸法を入れない。
        std::optional<float> height;
        // src を解決しない。そのパスを読んだことがあっても引かず、読めない画像にする。
        bool unresolved = false;
    };
    const Case kCases[] = {
        { "Relative", "![alt](small.png)\n", kTestDocPath, 1, { { L"C:\\dir\\small.png", 300.0f } }, 300.0f },
        // src は UTF-8。path を char から直接構築すると ACP (CP932 等) 解釈で非 ASCII 名が化け、寸法を引けない。
        { "NonAscii", "![alt](\xE7\x94\xBB\xE5\x83\x8F.png)\n" /* 画像.png */, kTestDocPath, 1, { { L"C:\\dir\\\u753B\u50CF.png", 300.0f } }, 300.0f },
        // md4c は src をパーセントエンコードのまま渡すので、ファイルパスにする前にデコードする。
        { "PercentEncoded", "![alt](my%20images/%E7%94%BB%E5%83%8F.png)\n", kTestDocPath, 1,
          { { L"C:\\dir\\my images\\\u753B\u50CF.png", 300.0f } }, 300.0f },
        // UNC の画像は読んだことがあっても引かない。判定はデコード後のパスにかける。
        // Markdown の \\ は \ 1 個になるので、UNC の \\ は \\\\ と書く。
        { "Unc", "![a](\\\\\\\\attacker\\share\\a.png)\n\n![c](%5C%5Cattacker%5Cshare%5Cc.png)\n", kTestDocPath, 2,
          { { L"\\\\attacker\\share\\a.png", 300.0f }, { L"\\\\attacker\\share\\c.png", 300.0f } }, std::nullopt, true },
        // 先頭 2 文字がスラッシュなら向きを問わず UNC。デコード後に "//" になる記述も同じ。
        { "SlashUnc", "![b](//attacker/share/b.png)\n\n![c](%2F%2Fattacker%2Fshare%2Fc.png)\n\n![d](/%5Cattacker/share/d.png)\n", kTestDocPath, 3,
          { { L"\\\\attacker\\share\\b.png", 300.0f }, { L"\\\\attacker\\share\\c.png", 300.0f }, { L"\\\\attacker\\share\\d.png", 300.0f } }, std::nullopt, true },
        // デコードは 1 回だけ。二重エンコードの %255C は文書フォルダ内の "%5C" という名前のファイルになる。
        { "PercentOnlyOnce", "![e](%255C%255Cattacker%255Cshare%255Ce.png)\n", kTestDocPath, 1,
          { { L"\\\\attacker\\share\\e.png", 100.0f }, { L"C:\\dir\\%5C%5Cattacker%5Cshare%5Ce.png", 300.0f } }, 300.0f },
        // "://" を含む src はリモートとして解決しない。文書フォルダとの結合結果を読んだことがあっても引かない。
        { "Scheme", "![alt](https://example.com/foo.png)\n", kTestDocPath, 1,
          { { joined(L"C:\\dir", L"https://example.com/foo.png"), 300.0f } }, std::nullopt, true },
        // 文書を \\?\ 付きのパスで開いていても、相対パスの .. で \\?\UNC\host へ抜けさせない。
        { "DevicePrefixEscape", "![a](../../UNC/attacker/share/a.png)\n", L"\\\\?\\C:\\dir\\test.md", 1, { { escaped, 300.0f } }, std::nullopt },
        // \\?\ 付きの文書でも、文書フォルダの下の相対パスの画像は従来どおり表示する。
        { "DevicePrefixRelative", "![a](img/../a.png)\n", L"\\\\?\\C:\\dir\\test.md", 1, { { L"C:\\dir\\a.png", 300.0f } }, 300.0f },
        // 読んだことが無ければ何も入れず、読み込み中のまま。
        { "CacheMiss", "![alt](missing.png)\n", kTestDocPath, 1, {}, std::nullopt },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.label);
        image_loader_.ClearImagesForTest();
        LoadMarkdown(c.md, 100.0f, c.doc_path);
        const auto& image_indices = doc_.GetImageNodeIndices();
        ASSERT_EQ(image_indices.size(), c.images);
        for (const auto& e : c.entries) {
            image_loader_.InsertImageForTest(e.key, 400.0f, e.height);
        }
        // 読み込みの起動を避けるため、読んだ画像があることを確かめる経路 (ApplyCachedImageSizes) を通す。
        image_loader_.InsertImageForTest(L"C:\\unrelated\\x.png", 1.0f, 1.0f);
        const bool applied = rm_.ApplyCachedImageSizes({ 0, SIZE_MAX });
        const Node& first = doc_.GetNodes()[image_indices[0]];
        const auto entry = inline_images_.Find(InlineImageKey(first.image_src(first.runs()[0])));
        EXPECT_EQ(applied, c.unresolved || c.height.has_value());
        if (c.unresolved) {
            EXPECT_EQ(entry.state, InlineImageState::Unavailable);
            continue;
        }
        if (c.height) {
            EXPECT_EQ(entry, (InlineImageEntry{ InlineImageState::Ready, { 400.0f, *c.height } }));
        }
        else {
            EXPECT_EQ(entry.state, InlineImageState::Loading);
        }
    }
}

// 読み直しで失敗の記録が消えても、読めるまでは読めない画像のまま (幅 0 の読み込み中と行き来させない)。
// 読めたら寸法が変わるので、画像を持つノードを組み直させる。
TEST_F(ResourceManagerTest, UnavailableImageStaysUntilLoaded)
{
    LoadMarkdown("![alt](a.png)\n");
    inline_images_.Exchange(InlineImageKey("a.png"), { .state = InlineImageState::Unavailable });

    rm_.LoadImages();
    EXPECT_EQ(Entry("a.png").state, InlineImageState::Unavailable);
    EXPECT_EQ(host_.inline_updates, 0);

    image_loader_.InsertImageForTest(L"C:\\dir\\a.png", 400.0f, 300.0f);
    rm_.LoadImages();
    EXPECT_EQ(Entry("a.png"), (InlineImageEntry{ InlineImageState::Ready, { 400.0f, 300.0f } }));
    EXPECT_EQ(host_.inline_updates, 1);
    EXPECT_TRUE(host_.last_sizes_changed);
}

// 解決できない画像は読めない画像にし、寸法を待っていたノードを alt で組み直させる。
TEST_F(ResourceManagerTest, RemoteImageBecomesUnavailable)
{
    LoadMarkdown("![logo](https://example.com/a.png)\n");
    rm_.LoadImages();
    EXPECT_EQ(Entry("https://example.com/a.png").state, InlineImageState::Unavailable);
    EXPECT_EQ(host_.inline_updates, 1);
    EXPECT_TRUE(host_.last_pending_resolved);
}

TEST_F(ResourceManagerTest, ApplyCachedImageSizesAppliesBeyondVisibleRange)
{
    // issue #148 の本質: 可視範囲外の画像も全件走査して寸法を入れる。
    // 画像を 2 個配置し、2 個目を viewport から大きく外して LoadImages との差を出す。
    LoadMarkdown("![one](one.png)\n\n![two](two.png)\n", /*block_height=*/3000.0f);
    ASSERT_EQ(doc_.GetImageNodeIndices().size(), 2u);
    image_loader_.InsertImageForTest(L"C:\\dir\\one.png", 400.0f, 300.0f);
    image_loader_.InsertImageForTest(L"C:\\dir\\two.png", 400.0f, 250.0f);

    // 先読み範囲は可視 ± PREFETCH_BUFFER_SCREENS 画面。2 個目は y=3000 にあり範囲外。
    rm_.LoadImages();
    EXPECT_EQ(Entry("one.png").state, InlineImageState::Ready);
    EXPECT_EQ(Entry("two.png").state, InlineImageState::Loading);

    EXPECT_TRUE(rm_.ApplyCachedImageSizes({ 0, SIZE_MAX }));
    EXPECT_EQ(Entry("two.png").state, InlineImageState::Ready);
}

// リロードでは変わった範囲のノードにだけ当てる。
TEST_F(ResourceManagerTest, ApplyCachedImageSizesAppliesOnlyGivenRange)
{
    LoadMarkdown("![one](one.png)\n\n![two](two.png)\n", /*block_height=*/3000.0f);
    const auto& image_indices = doc_.GetImageNodeIndices();
    ASSERT_EQ(image_indices.size(), 2u);
    image_loader_.InsertImageForTest(L"C:\\dir\\one.png", 400.0f, 300.0f);
    image_loader_.InsertImageForTest(L"C:\\dir\\two.png", 400.0f, 250.0f);

    EXPECT_TRUE(rm_.ApplyCachedImageSizes({ image_indices[1], image_indices[1] + 1 }));
    EXPECT_EQ(Entry("one.png").state, InlineImageState::Loading);
    EXPECT_EQ(Entry("two.png").state, InlineImageState::Ready);
}

// ビットマップは保持範囲の画像の分だけ残す。寸法は残すので、戻ってきても組み直さない。
TEST_F(ResourceManagerTest, EvictOffscreenBitmapsDropsFarImageBitmaps)
{
    LoadMarkdown("![one](one.png)\n\n![two](two.png)\n", /*block_height=*/3000.0f);
    image_loader_.InsertImageForTest(L"C:\\dir\\one.png", 400.0f, 300.0f);
    image_loader_.InsertImageForTest(L"C:\\dir\\two.png", 400.0f, 250.0f);
    ASSERT_TRUE(rm_.ApplyCachedImageSizes({ 0, SIZE_MAX }));
    GiveBitmaps({ "one.png", "two.png" });

    rm_.EvictOffscreenBitmaps();
    EXPECT_TRUE(HasBitmap("one.png"));
    EXPECT_FALSE(HasBitmap("two.png"));
    EXPECT_EQ(Entry("two.png").state, InlineImageState::Ready);
}

// 先読み範囲を出ても、保持範囲の中の画像は捨てない。境目の画像を小刻みなスクロールのたびに読み直さない。
TEST_F(ResourceManagerTest, EvictOffscreenBitmapsKeepsBitmapsBeyondPrefetchRange)
{
    // two は先読み範囲の下端と保持範囲の下端の間から始まる。
    constexpr float kBlockHeight = kViewportHeight * (1.0f + (ResourceManager::PREFETCH_BUFFER_SCREENS + ResourceManager::EVICT_BUFFER_SCREENS) / 2.0f);
    LoadMarkdown("![one](one.png)\n\n![two](two.png)\n", kBlockHeight);
    GiveBitmaps({ "one.png", "two.png" });

    rm_.EvictOffscreenBitmaps();
    EXPECT_TRUE(HasBitmap("one.png"));
    EXPECT_TRUE(HasBitmap("two.png"));
}

// 表は保持範囲の行の画像の分だけ残す。行の位置が決まらない間は、読んだ行を見分けられないので全部残す。
TEST_F(ResourceManagerTest, EvictOffscreenBitmapsKeepsTableRowsInRange)
{
    LoadMarkdown("| i |\n|---|\n| ![a](a.png) |\n| ![b](b.png) |\n");
    // a の行は保持範囲の下端の 6 割、b の行は 12 割の位置から。
    constexpr float kKeepBottom = kViewportHeight * (1.0f + ResourceManager::EVICT_BUFFER_SCREENS);
    SeedTableLayout(kKeepBottom * 0.6f);
    GiveBitmaps({ "a.png", "b.png" });

    cache_[0].pending_inline = true;
    rm_.EvictOffscreenBitmaps();
    EXPECT_TRUE(HasBitmap("a.png"));
    EXPECT_TRUE(HasBitmap("b.png"));

    cache_[0].pending_inline = false;
    rm_.EvictOffscreenBitmaps();
    EXPECT_TRUE(HasBitmap("a.png"));
    EXPECT_FALSE(HasBitmap("b.png"));
}

// ---- Mermaid 経路 ----

TEST_F(ResourceManagerTest, RequestMermaidRendersInvokesRendererForVisibleDiagram)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    ASSERT_FALSE(doc_.GetDiagramNodeIndices().empty());

    rm_.RequestMermaidRenders();
    EXPECT_EQ(host_.recompute_layout, 0); // mock は描画命令をセットしない
    EXPECT_EQ(mock_mermaid_.requested.size(), 1u);
    EXPECT_FLOAT_EQ(mock_mermaid_.last_max_width, kContentWidth);
    EXPECT_FALSE(mock_mermaid_.last_dark_mode);

    // 保留だった図は、次の要求で結果が揃えば適用して組み直す。
    mock_mermaid_.sync_apply_bitmap = true;
    rm_.RequestMermaidRenders();
    const auto* diagram = cache_.FindDiagram(doc_.GetDiagramNodeIndices()[0]);
    ASSERT_NE(diagram, nullptr);
    EXPECT_TRUE(diagram->HasImage());
    EXPECT_EQ(host_.recompute_layout, 1);
}

TEST_F(ResourceManagerTest, RequestMermaidRendersSkipsBeyondVisibleRange)
{
    // diagram が 2 個。ノード間隔を先読み範囲 (可視 ± PREFETCH_BUFFER_SCREENS 画面) より広くし、
    // 1個目を可視範囲、2個目を範囲外に置く。
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n\n```mermaid\ngraph TD;C-->D\n```\n",
                 /*block_height=*/3000.0f);
    ASSERT_GE(doc_.GetDiagramNodeIndices().size(), 2u);

    rm_.RequestMermaidRenders();
    EXPECT_EQ(mock_mermaid_.requested.size(), 1u);
}

// エラー確定 (error 非空) の図は再リクエストされない (issue #271: 失敗レンダの無限リトライ防止)。
TEST_F(ResourceManagerTest, RequestMermaidRendersSkipsErroredDiagram)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    ASSERT_FALSE(doc_.GetDiagramNodeIndices().empty());
    cache_.EnsureDiagram(doc_.GetDiagramNodeIndices()[0]).error = { DiagramError::Kind::Render, L"Parse error on line 1" };

    rm_.RequestMermaidRenders();
    EXPECT_EQ(host_.recompute_layout, 0);
    EXPECT_EQ(mock_mermaid_.requested.size(), 0u);
}

// その場でエラーが確定した図はプレースホルダーの文言が変わるので、画像と同じ再レイアウトで描き直させる。
TEST_F(ResourceManagerTest, FlushPendingResourcesRedrawsSynchronousDiagramError)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    mock_mermaid_.sync_error = true;

    rm_.FlushPendingResources();

    EXPECT_EQ(mock_mermaid_.requested.size(), 1u);
    EXPECT_EQ(host_.recompute_layout, 1);
}

// リロード前の寸法の適用は Y を組み直さないので、変えた範囲を後の再レイアウトに持ち越さない。
TEST_F(ResourceManagerTest, ApplyCachedImageSizesDoesNotCarryChangeRange)
{
    LoadMarkdown("![one](one.png)\n\n```mermaid\ngraph TD;A-->B\n```\n");
    image_loader_.InsertImageForTest(L"C:\\dir\\one.png", 400.0f, 300.0f);
    ASSERT_TRUE(rm_.ApplyCachedImageSizes({ 0, SIZE_MAX }));

    mock_mermaid_.sync_apply_bitmap = true;
    rm_.RequestMermaidRenders();

    const size_t diagram = doc_.GetDiagramNodeIndices()[0];
    EXPECT_EQ(host_.recompute_layout, 1);
    EXPECT_EQ(host_.last_change.begin, diagram);
}

// 量子化幅が変わると旧幅の描画待ちを捨て、エラーもクリアして再試行する (issue #271)。
// メモリキャッシュは幅込みのキーなので残す。
TEST_F(ResourceManagerTest, DiagramWidthChangeClearsError)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    ASSERT_FALSE(doc_.GetDiagramNodeIndices().empty());
    const size_t diagram_index = doc_.GetDiagramNodeIndices()[0];

    SetContentWidth(800.0f);
    rm_.RequestMermaidRenders();
    ASSERT_EQ(mock_mermaid_.requested.size(), 1u);

    cache_.EnsureDiagram(diagram_index).error = { DiagramError::Kind::Render, L"Parse error on line 1" };
    const int initial_drop = mock_mermaid_.drop_queued_count;

    SetContentWidth(1000.0f);
    rm_.RequestMermaidRenders();
    EXPECT_GT(mock_mermaid_.drop_queued_count, initial_drop);
    EXPECT_TRUE(cache_.FindDiagram(diagram_index)->error.empty());
    EXPECT_EQ(mock_mermaid_.requested.size(), 2u);
}

// 幅の変化で描き直す図は、新しい結果が届くまで旧幅の結果を仮表示として残す (プレースホルダに戻して跳ねさせない)。
TEST_F(ResourceManagerTest, DiagramWidthChangeKeepsBitmapAsStale)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    const size_t diagram_index = doc_.GetDiagramNodeIndices()[0];
    mock_mermaid_.sync_apply_bitmap = true;
    SetContentWidth(800.0f);
    rm_.RequestMermaidRenders();
    auto& diagram = *cache_.FindDiagram(diagram_index);
    ASSERT_TRUE(diagram.commands);
    // 幅の上限より広い図 (幅に縛られる結果) なので、幅が変われば描き直しになる。
    diagram.width = 2000.0f;

    mock_mermaid_.sync_apply_bitmap = false;
    SetContentWidth(1000.0f);
    rm_.RequestMermaidRenders();

    EXPECT_TRUE(diagram.commands) << "描き直しの間も旧結果を表示する";
    EXPECT_TRUE(diagram.stale);
    EXPECT_TRUE(diagram.NeedsRender());
    EXPECT_EQ(mock_mermaid_.requested.size(), 2u);
}

// テーマが変わると、幅に収まって幅の変化では残す図も含めて全部を仮表示にし、旧テーマの描画待ちを捨てる。
// 仮表示の間は旧テーマの絵を保存・コピーさせない (コピーの SVG は新しいテーマで描くので食い違う)。
TEST_F(ResourceManagerTest, DiagramThemeChangeMarksAllStale)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    const size_t diagram_index = doc_.GetDiagramNodeIndices()[0];
    mock_mermaid_.sync_apply_bitmap = true;
    rm_.RequestMermaidRenders();
    auto& diagram = *cache_.FindDiagram(diagram_index);
    ASSERT_TRUE(diagram.commands);
    diagram.width = 100.0f;
    diagram.drawing = std::make_shared<const mermaid_util::DiagramDrawing>();
    ASSERT_TRUE(diagram.CanExport());
    const int initial_drop = mock_mermaid_.drop_queued_count;

    // 同じ条件なら描き直さない。
    rm_.RequestMermaidRenders();
    EXPECT_FALSE(diagram.stale);
    EXPECT_EQ(mock_mermaid_.drop_queued_count, initial_drop);

    mock_mermaid_.sync_apply_bitmap = false;
    theme_.dark = true;
    rm_.RequestMermaidRenders();
    EXPECT_TRUE(diagram.commands) << "描き直しの間も旧結果を表示する";
    EXPECT_TRUE(diagram.stale);
    EXPECT_FALSE(diagram.CanExport());
    EXPECT_GT(mock_mermaid_.drop_queued_count, initial_drop);
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
    EXPECT_EQ(mock_mermaid_.requested.size(), 3u);
    EXPECT_EQ(host_.recompute_layout, 1);
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

// 再レイアウトの Flush の直後に SettleLayout が呼んでも、走査を繰り返さずタイマーに回す。
TEST_F(ResourceManagerTest, ScheduleBitmapManageSetsTimer)
{
    LoadMarkdown("# heading\n");
    rm_.ScheduleBitmapManage();
    EXPECT_EQ(host_.last_set_timer_id, app_timer::Id::BITMAP_MANAGE);
}

TEST_F(ResourceManagerTest, FlushPendingResourcesThrottlesFollowingBitmapManage)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    rm_.FlushPendingResources();
    EXPECT_EQ(mock_mermaid_.requested.size(), 1u);

    rm_.ScheduleBitmapManage();
    EXPECT_EQ(mock_mermaid_.requested.size(), 1u) << "直前の Flush があれば二重に走査しない";
    EXPECT_EQ(host_.last_set_timer_id, app_timer::Id::BITMAP_MANAGE);
}

// ---- ビットマップ管理 ----

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

// スクロール停止のたびに全画面再描画しないよう、何も適用されなければ再レイアウト (と再描画) しない。
TEST_F(ResourceManagerTest, OnBitmapManageTimerSkipsRecomputeWhenNothingApplied)
{
    LoadMarkdown("# heading\n");
    rm_.OnBitmapManageTimer();
    EXPECT_GE(host_.kill_timer, 1);
    EXPECT_EQ(host_.recompute_layout, 0);
}

// 届いていた画像と同期で決まった図を 1 回の再レイアウトでまとめて反映する。
TEST_F(ResourceManagerTest, FlushPendingResourcesRecomputesOnceForImagesAndDiagrams)
{
    LoadMarkdown("![a](one.png)\n\n![b](two.png)\n\n```mermaid\ngraph TD;A-->B\n```\n");
    image_loader_.InsertImageForTest(L"C:\\dir\\one.png", 400.0f, 300.0f);
    image_loader_.InsertImageForTest(L"C:\\dir\\two.png", 400.0f, 250.0f);
    mock_mermaid_.sync_apply_bitmap = true;

    rm_.FlushPendingResources();

    EXPECT_EQ(host_.recompute_layout, 1);
    EXPECT_EQ(mock_mermaid_.requested.size(), 1u);
}

// 取り込んだデコード結果が無ければ、読んだことのある画像も適用し直さない。
TEST_F(ResourceManagerTest, OnImagesDecodedSkipsWhenNothingDecoded)
{
    LoadMarkdown("![a](one.png)\n");
    image_loader_.InsertImageForTest(L"C:\\dir\\one.png", 400.0f, 300.0f);

    rm_.OnImagesDecoded();

    EXPECT_EQ(host_.inline_updates, 0);
    rm_.LoadImages();
    EXPECT_EQ(host_.inline_updates, 1);
}

// ---- ファイル切替時クリーンアップ ----

// 解決済みパスは src をキーに持ち、文書フォルダが変わったら捨てる。同じ index が別画像を指しても、
// 別フォルダの同じ src を指しても旧パスの寸法を引かない。
TEST_F(ResourceManagerTest, ResolvedPathFollowsSrcAndDirectory)
{
    LoadMarkdown("![alt](old.png)\n");
    EXPECT_FALSE(rm_.ApplyCachedImageSizes({ 0, SIZE_MAX }));

    LoadMarkdown("![alt](new.png)\n");
    image_loader_.InsertImageForTest(L"C:\\dir\\new.png", 400.0f, 300.0f);
    EXPECT_TRUE(rm_.ApplyCachedImageSizes({ 0, SIZE_MAX }));

    image_loader_.InsertImageForTest(L"C:\\other\\same.png", 400.0f, 300.0f);
    LoadMarkdown("![alt](same.png)\n", 100.0f, L"C:\\dir\\doc.md");
    EXPECT_FALSE(rm_.ApplyCachedImageSizes({ 0, SIZE_MAX }));
    LoadMarkdown("![alt](same.png)\n", 100.0f, L"C:\\other\\doc.md");
    EXPECT_TRUE(rm_.ApplyCachedImageSizes({ 0, SIZE_MAX }));
}

// ---- 別の文書の読み込み中 ----

// 別の文書を読み込んでいる間は、結果の到着・スクロール・BITMAP_MANAGE のどれからも表示中の文書の図を頼まない。
// 始まった描画は取り消せず、開いた文書の最初の図を待たせる。差し替えずに終わったら頼み直す。
TEST_F(ResourceManagerTest, NoDiagramRequestsWhileLoadingOtherDocument)
{
    LoadMarkdown("```mermaid\ngraph TD;A-->B\n```\n");
    rm_.RequestMermaidRenders();
    ASSERT_EQ(mock_mermaid_.requested.size(), 1u);

    host_.loading_other = true;
    rm_.DropQueues();
    rm_.RequestMermaidRenders();
    rm_.FlushPendingResources();
    rm_.OnBitmapManageTimer();
    EXPECT_EQ(mock_mermaid_.requested.size(), 1u);

    host_.loading_other = false;
    rm_.OnBitmapManageTimer();
    EXPECT_EQ(mock_mermaid_.requested.size(), 2u);
}

// 画像も同じ。捨てた要求が読まずに戻っても (待つ間に頼み直してあると読み直させる)、読み込み中は頼み直さない。
TEST_F(ResourceManagerImageFileTest, NoImageRequestsWhileLoadingOtherDocument)
{
    WritePng(dir_.path() / L"a.png");
    WritePng(dir_.path() / L"b.png");
    LoadMarkdown("![a](a.png)\n\n![b](b.png)\n", 100.0f, (dir_.path() / L"test.md").wstring().c_str());
    {
        WorkerGate gate(scheduler_, 2);
        rm_.LoadImages();
        rm_.LoadImages();
        host_.loading_other = true;
        rm_.DropQueues();
    }
    ASSERT_TRUE(DrainScheduler(scheduler_));
    rm_.OnImagesDecoded(kNoDeadline);
    rm_.FlushPendingResources();
    rm_.OnBitmapManageTimer();
    ASSERT_TRUE(DrainScheduler(scheduler_));
    EXPECT_FALSE(image_loader_.ProcessCompletedDecodes(kNoDeadline));
    EXPECT_EQ(image_loader_.ImageInfoCount(), 0u) << "読み込み中に表示中の文書の画像を読んだ";

    host_.loading_other = false;
    ASSERT_TRUE(SettleLoads());
    EXPECT_TRUE(HasBitmap("a.png"));
    EXPECT_TRUE(HasBitmap("b.png"));
}

// 読み込みを始める前にデコードを終えていた画像は、読み込み中でも取り込んで描く。
TEST_F(ResourceManagerImageFileTest, LoadingOtherDocumentStillTakesInDecodedImages)
{
    WritePng(dir_.path() / L"a.png");
    LoadMarkdown("![a](a.png)\n", 100.0f, (dir_.path() / L"test.md").wstring().c_str());
    rm_.LoadImages();
    ASSERT_TRUE(DrainScheduler(scheduler_));

    host_.loading_other = true;
    rm_.DropQueues();
    rm_.OnImagesDecoded(kNoDeadline);
    EXPECT_TRUE(HasBitmap("a.png"));
}
