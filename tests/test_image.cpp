#include <gtest/gtest.h>
#include <filesystem>
#include "parser.h"
#include "layout.h"
#include "layout_cache.h"
#include "mock_text_measurer.h"
#include "file_io.h"
#include "image_loader.h"
#include "wic_util.h"
#include "task_scheduler.h"
#include "test_helpers.h"
#include "test_nodes.h"
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <shlwapi.h>
#include <atomic>
#include <deque>
#include <latch>

namespace {

// 共有なしで書き込み用に開ければ、ローダーがファイルを開いたままにしていない。
bool CanOpenExclusively(const std::wstring& path)
{
    const UniqueHandle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    return static_cast<bool>(file);
}

} // namespace


using Microsoft::WRL::ComPtr;

// ============================================================
// パーサーテスト: 画像ノードの生成
// ============================================================

TEST(ParserImage, SingleImageBecomesImageNode)
{
    struct Case {
        const char* md;
        const char* src;
        const char* alt;  // nullptr なら alt は確かめない
    };
    const Case cases[] = {
        {"![alt text](image.png)", "image.png", "alt text"},
        {"![photo](./images/photo.jpg)", "./images/photo.jpg", nullptr},
        {"![img](C:/Users/test/pic.png)", "C:/Users/test/pic.png", nullptr},
        {"![logo](https://example.com/logo.png)", "https://example.com/logo.png", nullptr},
        {"![](image.png)", "image.png", ""},
        {"![alt](image.png \"My Title\")", "image.png", nullptr},
        // 現在の実装では、画像を含む段落は全体が Image ノードに変換される
        {"before ![alt](img.png) after", "img.png", nullptr},
        // OnLeaveSpan で最後の image_src がノードに設定される
        {"![a](first.png) ![b](second.png)", "second.png", nullptr},
        {"> ![alt](img.png)", "img.png", nullptr},
        // CommonMark では画像説明内に画像記法を書ける。ノードの src は最外側を採用する
        {"![outer ![inner](inner.png)](outer.png)", "outer.png", nullptr},
        {"![alt](path%20with%20spaces/img.png)", "path%20with%20spaces/img.png", nullptr},
        // エスケープを含む src は md4c が一時バッファで組み立てて渡すので、原文ではなく arena へ写す。
        {"![alt](img\\_x.png)", "img_x.png", nullptr},
        // 参照形式の画像の src は原文の参照定義から取る。
        {"![alt][logo]\n\n[logo]: ref.png", "ref.png", nullptr},
        {"![日本語のaltテキスト](img.png)", "img.png", "日本語のaltテキスト"},
        {"![alt](画像/テスト.png)", "画像/テスト.png", nullptr},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.md);
        auto nodes = ParseNodes(c.md);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_EQ(nodes[0].type, NodeType::Image);
        EXPECT_EQ(nodes[0].image_src(), c.src);
        if (c.alt) {
            EXPECT_EQ(nodes[0].GetText(), c.alt);
        }
    }
}

// ---- ブロック要素内の画像 ----

TEST(ParserImage, ImageInTableCellDoesNotCorruptTable)
{
    // テーブルセル内の画像が variant の NodeTableData を破壊しないこと。
    // 破壊されると active_text_buffer がダングリングになり UAF / クラッシュする。
    auto nodes = ParseNodes("| ![badge](a.png) text | b |\n|---|---|\n| c | d |");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Table);
    ASSERT_NE(nodes[0].table_data(), nullptr);
    EXPECT_EQ(nodes[0].table_data()->row_count, 2u);
    EXPECT_TRUE(nodes[0].image_src().empty());
}

TEST(ParserImage, ImageInHeadingKeepsHeadingLevel)
{
    // 見出し内の画像バッジが見出しレベルを壊さないこと
    auto nodes = ParseNodes("# ![logo](l.png) Title");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Heading);
    EXPECT_EQ(nodes[0].heading_level, 1);
    EXPECT_TRUE(nodes[0].image_src().empty());
}

TEST(ParserImage, ImageInTightListStaysListItem)
{
    // タイトリスト内の画像はP生成されないため、ListItem のまま
    auto nodes = ParseNodes("- ![alt](img.png)");
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::ListItem);
    EXPECT_TRUE(nodes[0].image_src().empty());
}

// issue#298: リスト項目の番号が画像 src で上書きされないこと
TEST(ParserImage, ImageInTightOrderedListKeepsNumber)
{
    auto nodes = ParseNodes("3. ![alt](img.png)");
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::ListItem);
    EXPECT_TRUE(nodes[0].list_ordered());
    EXPECT_EQ(nodes[0].list_number(), 3);
}

TEST(ParserImage, ImageInFootnoteDefKeepsNumber)
{
    const auto result = ParseMarkdown("text[^1]\n\n[^1]: ![alt](img.png)");
    ASSERT_EQ(result.indices.footnote_def_indices.size(), 1u);
    const auto& def = result.nodes[result.indices.footnote_def_indices[0]];
    EXPECT_EQ(def.type, NodeType::ListItem);
    EXPECT_TRUE(def.list_ordered());
    EXPECT_EQ(def.list_number(), 1);
}

TEST(ParserImage, ImageInTightListDoesNotLeakToNextParagraph)
{
    // 画像を含むリスト項目の後の段落が Image に変換されないこと
    auto nodes = ParseNodes("- ![alt](img.png)\n\nNormal paragraph");
    bool found_para = false;
    for (const auto& node : nodes) {
        if (node.type == NodeType::Paragraph) {
            found_para = true;
            EXPECT_TRUE(node.image_src().empty())
                << "後続の段落にimage_srcがリークしていないこと";
        }
    }
    EXPECT_TRUE(found_para) << "後続の段落ノードが存在すること";
}

TEST(ParserImage, ImageInTaskListStaysTaskListItem)
{
    auto nodes = ParseNodes("- [x] ![alt](img.png)");
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::TaskListItem);
    EXPECT_TRUE(nodes[0].task_checked());
}

// ---- 画像がない場合 ----

TEST(ParserImage, ParagraphWithoutImageStaysParagraph)
{
    auto nodes = ParseNodes("Just text");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_TRUE(nodes[0].image_src().empty());
}

TEST(ParserImage, LinkDoesNotTriggerImage)
{
    auto nodes = ParseNodes("[link text](https://example.com)");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_TRUE(nodes[0].image_src().empty());
}

// ---- 複数ノード内の画像 ----

TEST(ParserImage, ImageBetweenParagraphs)
{
    auto nodes = ParseNodes("Before\n\n![alt](img.png)\n\nAfter");
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_EQ(nodes[1].type, NodeType::Image);
    EXPECT_EQ(nodes[2].type, NodeType::Paragraph);
}

TEST(ParserImage, MultipleImageParagraphs)
{
    auto nodes = ParseNodes("![a](a.png)\n\n![b](b.png)\n\n![c](c.png)");
    ASSERT_EQ(nodes.size(), 3u);
    for (size_t i = 0; i < 3; i++) {
        EXPECT_EQ(nodes[i].type, NodeType::Image) << "ノード " << i;
    }
    EXPECT_EQ(nodes[0].image_src(), "a.png");
    EXPECT_EQ(nodes[1].image_src(), "b.png");
    EXPECT_EQ(nodes[2].image_src(), "c.png");
}

// ============================================================
// レイアウトテスト: 画像ノードの高さ計算
// ============================================================

// 画像の読み込み (DiagramEntry への寸法設定) を模してからレイアウトするため、ParseAndLayout ではなく Layout を使う。
class ImageLayoutTest : public MockLayoutTestBase {
protected:
    static void SetImageSize(LayoutCache& cache, size_t i, float width, float height)
    {
        auto& d = cache.EnsureDiagram(i);
        d.width = width;
        d.height = height;
    }
    LayoutCache Layout(std::vector<Node>& nodes, float viewport_w = 800.0f)
    {
        LayoutCache cache(nodes.size());
        ComputeLayout(engine_, nodes, cache, viewport_w);
        return cache;
    }
    void Layout(std::vector<Node>& nodes, LayoutCache& cache, float viewport_w = 800.0f)
    {
        ComputeLayout(engine_, nodes, cache, viewport_w);
    }
};

TEST_F(ImageLayoutTest, ImagePlaceholderHeight)
{
    auto nodes = ParseNodes("![alt](img.png)");
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].type, NodeType::Image);

    auto cache = Layout(nodes);

    EXPECT_GT(cache[0].height, 0.0f) << "画像プレースホルダーの高さは正であるべき";
    EXPECT_FALSE(cache[0].layout_dirty);
}

TEST_F(ImageLayoutTest, ImageWithDimensionsUsesScaledHeight)
{
    auto nodes = ParseNodes("![alt](img.png)");
    ASSERT_EQ(nodes.size(), 1u);
    // 画像読み込み後のサイズをシミュレート
    LayoutCache cache(nodes.size());
    SetImageSize(cache, 0, 400.0f, 300.0f);
    Layout(nodes, cache);

    // 幅400 < コンテンツ幅のため、元の高さ300が使われる
    EXPECT_FLOAT_EQ(cache[0].height, 300.0f);
}

TEST_F(ImageLayoutTest, WideImageScaledDown)
{
    auto nodes = ParseNodes("![alt](img.png)");
    ASSERT_EQ(nodes.size(), 1u);
    // コンテンツ幅より大きい画像
    LayoutCache cache(nodes.size());
    SetImageSize(cache, 0, 1600.0f, 900.0f);
    Layout(nodes, cache);

    // スケールダウンされて元の高さより小さくなること
    EXPECT_LT(cache[0].height, 900.0f);
    EXPECT_GT(cache[0].height, 0.0f);
}

TEST_F(ImageLayoutTest, SmallImageNotScaledUp)
{
    auto nodes = ParseNodes("![alt](img.png)");
    ASSERT_EQ(nodes.size(), 1u);
    LayoutCache cache(nodes.size());
    SetImageSize(cache, 0, 100.0f, 80.0f);
    Layout(nodes, cache);

    // 小さい画像は拡大されない
    EXPECT_FLOAT_EQ(cache[0].height, 80.0f);
}

TEST_F(ImageLayoutTest, ImageHeightRecalculatedOnWidthChange)
{
    auto nodes = ParseNodes("![alt](img.png)");
    ASSERT_EQ(nodes.size(), 1u);
    LayoutCache cache(nodes.size());
    SetImageSize(cache, 0, 1600.0f, 900.0f);

    // 広い幅
    ComputeLayout(engine_, nodes, cache, 1800.0f);
    float h_wide = cache[0].height;

    // 狭い幅 → 再計算で高さが変わること
    ComputeLayout(engine_, nodes, cache, 400.0f);
    float h_narrow = cache[0].height;

    EXPECT_LT(h_narrow, h_wide);
}

TEST_F(ImageLayoutTest, ImageNodesDoNotOverlap)
{
    auto nodes = ParseNodes("![a](a.png)\n\n![b](b.png)\n\n![c](c.png)");
    LayoutCache cache(nodes.size());
    for (size_t i = 0; i < nodes.size(); i++) {
        SetImageSize(cache, i, 200.0f, 150.0f);
    }
    Layout(nodes, cache);

    for (size_t i = 1; i < nodes.size(); i++) {
        float prev_bottom = cache.Top(i - 1) + cache[i - 1].height;
        EXPECT_GE(cache.Top(i), prev_bottom) << "ノード " << i << " が前のノードと重なっている";
    }
}

TEST_F(ImageLayoutTest, ImageBetweenTextNodesDoNotOverlap)
{
    auto nodes = ParseNodes("Text before\n\n![alt](img.png)\n\nText after");
    ASSERT_EQ(nodes.size(), 3u);
    LayoutCache cache(nodes.size());
    SetImageSize(cache, 1, 400.0f, 300.0f);
    Layout(nodes, cache);

    for (size_t i = 1; i < nodes.size(); i++) {
        float prev_bottom = cache.Top(i - 1) + cache[i - 1].height;
        EXPECT_GE(cache.Top(i), prev_bottom);
    }
}

TEST_F(ImageLayoutTest, ImageWithZeroDimensionsGetsPlaceholder)
{
    auto nodes = ParseNodes("![alt](img.png)");
    ASSERT_EQ(nodes.size(), 1u);
    // 寸法 (DiagramEntry) が未設定 = 未読み込み
    auto cache = Layout(nodes);
    ASSERT_EQ(cache.FindDiagram(0), nullptr);

    EXPECT_GT(cache[0].height, 0.0f) << "プレースホルダー高さが設定されるべき";
}

TEST_F(ImageLayoutTest, ImageAspectRatioPreserved)
{
    auto nodes = ParseNodes("![alt](img.png)");
    LayoutCache cache(nodes.size());
    SetImageSize(cache, 0, 1000.0f, 500.0f);

    const float viewport_width = 600.0f;
    Layout(nodes, cache, viewport_width);

    const float content_width = theme_.ContentWidth(viewport_width);
    // コンテンツ幅 < 画像幅なのでスケールされる
    // アスペクト比 = 500/1000 = 0.5
    float expected_height = content_width * (500.0f / 1000.0f);
    EXPECT_NEAR(cache[0].height, expected_height, 0.1f);
}

// ============================================================
// ImageLoader テスト: WIC による画像読み込み
// ============================================================

class ImageLoaderTest : public ComApartmentTest {
protected:
    // loader_ や COM オブジェクトより後に破棄し、ファイル削除を最後に行う
    ScopedTempDir temp_dir_;
    ComPtr<ID2D1Factory> d2d_factory_;
    ComPtr<IWICImagingFactory> wic_factory_;
    ComPtr<ID2D1RenderTarget> render_target_;
    ImageLoader loader_;
    TaskScheduler scheduler_{ 2 };

    void SetUp() override
    {
        const HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_factory_.GetAddressOf());
        ASSERT_TRUE(SUCCEEDED(hr)) << "D2Dファクトリ作成に失敗";

        wic_factory_ = wic_util::CreateWicFactory("ImageLoaderTest");
        ASSERT_TRUE(wic_factory_) << "WICファクトリ作成に失敗";

        render_target_ = CreateRenderTarget();
        ASSERT_TRUE(render_target_) << "レンダーターゲット作成に失敗";

        // テストでは PostMessage を受け取れないため HWND は nullptr にし、ProcessCompletedDecodes を手動で呼ぶ
        loader_.Init(wic_factory_.Get(), nullptr, 0, scheduler_);
        loader_.SetRenderTarget(render_target_.Get());
    }

    void TearDown() override
    {
        scheduler_.Shutdown();
        loader_.ClearCache();
    }

    // worker (2 つ) を塞ぎ、release を count_down するまで以降のタスクを待たせる。
    void BlockWorkers(std::latch& release)
    {
        std::atomic<int> blocked{ 0 };
        for (int i = 0; i < 2; ++i) {
            scheduler_.Post([&] {
                ++blocked;
                release.wait();
            });
        }
        ASSERT_TRUE(PollUntil([&] { return blocked.load() == 2; }));
    }

    // 非同期経路でデコードさせ、完了後にキャッシュされた結果を取り出す。
    bool LoadAndGet(ImageLoader& loader, const std::wstring& path, DiagramEntry& out)
    {
        loader.RequestLoadAsync(path, 0.0f);
        if (!PollUntil([&] { return loader.ProcessCompletedDecodes(); })) {
            return false;
        }
        return GetCachedImage(loader, path, out);
    }
    bool LoadAndGet(const std::wstring& path, DiagramEntry& out)
    {
        return LoadAndGet(loader_, path, out);
    }

    // WIC ビットマップを描画先にするので HWND 不要。dpi 0 は D2D 既定 (96 DPI)。
    ComPtr<ID2D1RenderTarget> CreateRenderTarget(float dpi = 0.0f)
    {
        ComPtr<IWICBitmap> wic_bitmap;
        if (FAILED(wic_factory_->CreateBitmap(1, 1, GUID_WICPixelFormat32bppPBGRA,
                                              WICBitmapCacheOnLoad, &wic_bitmap))) {
            return nullptr;
        }

        auto props = D2D1::RenderTargetProperties();
        props.dpiX = dpi;
        props.dpiY = dpi;

        ComPtr<ID2D1RenderTarget> rt;
        if (FAILED(d2d_factory_->CreateWicBitmapRenderTarget(wic_bitmap.Get(), props, &rt))) {
            return nullptr;
        }
        return rt;
    }

    // WIC エンコーダで単色画像を temp_dir_ 配下に書き出す。
    bool CreateTestImage(std::wstring_view filename, const GUID& container_format,
                         UINT width, UINT height)
    {
        ComPtr<IStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(wic_factory_->CreateEncoder(container_format, nullptr, &encoder)) ||
            FAILED(SHCreateStreamOnFileW(GetTestImagePath(filename).c_str(), STGM_CREATE | STGM_WRITE, &stream)) ||
            FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
            FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
            FAILED(frame->Initialize(nullptr)) ||
            FAILED(frame->SetSize(width, height)) ||
            FAILED(frame->SetPixelFormat(&format))) {
            return false;
        }

        // 不透明な赤 (BGRA) の 1 行を使い回し、大画像でもメモリ使用量を抑える
        const UINT stride = width * 4;
        std::vector<BYTE> row(stride);
        for (UINT x = 0; x < width; ++x) {
            row[x * 4 + 2] = 255;
            row[x * 4 + 3] = 255;
        }
        for (UINT y = 0; y < height; ++y) {
            if (FAILED(frame->WritePixels(1, stride, stride, row.data()))) {
                return false;
            }
        }
        return SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }

    std::wstring GetTestImagePath(std::wstring_view filename) const
    {
        return (temp_dir_.path() / filename).wstring();
    }
};

// 拡張子ではなく先頭バイトで形式を決め、ブラウザで表示できる形式以外のデコーダは動かさない。
// 内容から自動判定する CreateDecoderFromStream に戻すと、インストール済みの任意のコーデックが
// 文書を開いただけでプロセス内で動く。
TEST_F(ImageLoaderTest, DoesNotDecodeFormatsOutsideAllowList)
{
    ASSERT_TRUE(CreateTestImage(L"tiff_as.png", GUID_ContainerFormatTiff, 40, 30));
    const auto path = GetTestImagePath(L"tiff_as.png");
    ComPtr<IWICBitmapDecoder> system_decoder;
    ASSERT_TRUE(SUCCEEDED(wic_factory_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                                  WICDecodeMetadataCacheOnDemand, &system_decoder)))
        << "WIC 自体は TIFF を読めること";

    DiagramEntry entry;
    EXPECT_FALSE(LoadAndGet(path, entry));
    EXPECT_EQ(loader_.FindCachedImage(path), nullptr);
}

// 呼び出し側が指定した形式のデコーダだけを使い、別形式のバイト列は読まない (Mermaid の PNG 等)。
TEST_F(ImageLoaderTest, DecodeFromMemoryUsesOnlyRequestedContainer)
{
    ASSERT_TRUE(CreateTestImage(L"mem.bmp", GUID_ContainerFormatBmp, 50, 40));
    const auto bytes = ReadAllBytes(GetTestImagePath(L"mem.bmp").c_str());
    ASSERT_FALSE(bytes.empty());

    EXPECT_FALSE(wic_util::DecodeFromMemory(wic_factory_.Get(), bytes, GUID_ContainerFormatPng).has_value());
    const auto bmp = wic_util::DecodeFromMemory(wic_factory_.Get(), bytes, GUID_ContainerFormatBmp);
    ASSERT_TRUE(bmp.has_value());
    EXPECT_EQ(bmp->pixel_width, 50u);
    EXPECT_EQ(bmp->pixel_height, 40u);
}

// ---- 正常系: 画像フォーマット ----

TEST_F(ImageLoaderTest, LoadsEachFormatAtPixelSize)
{
    struct Case {
        std::wstring_view filename;
        const GUID& container;
        UINT width;
        UINT height;
    };
    const Case kCases[] = {
        { L"test.png", GUID_ContainerFormatPng, 100, 80 },
        { L"test.bmp", GUID_ContainerFormatBmp, 50, 50 },
        { L"test.jpg", GUID_ContainerFormatJpeg, 200, 150 },
        { L"large.png", GUID_ContainerFormatPng, 4096, 2048 },
        { L"tiny.png", GUID_ContainerFormatPng, 1, 1 },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ c.filename }));
        ASSERT_TRUE(CreateTestImage(c.filename, c.container, c.width, c.height));
        DiagramEntry entry;
        EXPECT_TRUE(LoadAndGet(GetTestImagePath(c.filename), entry));
        EXPECT_TRUE(entry.bitmap);
        EXPECT_FLOAT_EQ(entry.width, static_cast<float>(c.width));
        EXPECT_FLOAT_EQ(entry.height, static_cast<float>(c.height));
    }
}

// ---- 正常系: キャッシュ動作 ----

TEST_F(ImageLoaderTest, CacheHitReturnsSameBitmap)
{
    ASSERT_TRUE(CreateTestImage(L"cached.png", GUID_ContainerFormatPng, 64, 64));
    auto path = GetTestImagePath(L"cached.png");

    DiagramEntry entry1;
    EXPECT_TRUE(LoadAndGet(path, entry1));

    DiagramEntry entry2;
    EXPECT_TRUE(GetCachedImage(loader_, path, entry2));

    // 同じビットマップオブジェクトが返されること
    EXPECT_EQ(entry1.bitmap.Get(), entry2.bitmap.Get());
}

TEST_F(ImageLoaderTest, DifferentPathsDifferentBitmaps)
{
    ASSERT_TRUE(CreateTestImage(L"img1.png", GUID_ContainerFormatPng, 32, 32));
    ASSERT_TRUE(CreateTestImage(L"img2.png", GUID_ContainerFormatPng, 64, 64));

    DiagramEntry entry1, entry2;
    EXPECT_TRUE(LoadAndGet(GetTestImagePath(L"img1.png"), entry1));
    EXPECT_TRUE(LoadAndGet(GetTestImagePath(L"img2.png"), entry2));

    EXPECT_NE(entry1.bitmap.Get(), entry2.bitmap.Get());
    EXPECT_FLOAT_EQ(entry1.width, 32.0f);
    EXPECT_FLOAT_EQ(entry2.width, 64.0f);
}

TEST_F(ImageLoaderTest, ClearCacheInvalidatesEntries)
{
    ASSERT_TRUE(CreateTestImage(L"clear.png", GUID_ContainerFormatPng, 40, 40));
    auto path = GetTestImagePath(L"clear.png");

    DiagramEntry entry1;
    EXPECT_TRUE(LoadAndGet(path, entry1));
    auto* first_bitmap = entry1.bitmap.Get();

    loader_.ClearCache();

    DiagramEntry entry2;
    EXPECT_TRUE(LoadAndGet(path, entry2));

    // キャッシュクリア後は新しいビットマップが作成される
    EXPECT_NE(first_bitmap, entry2.bitmap.Get());
}

// 始まっていないデコードを捨てると読まずに返る。待つ間に要求し直されていれば、届いたときに読み直させる。失敗には数えない。
TEST_F(ImageLoaderTest, DropQueuedAllowsRerequest)
{
    ASSERT_TRUE(CreateTestImage(L"drop.png", GUID_ContainerFormatPng, 40, 40));
    const auto path = GetTestImagePath(L"drop.png");

    for (int attempt = 0; attempt < 4; ++attempt) {
        // 要求をキュー待ちのまま捨てる。
        std::latch release{ 1 };
        BlockWorkers(release);
        loader_.RequestLoadAsync(path, 0.0f);
        loader_.DropQueued();
        loader_.RequestLoadAsync(path, 0.0f);
        release.count_down();
        ASSERT_TRUE(PollUntil([&] { return loader_.ProcessCompletedDecodes(); }));
        EXPECT_EQ(loader_.CacheSize(), 0u);
    }

    DiagramEntry entry;
    EXPECT_TRUE(LoadAndGet(path, entry));
}

// 文書の切り替えで捨てた要求の結果は、要求し直されていなければ読み直させない。
TEST_F(ImageLoaderTest, DropQueuedWithoutRerequestDoesNotAskToRequestAgain)
{
    ASSERT_TRUE(CreateTestImage(L"drop.png", GUID_ContainerFormatPng, 40, 40));

    std::latch release{ 1 };
    BlockWorkers(release);
    loader_.RequestLoadAsync(GetTestImagePath(L"drop.png"), 0.0f);
    loader_.DropQueued();
    release.count_down();

    // Shutdown はキュー済みタスクを実行し切ってから戻るので、捨てた要求の結果が届いている。
    scheduler_.Shutdown();
    EXPECT_FALSE(loader_.ProcessCompletedDecodes());
    EXPECT_EQ(loader_.CacheSize(), 0u);
}

// ---- 異常系: ファイルが存在しない ----

TEST_F(ImageLoaderTest, NonExistentFileReturnsFalse)
{
    DiagramEntry entry;
    EXPECT_FALSE(LoadAndGet(L"C:\\nonexistent\\path\\image.png", entry));
    EXPECT_FALSE(entry.bitmap);
}

TEST_F(ImageLoaderTest, EmptyPathReturnsFalse)
{
    DiagramEntry entry;
    EXPECT_FALSE(LoadAndGet(L"", entry));
    EXPECT_FALSE(entry.bitmap);
}

// ---- 異常系: 壊れた画像ファイル・非画像ファイル ----

TEST_F(ImageLoaderTest, UndecodableFileReturnsFalse)
{
    struct Case {
        std::wstring_view filename;
        std::string_view content;
    };
    constexpr Case kCases[] = {
        // PNG ヘッダの途中で切れたデータ
        { L"corrupt.png", std::string_view("\x89PNG\r\n\x1a\n\x00\x00", 10) },
        { L"empty.png", {} },
        { L"random.png", "This is not an image file at all!" },
        // JPEG SOI マーカーのみの不完全データ
        { L"truncated.jpg", std::string_view("\xFF\xD8\xFF\xE0\x00\x10", 6) },
        { L"readme.txt", "Hello, World!" },
        { L"page.html", "<html><body>test</body></html>" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ c.filename }));
        const auto path = temp_dir_.Write(c.filename, c.content);
        DiagramEntry entry;
        EXPECT_FALSE(LoadAndGet(path.wstring(), entry));
        EXPECT_FALSE(entry.bitmap);
    }
}

// ---- 異常系: 未初期化の状態 ----

TEST_F(ImageLoaderTest, UninitializedLoaderReturnsFalse)
{
    ImageLoader uninitialized;
    EXPECT_FALSE(uninitialized.Init(nullptr, nullptr, 0, scheduler_));
    ASSERT_TRUE(CreateTestImage(L"valid.png", GUID_ContainerFormatPng, 10, 10));

    DiagramEntry entry;
    EXPECT_FALSE(LoadAndGet(uninitialized, GetTestImagePath(L"valid.png"), entry));
    EXPECT_FALSE(entry.bitmap);
}

TEST_F(ImageLoaderTest, NullRenderTargetReturnsFalse)
{
    ImageLoader loader_null;
    loader_null.Init(wic_factory_.Get(), nullptr, 0, scheduler_);
    ASSERT_TRUE(CreateTestImage(L"valid.png", GUID_ContainerFormatPng, 10, 10));

    DiagramEntry entry;
    EXPECT_FALSE(LoadAndGet(loader_null, GetTestImagePath(L"valid.png"), entry));
}

// ---- 異常系: エントリの状態保証 ----

TEST_F(ImageLoaderTest, FailedLoadDoesNotModifyExistingEntry)
{
    DiagramEntry entry;
    entry.width = 999.0f;
    entry.height = 888.0f;

    // 存在しないファイルのロードを試みる
    EXPECT_FALSE(LoadAndGet(L"C:\\no_such_file.png", entry));

    // 既存の値が変更されていないこと
    EXPECT_FLOAT_EQ(entry.width, 999.0f);
    EXPECT_FLOAT_EQ(entry.height, 888.0f);
    EXPECT_FALSE(entry.bitmap);
}

// ---- GetCachedImage テスト ----

TEST_F(ImageLoaderTest, GetCachedImageReturnsFalseWhenNotCached)
{
    DiagramEntry entry;
    EXPECT_FALSE(GetCachedImage(loader_, L"C:\\not_cached.png", entry));
}

TEST_F(ImageLoaderTest, GetCachedImageReturnsFalseAfterClearCache)
{
    ASSERT_TRUE(CreateTestImage(L"clear_test.png", GUID_ContainerFormatPng, 30, 30));
    auto path = GetTestImagePath(L"clear_test.png");

    DiagramEntry entry;
    ASSERT_TRUE(LoadAndGet(path, entry));
    loader_.ClearCache();

    EXPECT_FALSE(GetCachedImage(loader_, path, entry));
}

// ---- Shutdown テスト ----

TEST_F(ImageLoaderTest, ShutdownWithoutInitIsNoOp)
{
    ImageLoader loader;
    loader.SetRenderTarget(render_target_.Get());
    loader.Shutdown(); // Init 未呼び出しでもクラッシュしないこと
}

TEST_F(ImageLoaderTest, CancelPendingIsNoOp)
{
    // CancelPending が空の状態でもクラッシュしないこと
    loader_.CancelPending();
}

// ---- ファイルロック回避テスト ----

TEST_F(ImageLoaderTest, FileCanBeDeletedAfterLoad)
{
    ASSERT_TRUE(CreateTestImage(L"deletable.png", GUID_ContainerFormatPng, 32, 32));
    auto path = GetTestImagePath(L"deletable.png");

    DiagramEntry entry;
    ASSERT_TRUE(LoadAndGet(path, entry));

    // 読み込み後にファイルを削除できること（ロックされていない証拠）
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::remove(path, ec))
        << "画像読み込み後にファイルを削除できなかった: " << ec.message();
}

TEST_F(ImageLoaderTest, FileNotLockedAfterFailedLoad)
{
    // 壊れた画像でも読み込み後にファイルがロックされないこと
    const auto path = temp_dir_.Write(L"bad_lock.png", std::string_view("\x89PNG\r\n\x1a\n\x00\x00", 10));

    DiagramEntry entry;
    LoadAndGet(path.wstring(), entry);
    EXPECT_TRUE(CanOpenExclusively(path.wstring())) << "失敗した読み込み後にファイルがロックされている";
}

// ---- 縮小デコード ----

class WicDecodeTest : public ImageLoaderTest {
protected:
    // 戻り値はファイルのバイト列を参照するので、フィクスチャの寿命まで保持する。
    std::optional<wic_util::DecodeResult> Decode(std::wstring_view filename)
    {
        const auto& bytes = files_.emplace_back(ReadAllBytes(GetTestImagePath(filename).c_str()));
        return wic_util::DecodeImageFileFromMemory(wic_factory_.Get(), bytes);
    }

    std::deque<ByteBuffer> files_;

    static void ExpectPbgraOfSize(IWICBitmap* bitmap, UINT width, UINT height)
    {
        ASSERT_TRUE(bitmap);
        UINT w = 0;
        UINT h = 0;
        ASSERT_TRUE(SUCCEEDED(bitmap->GetSize(&w, &h)));
        EXPECT_EQ(w, width);
        EXPECT_EQ(h, height);
        WICPixelFormatGUID format{};
        ASSERT_TRUE(SUCCEEDED(bitmap->GetPixelFormat(&format)));
        EXPECT_TRUE(format == GUID_WICPixelFormat32bppPBGRA);
    }
};

TEST_F(WicDecodeTest, JpegScalesFromOpaqueFrame)
{
    ASSERT_TRUE(CreateTestImage(L"scale.jpg", GUID_ContainerFormatJpeg, 400, 300));
    const auto decoded = Decode(L"scale.jpg");
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(decoded->opaque_frame);
    ExpectPbgraOfSize(wic_util::DecodeToWicBitmap(wic_factory_.Get(), *decoded, { 100, 75 }).Get(), 100, 75);
}

TEST_F(WicDecodeTest, AlphaImageScalesAfterConversion)
{
    ASSERT_TRUE(CreateTestImage(L"scale.png", GUID_ContainerFormatPng, 400, 300));
    const auto decoded = Decode(L"scale.png");
    ASSERT_TRUE(decoded);
    EXPECT_FALSE(decoded->opaque_frame);
    ExpectPbgraOfSize(wic_util::DecodeToWicBitmap(wic_factory_.Get(), *decoded, { 100, 75 }).Get(), 100, 75);
}

TEST_F(WicDecodeTest, OriginalSizeKeepsPixels)
{
    ASSERT_TRUE(CreateTestImage(L"keep.jpg", GUID_ContainerFormatJpeg, 64, 32));
    const auto decoded = Decode(L"keep.jpg");
    ASSERT_TRUE(decoded);
    ExpectPbgraOfSize(wic_util::DecodeToWicBitmap(wic_factory_.Get(), *decoded, { 64, 32 }).Get(), 64, 32);
}

// ============================================================
// DPI スケーリングテスト: 画像サイズが DIP 単位で返されること
// ============================================================

// 基底の loader_ (既定 DPI) は使わず、CreateRenderTarget(dpi) から DPI 別のローダーを作る。
// 読み込み直後もキャッシュから引き直しても、同じ DIP サイズになること。
TEST_F(ImageLoaderTest, SizeIsPixelsDividedByDpiScale)
{
    struct Case {
        float dpi;
        UINT px_width;
        UINT px_height;
        float dip_width;
        float dip_height;
    };
    constexpr Case kCases[] = {
        { 96.0f, 200, 100, 200.0f, 100.0f },
        { 120.0f, 250, 100, 200.0f, 80.0f },  // 125%
        { 144.0f, 300, 150, 200.0f, 100.0f }, // 150%
        { 192.0f, 400, 200, 200.0f, 100.0f }, // 200%
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.dpi);
        const auto rt = CreateRenderTarget(c.dpi);
        ASSERT_TRUE(rt);
        ImageLoader loader;
        loader.Init(wic_factory_.Get(), nullptr, 0, scheduler_);
        loader.SetRenderTarget(rt.Get());

        const auto filename = L"dpi" + std::to_wstring(static_cast<int>(c.dpi)) + L".png";
        ASSERT_TRUE(CreateTestImage(filename, GUID_ContainerFormatPng, c.px_width, c.px_height));
        const auto path = GetTestImagePath(filename);
        // 96 / 192 DPI は倍率が 2 の冪なので丸め誤差なく一致する。
        const float tolerance = (c.dpi == 96.0f || c.dpi == 192.0f) ? 0.0f : 0.1f;
        DiagramEntry loaded;
        ASSERT_TRUE(LoadAndGet(loader, path, loaded));
        EXPECT_NEAR(loaded.width, c.dip_width, tolerance);
        EXPECT_NEAR(loaded.height, c.dip_height, tolerance);

        DiagramEntry cached;
        ASSERT_TRUE(GetCachedImage(loader, path, cached));
        EXPECT_NEAR(cached.width, c.dip_width, tolerance);
        EXPECT_NEAR(cached.height, c.dip_height, tolerance);
    }
}

// ============================================================
// 非同期読み込みテスト
// ============================================================

class ImageLoaderAsyncTest : public ImageLoaderTest {
protected:
    // 結果を取り込んだ ProcessCompletedDecodes の回数。
    int processed_count_ = 0;

    void Process(std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kUploadSliceBudget)
    {
        if (loader_.ProcessCompletedDecodes(deadline)) {
            ++processed_count_;
        }
    }

    // 失敗結果も取り込んだと数えるので、失敗の完了待ちにも使える。
    bool WaitForResults(int expected_count)
    {
        return PollUntil([&] {
            Process();
            return processed_count_ >= expected_count;
        });
    }

    // 1 回の取り込みで複数件入りうるので、複数リクエストの完了はキャッシュ件数で待つ。
    bool WaitForCached(size_t count)
    {
        return PollUntil([&] {
            Process();
            return loader_.CacheSize() >= count;
        });
    }

    // path の非同期ロードを 1 件ずつ完了させながら times 回失敗させる。
    bool FailLoads(const std::wstring& path, int times)
    {
        for (int i = 0; i < times; ++i) {
            const int before = processed_count_;
            loader_.RequestLoadAsync(path, 0.0f);
            if (!WaitForResults(before + 1)) {
                return false;
            }
        }
        return true;
    }
};

TEST_F(ImageLoaderAsyncTest, AsyncLoadPopulatesCache)
{
    ASSERT_TRUE(CreateTestImage(L"async.png", GUID_ContainerFormatPng, 120, 90));
    auto path = GetTestImagePath(L"async.png");

    DiagramEntry entry;
    ASSERT_TRUE(LoadAndGet(path, entry)) << "非同期読み込みが完了しなかった";
    EXPECT_TRUE(entry.bitmap);
    EXPECT_FLOAT_EQ(entry.width, 120.0f);
    EXPECT_FLOAT_EQ(entry.height, 90.0f);
}

TEST_F(ImageLoaderAsyncTest, DuplicateRequestIsIgnored)
{
    ASSERT_TRUE(CreateTestImage(L"dup.png", GUID_ContainerFormatPng, 50, 50));
    auto path = GetTestImagePath(L"dup.png");

    // 同じパスを2回リクエスト — 重複は無視される
    loader_.RequestLoadAsync(path, 0.0f);
    loader_.RequestLoadAsync(path, 0.0f);

    ASSERT_TRUE(WaitForResults(1));

    // 結果は 1 件だけ届く
    EXPECT_EQ(processed_count_, 1);
    scheduler_.Shutdown();
    EXPECT_FALSE(loader_.ProcessCompletedDecodes());
}

TEST_F(ImageLoaderAsyncTest, MultiplePathsAllCached)
{
    ASSERT_TRUE(CreateTestImage(L"a.png", GUID_ContainerFormatPng, 10, 10));
    ASSERT_TRUE(CreateTestImage(L"b.png", GUID_ContainerFormatPng, 20, 20));
    auto path_a = GetTestImagePath(L"a.png");
    auto path_b = GetTestImagePath(L"b.png");

    loader_.RequestLoadAsync(path_a, 0.0f);
    loader_.RequestLoadAsync(path_b, 0.0f);

    ASSERT_TRUE(WaitForCached(2));
    EXPECT_GE(processed_count_, 1);

    DiagramEntry entry_a, entry_b;
    EXPECT_TRUE(GetCachedImage(loader_, path_a, entry_a));
    EXPECT_TRUE(GetCachedImage(loader_, path_b, entry_b));
    EXPECT_FLOAT_EQ(entry_a.width, 10.0f);
    EXPECT_FLOAT_EQ(entry_b.width, 20.0f);
}

TEST_F(ImageLoaderAsyncTest, FileNotLockedAfterAsyncLoad)
{
    ASSERT_TRUE(CreateTestImage(L"async_lock.png", GUID_ContainerFormatPng, 80, 60));
    auto path = GetTestImagePath(L"async_lock.png");

    DiagramEntry entry;
    ASSERT_TRUE(LoadAndGet(path, entry)) << "非同期読み込みが完了しなかった";
    EXPECT_TRUE(CanOpenExclusively(path)) << "非同期読み込み後にファイルが書き込みロックされている";
}

TEST_F(ImageLoaderAsyncTest, AsyncLoadReturnsDipSizeAt150Percent)
{
    // 非同期パスでも DPI 補正が適用されることを確認
    // 既存ローダーを停止し、144 DPI のレンダーターゲットで再初期化
    loader_.Shutdown();
    loader_.ClearCache();

    const auto rt_144 = CreateRenderTarget(144.0f);
    ASSERT_TRUE(rt_144);

    loader_.SetRenderTarget(rt_144.Get());

    ASSERT_TRUE(CreateTestImage(L"async_dpi.png", GUID_ContainerFormatPng, 300, 150));
    auto path = GetTestImagePath(L"async_dpi.png");

    DiagramEntry entry;
    ASSERT_TRUE(LoadAndGet(path, entry)) << "非同期読み込みが完了しなかった";
    EXPECT_TRUE(entry.bitmap);
    // 300px / 1.5 = 200 DIP, 150px / 1.5 = 100 DIP
    EXPECT_NEAR(entry.width, 200.0f, 0.1f);
    EXPECT_NEAR(entry.height, 100.0f, 0.1f);
}

TEST_F(ImageLoaderAsyncTest, CancelPendingClearsQueue)
{
    ASSERT_TRUE(CreateTestImage(L"cancel.png", GUID_ContainerFormatPng, 30, 30));
    auto path = GetTestImagePath(L"cancel.png");

    // 要求をキュー待ちのまま取り消す。
    std::latch release{ 1 };
    BlockWorkers(release);
    loader_.RequestLoadAsync(path, 0.0f);
    loader_.CancelPending();
    release.count_down();

    // Shutdown はキュー済みタスクを実行し切ってから戻るので、以降に結果が届くことはない
    scheduler_.Shutdown();
    EXPECT_FALSE(loader_.ProcessCompletedDecodes());
    EXPECT_EQ(loader_.CacheSize(), 0u);
}

// ---- スレッドプールテスト ----

TEST_F(ImageLoaderAsyncTest, ManyImagesAllCachedCorrectly)
{
    // ワーカー数を超える画像を同時にリクエストし、全て正しくキャッシュされること
    constexpr int kImageCount = 8;
    std::vector<std::wstring> paths;
    for (int i = 0; i < kImageCount; ++i) {
        auto name = L"pool_" + std::to_wstring(i) + L".png";
        UINT w = static_cast<UINT>(10 + i * 5);
        UINT h = static_cast<UINT>(20 + i * 3);
        ASSERT_TRUE(CreateTestImage(name, GUID_ContainerFormatPng, w, h));
        paths.emplace_back(GetTestImagePath(name));
    }

    for (auto& p : paths) {
        loader_.RequestLoadAsync(p, 0.0f);
    }

    ASSERT_TRUE(WaitForCached(paths.size())) << "非同期読み込みが完了しなかった";
    EXPECT_GE(processed_count_, 1);

    for (int i = 0; i < kImageCount; ++i) {
        DiagramEntry entry;
        EXPECT_TRUE(GetCachedImage(loader_, paths[i], entry))
            << "画像 " << i << " がキャッシュされていない";
        EXPECT_FLOAT_EQ(entry.width, static_cast<float>(10 + i * 5));
        EXPECT_FLOAT_EQ(entry.height, static_cast<float>(20 + i * 3));
    }
}

// まとめて届いた結果は時間予算を超えた分を次の呼び出しに回す。予算が無くても 1 件は進める。
TEST_F(ImageLoaderAsyncTest, ProcessCompletedDecodesDefersOverBudget)
{
    constexpr int kImageCount = 3;
    for (int i = 0; i < kImageCount; ++i) {
        const auto name = L"budget_" + std::to_wstring(i) + L".png";
        ASSERT_TRUE(CreateTestImage(name, GUID_ContainerFormatPng, 16, 16));
        loader_.RequestLoadAsync(GetTestImagePath(name), 0.0f);
    }
    // Shutdown はキュー済みタスクを実行し切ってから戻るので、全件の結果が届いている。
    scheduler_.Shutdown();

    EXPECT_FALSE(loader_.HasLeftoverUploads()) << "届いただけの結果は投函で知らせる";
    EXPECT_TRUE(loader_.ProcessCompletedDecodes(std::chrono::steady_clock::time_point::min()));
    EXPECT_EQ(loader_.CacheSize(), 1u);
    EXPECT_TRUE(loader_.HasLeftoverUploads());

    loader_.ProcessCompletedDecodes(std::chrono::steady_clock::time_point::max());
    EXPECT_EQ(loader_.CacheSize(), static_cast<size_t>(kImageCount));
    EXPECT_FALSE(loader_.HasLeftoverUploads());
}

TEST_F(ImageLoaderAsyncTest, ShutdownDuringHeavyLoad)
{
    // 大量のリクエスト中に Shutdown してもクラッシュしないこと
    constexpr int kImageCount = 10;
    for (int i = 0; i < kImageCount; ++i) {
        auto name = L"heavy_" + std::to_wstring(i) + L".png";
        ASSERT_TRUE(CreateTestImage(name, GUID_ContainerFormatPng, 100, 100));
        loader_.RequestLoadAsync(GetTestImagePath(name), 0.0f);
    }

    // 処理途中で即座にシャットダウン — デッドロックやクラッシュが起きないこと
    loader_.Shutdown();
}

TEST_F(ImageLoaderAsyncTest, LoadAfterShutdownWithHeavyLoad)
{
    // Shutdown 後も同じローダーで読み込めること
    constexpr int kImageCount = 6;
    for (int i = 0; i < kImageCount; ++i) {
        auto name = L"reinit_a_" + std::to_wstring(i) + L".png";
        ASSERT_TRUE(CreateTestImage(name, GUID_ContainerFormatPng, 40, 40));
        loader_.RequestLoadAsync(GetTestImagePath(name), 0.0f);
    }

    loader_.Shutdown();
    loader_.CancelPending();
    loader_.ClearCache();

    auto name = L"reinit_b.png";
    ASSERT_TRUE(CreateTestImage(name, GUID_ContainerFormatPng, 77, 55));
    auto path = GetTestImagePath(name);

    DiagramEntry entry;
    ASSERT_TRUE(LoadAndGet(path, entry)) << "Shutdown 後の非同期読み込みが完了しなかった";
    EXPECT_FLOAT_EQ(entry.width, 77.0f);
    EXPECT_FLOAT_EQ(entry.height, 55.0f);
}

TEST_F(ImageLoaderAsyncTest, CancelDuringHeavyLoadThenReload)
{
    // 大量リクエスト中にキャンセルし、その後に新たなリクエストが処理されること
    constexpr int kImageCount = 8;
    for (int i = 0; i < kImageCount; ++i) {
        auto name = L"cancel_heavy_" + std::to_wstring(i) + L".png";
        ASSERT_TRUE(CreateTestImage(name, GUID_ContainerFormatPng, 50, 50));
        loader_.RequestLoadAsync(GetTestImagePath(name), 0.0f);
    }

    loader_.CancelPending();

    // キャンセル後に新しいリクエストが正常に処理されること
    ASSERT_TRUE(CreateTestImage(L"after_cancel.png", GUID_ContainerFormatPng, 88, 66));
    auto path = GetTestImagePath(L"after_cancel.png");

    loader_.RequestLoadAsync(path, 0.0f);

    // キャンセル直後は進行中だったワーカーの結果が先に返る可能性があるため、
    // 取り込んだ回数ではなく対象画像がキャッシュされるまで直接ポーリングする
    DiagramEntry entry;
    ASSERT_TRUE(PollUntil([&] {
        loader_.ProcessCompletedDecodes();
        return GetCachedImage(loader_, path, entry);
    })) << "キャンセル後のリクエストが完了しなかった";
    EXPECT_FLOAT_EQ(entry.width, 88.0f);
    EXPECT_FLOAT_EQ(entry.height, 66.0f);
}

// ---- failed_paths_ ブラックリストテスト ----

TEST_F(ImageLoaderAsyncTest, NonexistentPathBlockedAfterMaxRetries)
{
    const auto path = GetTestImagePath(L"does_not_exist.png");

    // kMaxImageRetries (3) 回失敗するまではリトライが許可される
    ASSERT_TRUE(FailLoads(path, 3));

    // 4回目のリクエストはブロックされ、ワーカーが起動しないこと。
    // Shutdown はキュー済みタスクを実行し切ってから戻るので、投入されていれば結果が残る。
    loader_.RequestLoadAsync(path, 0.0f);
    scheduler_.Shutdown();
    EXPECT_FALSE(loader_.ProcessCompletedDecodes());

    DiagramEntry entry;
    EXPECT_FALSE(GetCachedImage(loader_, path, entry));
}

TEST_F(ImageLoaderAsyncTest, CancelPendingClearsFailedPaths)
{
    const auto path = GetTestImagePath(L"fail_then_clear.png");

    // 3回失敗させてブラックリスト化
    ASSERT_TRUE(FailLoads(path, 3));

    // CancelPending でクリア
    loader_.CancelPending();

    // 実在するファイルを同じパスに作成 → リトライが成功すること
    ASSERT_TRUE(CreateTestImage(L"fail_then_clear.png", GUID_ContainerFormatPng, 40, 30));

    DiagramEntry entry;
    ASSERT_TRUE(LoadAndGet(path, entry)) << "CancelPending 後のリトライが完了しなかった";
    EXPECT_FLOAT_EQ(entry.width, 40.0f);
    EXPECT_FLOAT_EQ(entry.height, 30.0f);
}

TEST_F(ImageLoaderAsyncTest, ClearCacheAlsoClearsFailedPaths)
{
    const auto path = GetTestImagePath(L"fail_then_clearcache.png");

    // 3回失敗させてブラックリスト化
    ASSERT_TRUE(FailLoads(path, 3));

    // ClearCache でもクリアされること
    loader_.ClearCache();

    ASSERT_TRUE(CreateTestImage(L"fail_then_clearcache.png", GUID_ContainerFormatPng, 55, 45));

    DiagramEntry entry;
    ASSERT_TRUE(LoadAndGet(path, entry)) << "ClearCache 後のリトライが完了しなかった";
    EXPECT_FLOAT_EQ(entry.width, 55.0f);
    EXPECT_FLOAT_EQ(entry.height, 45.0f);
}

TEST_F(ImageLoaderAsyncTest, TransientFailureRetries)
{
    const auto path = GetTestImagePath(L"transient.png");

    // 1回目: ファイルが存在しないので失敗
    ASSERT_TRUE(FailLoads(path, 1));

    // ファイルを作成（一時障害から復帰を模擬）
    ASSERT_TRUE(CreateTestImage(L"transient.png", GUID_ContainerFormatPng, 70, 50));

    // 2回目: リトライが許可され、今度は成功すること
    DiagramEntry entry;
    ASSERT_TRUE(LoadAndGet(path, entry)) << "一時障害後のリトライが成功しなかった";
    EXPECT_FLOAT_EQ(entry.width, 70.0f);
    EXPECT_FLOAT_EQ(entry.height, 50.0f);
}

TEST_F(ImageLoaderAsyncTest, ResetFailedPathsAllowsRetry)
{
    const auto path = GetTestImagePath(L"reset_failed.png");

    // 3回失敗させてブラックリスト化
    ASSERT_TRUE(FailLoads(path, 3));

    // ResetFailedPaths でクリア
    loader_.ResetFailedPaths();

    ASSERT_TRUE(CreateTestImage(L"reset_failed.png", GUID_ContainerFormatPng, 33, 22));

    DiagramEntry entry;
    ASSERT_TRUE(LoadAndGet(path, entry)) << "ResetFailedPaths 後のリトライが完了しなかった";
    EXPECT_FLOAT_EQ(entry.width, 33.0f);
    EXPECT_FLOAT_EQ(entry.height, 22.0f);
}

TEST_F(ImageLoaderAsyncTest, DecodesUpToQuantizedDisplayWidth)
{
    ASSERT_TRUE(CreateTestImage(L"wide.png", GUID_ContainerFormatPng, 1000, 500));
    const auto path = GetTestImagePath(L"wide.png");

    // 300 DIP (96 DPI で 300px) は 256px 単位に切り上げて 512px でデコードする。
    loader_.RequestLoadAsync(path, 300.0f);
    ASSERT_TRUE(WaitForResults(1));
    const auto* cached = loader_.FindCachedImage(path);
    ASSERT_NE(cached, nullptr);
    ASSERT_TRUE(cached->bitmap);
    EXPECT_EQ(cached->bitmap->GetPixelSize().width, 512u);
    // レイアウトは原寸基準のまま。
    EXPECT_FLOAT_EQ(cached->pixel_width, 1000.0f);
    EXPECT_FLOAT_EQ(cached->pixel_height, 500.0f);
    EXPECT_EQ(cached->sharp_width_px, 512u);
    EXPECT_FALSE(loader_.NeedsSharperDecode(*cached, 500.0f));
    EXPECT_TRUE(loader_.NeedsSharperDecode(*cached, 600.0f));

    // 表示幅が広がったら、高い解像度で読み直してキャッシュを差し替える。
    loader_.RequestLoadAsync(path, 600.0f);
    ASSERT_TRUE(WaitForResults(2));
    cached = loader_.FindCachedImage(path);
    ASSERT_NE(cached, nullptr);
    EXPECT_EQ(cached->bitmap->GetPixelSize().width, 768u);
    EXPECT_EQ(cached->sharp_width_px, 768u);
}

TEST_F(ImageLoaderAsyncTest, ImageNarrowerThanDisplayIsNotRedecoded)
{
    ASSERT_TRUE(CreateTestImage(L"narrow.png", GUID_ContainerFormatPng, 100, 50));
    const auto path = GetTestImagePath(L"narrow.png");

    loader_.RequestLoadAsync(path, 300.0f);
    ASSERT_TRUE(WaitForResults(1));
    const auto* cached = loader_.FindCachedImage(path);
    ASSERT_NE(cached, nullptr);
    EXPECT_EQ(cached->bitmap->GetPixelSize().width, 100u);
    EXPECT_FALSE(loader_.NeedsSharperDecode(*cached, 5000.0f));
}

// ============================================================
// デコードサイズ計算
// ============================================================

TEST(ComputeDecodeSize, KeepsSizeWithinLimits)
{
    EXPECT_EQ(wic_util::ComputeDecodeSize(800, 600, 1920, 16384), (wic_util::PixelSize{ 800, 600 }));
}

TEST(ComputeDecodeSize, ShrinksToMaxWidthKeepingAspect)
{
    EXPECT_EQ(wic_util::ComputeDecodeSize(6000, 4000, 1920, 16384), (wic_util::PixelSize{ 1920, 1280 }));
}

TEST(ComputeDecodeSize, ShrinksTallImageToMaxDimension)
{
    EXPECT_EQ(wic_util::ComputeDecodeSize(1000, 40000, 1920, 16384), (wic_util::PixelSize{ 410, 16384 }));
}

TEST(ComputeDecodeSize, ZeroLimitsMeanUnlimited)
{
    EXPECT_EQ(wic_util::ComputeDecodeSize(6000, 4000, 0, 0), (wic_util::PixelSize{ 6000, 4000 }));
}

TEST(ComputeDecodeSize, NeverProducesZeroDimension)
{
    EXPECT_EQ(wic_util::ComputeDecodeSize(100000, 1, 1000, 0), (wic_util::PixelSize{ 1000, 1 }));
}

TEST_F(ImageLoaderAsyncTest, AsyncLoadedBitmapIsUploadedFromDecodedPixels)
{
    ASSERT_TRUE(CreateTestImage(L"decoded.png", GUID_ContainerFormatPng, 64, 32));
    auto path = GetTestImagePath(L"decoded.png");

    DiagramEntry entry;
    ASSERT_TRUE(LoadAndGet(path, entry));
    ASSERT_TRUE(entry.bitmap);
    const auto px = entry.bitmap->GetPixelSize();
    EXPECT_EQ(px.width, 64u);
    EXPECT_EQ(px.height, 32u);
}

static const GUID* Sniff(std::initializer_list<uint8_t> head)
{
    const std::vector<uint8_t> bytes(head);
    return wic_util::SniffImageContainer(bytes);
}

TEST(SniffImageContainer, AcceptsBrowserImageFormats)
{
    EXPECT_EQ(Sniff({ 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0 }), &GUID_ContainerFormatPng);
    EXPECT_EQ(Sniff({ 0xFF, 0xD8, 0xFF, 0xE0 }), &GUID_ContainerFormatJpeg);
    EXPECT_EQ(Sniff({ 'G', 'I', 'F', '8', '9', 'a' }), &GUID_ContainerFormatGif);
    EXPECT_EQ(Sniff({ 'B', 'M', 0, 0 }), &GUID_ContainerFormatBmp);
    EXPECT_EQ(Sniff({ 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P' }), &GUID_ContainerFormatWebp);
    EXPECT_EQ(Sniff({ 0x00, 0x00, 0x01, 0x00, 1, 0 }), &GUID_ContainerFormatIco);
}

// HEIF・TIFF・JPEG XR・DDS や、途中で切れた署名は弾く。
TEST(SniffImageContainer, RejectsOtherFormats)
{
    EXPECT_EQ(Sniff({}), nullptr);
    EXPECT_EQ(Sniff({ 0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'h', 'e', 'i', 'c' }), nullptr);
    EXPECT_EQ(Sniff({ 'I', 'I', 0x2A, 0 }), nullptr);
    EXPECT_EQ(Sniff({ 'I', 'I', 0xBC, 0x01 }), nullptr);
    EXPECT_EQ(Sniff({ 'D', 'D', 'S', ' ' }), nullptr);
    EXPECT_EQ(Sniff({ 0x89, 'P', 'N', 'G' }), nullptr);
    EXPECT_EQ(Sniff({ 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E' }), nullptr);
}
