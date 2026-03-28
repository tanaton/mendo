#include <gtest/gtest.h>
#include <memory_resource>
#include <filesystem>
#include <fstream>
#include <chrono>
#include "parser.h"
#include "layout.h"
#include "layout_cache.h"
#include "mock_text_measurer.h"
#include "image_loader.h"
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <shlwapi.h>

using Microsoft::WRL::ComPtr;

// ============================================================
// パーサーテスト: 画像ノードの生成
// ============================================================

TEST(ParserImage, ImageOnlyParagraphBecomesImageNode) {
    auto nodes = ParseMarkdown("![alt text](image.png)");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Image);
    EXPECT_EQ(nodes[0].image_src, L"image.png");
    EXPECT_EQ(nodes[0].text, L"alt text");
}

TEST(ParserImage, ImageWithRelativePath) {
    auto nodes = ParseMarkdown("![photo](./images/photo.jpg)");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Image);
    EXPECT_EQ(nodes[0].image_src, L"./images/photo.jpg");
}

TEST(ParserImage, ImageWithAbsolutePath) {
    auto nodes = ParseMarkdown("![img](C:/Users/test/pic.png)");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Image);
    EXPECT_EQ(nodes[0].image_src, L"C:/Users/test/pic.png");
}

TEST(ParserImage, ImageWithHttpUrl) {
    auto nodes = ParseMarkdown("![logo](https://example.com/logo.png)");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Image);
    EXPECT_EQ(nodes[0].image_src, L"https://example.com/logo.png");
}

TEST(ParserImage, ImageWithEmptyAlt) {
    auto nodes = ParseMarkdown("![](image.png)");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Image);
    EXPECT_EQ(nodes[0].image_src, L"image.png");
    EXPECT_TRUE(nodes[0].text.empty());
}

TEST(ParserImage, ImageWithTitle) {
    auto nodes = ParseMarkdown("![alt](image.png \"My Title\")");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Image);
    EXPECT_EQ(nodes[0].image_src, L"image.png");
}

TEST(ParserImage, ImageAltTextPreserved) {
    auto nodes = ParseMarkdown("![Hello World](pic.png)");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].text, L"Hello World");
}

TEST(ParserImage, ImageDefaultDimensionsZero) {
    auto nodes = ParseMarkdown("![alt](img.png)");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_FLOAT_EQ(nodes[0].image_width, 0.0f);
    EXPECT_FLOAT_EQ(nodes[0].image_height, 0.0f);
}

// ---- 画像が段落に混在するケース ----

TEST(ParserImage, ImageWithSurroundingTextStillConverts) {
    // 現在の実装では、画像を含む段落は全体がImageノードに変換される
    auto nodes = ParseMarkdown("before ![alt](img.png) after");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Image);
    EXPECT_EQ(nodes[0].image_src, L"img.png");
}

TEST(ParserImage, MultipleImagesLastOneWins) {
    auto nodes = ParseMarkdown("![a](first.png) ![b](second.png)");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Image);
    // OnLeaveSpanで最後のimage_srcがノードに設定される
    EXPECT_EQ(nodes[0].image_src, L"second.png");
}

// ---- ブロック要素内の画像 ----

TEST(ParserImage, ImageInBlockquoteBecomesImageNode) {
    auto nodes = ParseMarkdown("> ![alt](img.png)");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Image);
    EXPECT_EQ(nodes[0].image_src, L"img.png");
}

TEST(ParserImage, ImageInTightListStaysListItem) {
    // タイトリスト内の画像はP生成されないため、ListItem のまま
    auto nodes = ParseMarkdown("- ![alt](img.png)");
    ASSERT_GE(nodes.size(), 1u);
    // ListItemノードの型が保持されること
    EXPECT_EQ(nodes[0].type, NodeType::ListItem);
    // image_srcはノードに設定されるが、型はListItemのまま
    EXPECT_EQ(nodes[0].image_src, L"img.png");
}

TEST(ParserImage, ImageInTightListDoesNotLeakToNextParagraph) {
    // 画像を含むリスト項目の後の段落が Image に変換されないこと
    auto nodes = ParseMarkdown("- ![alt](img.png)\n\nNormal paragraph");
    bool found_para = false;
    for (const auto& node : nodes) {
        if (node.type == NodeType::Paragraph) {
            found_para = true;
            EXPECT_TRUE(node.image_src.empty())
                << "後続の段落にimage_srcがリークしていないこと";
        }
    }
    EXPECT_TRUE(found_para) << "後続の段落ノードが存在すること";
}

TEST(ParserImage, ImageInTaskListStaysTaskListItem) {
    auto nodes = ParseMarkdown("- [x] ![alt](img.png)");
    ASSERT_GE(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::TaskListItem);
}

// ---- 画像がない場合 ----

TEST(ParserImage, ParagraphWithoutImageStaysParagraph) {
    auto nodes = ParseMarkdown("Just text");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_TRUE(nodes[0].image_src.empty());
}

TEST(ParserImage, LinkDoesNotTriggerImage) {
    auto nodes = ParseMarkdown("[link text](https://example.com)");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_TRUE(nodes[0].image_src.empty());
}

// ---- 複数ノード内の画像 ----

TEST(ParserImage, ImageBetweenParagraphs) {
    auto nodes = ParseMarkdown("Before\n\n![alt](img.png)\n\nAfter");
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_EQ(nodes[1].type, NodeType::Image);
    EXPECT_EQ(nodes[2].type, NodeType::Paragraph);
}

TEST(ParserImage, MultipleImageParagraphs) {
    auto nodes = ParseMarkdown("![a](a.png)\n\n![b](b.png)\n\n![c](c.png)");
    ASSERT_EQ(nodes.size(), 3u);
    for (size_t i = 0; i < 3; i++) {
        EXPECT_EQ(nodes[i].type, NodeType::Image) << "ノード " << i;
    }
    EXPECT_EQ(nodes[0].image_src, L"a.png");
    EXPECT_EQ(nodes[1].image_src, L"b.png");
    EXPECT_EQ(nodes[2].image_src, L"c.png");
}

// ---- エッジケース ----

TEST(ParserImage, ImageWithSpecialCharsInPath) {
    auto nodes = ParseMarkdown("![alt](path%20with%20spaces/img.png)");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Image);
    EXPECT_EQ(nodes[0].image_src, L"path%20with%20spaces/img.png");
}

TEST(ParserImage, ImageWithJapaneseAltText) {
    auto nodes = ParseMarkdown("![日本語のaltテキスト](img.png)");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Image);
    EXPECT_EQ(nodes[0].text, L"日本語のaltテキスト");
}

TEST(ParserImage, ImageWithJapanesePath) {
    auto nodes = ParseMarkdown("![alt](画像/テスト.png)");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].image_src, L"画像/テスト.png");
}

// ============================================================
// レイアウトテスト: 画像ノードの高さ計算
// ============================================================

class ImageLayoutTest : public ::testing::Test {
protected:
    MockTextMeasurer mock_;
    LayoutEngine engine_;
    Theme theme_;

    void SetUp() override {
        theme_ = GetLightTheme();
        ASSERT_TRUE(engine_.Init(&mock_, theme_));
    }
};

TEST_F(ImageLayoutTest, ImagePlaceholderHeight) {
    auto nodes = ParseMarkdown("![alt](img.png)");
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].type, NodeType::Image);

    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    EXPECT_GT(cache[0].height, 0.0f) << "画像プレースホルダーの高さは正であるべき";
    EXPECT_FALSE(cache[0].layout_dirty);
}

TEST_F(ImageLayoutTest, ImageWithDimensionsUsesScaledHeight) {
    auto nodes = ParseMarkdown("![alt](img.png)");
    ASSERT_EQ(nodes.size(), 1u);
    // 画像読み込み後のサイズをシミュレート
    nodes[0].image_width = 400.0f;
    nodes[0].image_height = 300.0f;

    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    // 幅400 < コンテンツ幅のため、元の高さ300が使われる
    EXPECT_FLOAT_EQ(cache[0].height, 300.0f);
}

TEST_F(ImageLayoutTest, WideImageScaledDown) {
    auto nodes = ParseMarkdown("![alt](img.png)");
    ASSERT_EQ(nodes.size(), 1u);
    // コンテンツ幅より大きい画像
    nodes[0].image_width = 1600.0f;
    nodes[0].image_height = 900.0f;

    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    // スケールダウンされて元の高さより小さくなること
    EXPECT_LT(cache[0].height, 900.0f);
    EXPECT_GT(cache[0].height, 0.0f);
}

TEST_F(ImageLayoutTest, SmallImageNotScaledUp) {
    auto nodes = ParseMarkdown("![alt](img.png)");
    ASSERT_EQ(nodes.size(), 1u);
    nodes[0].image_width = 100.0f;
    nodes[0].image_height = 80.0f;

    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    // 小さい画像は拡大されない
    EXPECT_FLOAT_EQ(cache[0].height, 80.0f);
}

TEST_F(ImageLayoutTest, ImageHeightRecalculatedOnWidthChange) {
    auto nodes = ParseMarkdown("![alt](img.png)");
    ASSERT_EQ(nodes.size(), 1u);
    nodes[0].image_width = 1600.0f;
    nodes[0].image_height = 900.0f;

    LayoutCache cache;
    cache.Resize(nodes.size());

    // 広い幅
    engine_.ComputeLayout(nodes, cache, 1800.0f);
    float h_wide = cache[0].height;

    // 狭い幅 → 再計算で高さが変わること
    engine_.ComputeLayout(nodes, cache, 400.0f);
    float h_narrow = cache[0].height;

    EXPECT_LT(h_narrow, h_wide);
}

TEST_F(ImageLayoutTest, ImageNodesDoNotOverlap) {
    auto nodes = ParseMarkdown("![a](a.png)\n\n![b](b.png)\n\n![c](c.png)");
    for (auto& n : nodes) {
        n.image_width = 200.0f;
        n.image_height = 150.0f;
    }

    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    for (size_t i = 1; i < nodes.size(); i++) {
        float prev_bottom = cache[i - 1].y_position + cache[i - 1].height;
        EXPECT_GE(cache[i].y_position, prev_bottom) << "ノード " << i << " が前のノードと重なっている";
    }
}

TEST_F(ImageLayoutTest, ImageBetweenTextNodesDoNotOverlap) {
    auto nodes = ParseMarkdown("Text before\n\n![alt](img.png)\n\nText after");
    ASSERT_EQ(nodes.size(), 3u);
    nodes[1].image_width = 400.0f;
    nodes[1].image_height = 300.0f;

    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    for (size_t i = 1; i < nodes.size(); i++) {
        float prev_bottom = cache[i - 1].y_position + cache[i - 1].height;
        EXPECT_GE(cache[i].y_position, prev_bottom);
    }
}

TEST_F(ImageLayoutTest, ImageWithZeroDimensionsGetsPlaceholder) {
    auto nodes = ParseMarkdown("![alt](img.png)");
    ASSERT_EQ(nodes.size(), 1u);
    // image_width/heightはデフォルトの0.0f
    ASSERT_FLOAT_EQ(nodes[0].image_width, 0.0f);

    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    EXPECT_GT(cache[0].height, 0.0f) << "プレースホルダー高さが設定されるべき";
}

TEST_F(ImageLayoutTest, ImageAspectRatioPreserved) {
    auto nodes = ParseMarkdown("![alt](img.png)");
    nodes[0].image_width = 1000.0f;
    nodes[0].image_height = 500.0f;

    LayoutCache cache;
    cache.Resize(nodes.size());
    // margin を除いたコンテンツ幅を考慮
    float viewport_width = 600.0f;
    engine_.ComputeLayout(nodes, cache, viewport_width);

    float content_width = viewport_width - theme_.margin_left - theme_.margin_right;
    // コンテンツ幅 < 画像幅なのでスケールされる
    // アスペクト比 = 500/1000 = 0.5
    float expected_height = content_width * (500.0f / 1000.0f);
    EXPECT_NEAR(cache[0].height, expected_height, 0.1f);
}

// ============================================================
// ImageLoader テスト: WIC による画像読み込み
// ============================================================

class ImageLoaderTest : public ::testing::Test {
protected:
    ComPtr<ID2D1Factory> d2d_factory_;
    ComPtr<IWICImagingFactory> wic_factory_;
    ComPtr<ID2D1RenderTarget> render_target_;
    ImageLoader loader_;
    std::filesystem::path temp_dir_;

    static void SetUpTestSuite() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ASSERT_TRUE(SUCCEEDED(hr)) << "COM初期化に失敗";
    }

    static void TearDownTestSuite() {
        CoUninitialize();
    }

    void SetUp() override {
        // D2Dファクトリ作成
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            d2d_factory_.GetAddressOf());
        ASSERT_TRUE(SUCCEEDED(hr)) << "D2Dファクトリ作成に失敗";

        // WICファクトリ作成
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_factory_));
        ASSERT_TRUE(SUCCEEDED(hr)) << "WICファクトリ作成に失敗";

        // WICビットマップベースのレンダーターゲットを作成（HWND不要）
        ComPtr<IWICBitmap> wic_bitmap;
        hr = wic_factory_->CreateBitmap(1, 1, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapCacheOnLoad, &wic_bitmap);
        ASSERT_TRUE(SUCCEEDED(hr)) << "WICビットマップ作成に失敗";

        hr = d2d_factory_->CreateWicBitmapRenderTarget(
            wic_bitmap.Get(), D2D1::RenderTargetProperties(), &render_target_);
        ASSERT_TRUE(SUCCEEDED(hr)) << "レンダーターゲット作成に失敗";

        loader_.Init(render_target_.Get());

        // テスト用一時ディレクトリ
        temp_dir_ = std::filesystem::temp_directory_path() / "mendo_test_images";
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override {
        loader_.ClearCache();
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    // WICエンコーダーでテスト画像を作成するヘルパー
    bool CreateTestImage(const std::wstring& filename, const GUID& container_format,
        UINT width, UINT height)
    {
        auto path = temp_dir_ / filename;
        ComPtr<IWICBitmapEncoder> encoder;
        HRESULT hr = wic_factory_->CreateEncoder(container_format, nullptr, &encoder);
        if (FAILED(hr)) {
            return false;
        }

        ComPtr<IStream> stream;
        hr = SHCreateStreamOnFileW(path.wstring().c_str(), STGM_CREATE | STGM_WRITE, &stream);
        if (FAILED(hr)) {
            return false;
        }

        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (FAILED(hr)) {
            return false;
        }

        ComPtr<IWICBitmapFrameEncode> frame;
        hr = encoder->CreateNewFrame(&frame, nullptr);
        if (FAILED(hr)) {
            return false;
        }

        hr = frame->Initialize(nullptr);
        if (FAILED(hr)) {
            return false;
        }

        hr = frame->SetSize(width, height);
        if (FAILED(hr)) {
            return false;
        }

        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        hr = frame->SetPixelFormat(&format);
        if (FAILED(hr)) {
            return false;
        }

        // 赤色のピクセルデータを書き込み
        UINT stride = width * 4;
        std::vector<BYTE> pixels(stride * height);
        for (UINT y = 0; y < height; y++) {
            for (UINT x = 0; x < width; x++) {
                UINT offset = y * stride + x * 4;
                pixels[offset + 0] = 0;     // B
                pixels[offset + 1] = 0;     // G
                pixels[offset + 2] = 255;   // R
                pixels[offset + 3] = 255;   // A
            }
        }

        hr = frame->WritePixels(height, stride, static_cast<UINT>(pixels.size()), pixels.data());
        if (FAILED(hr)) {
            return false;
        }

        hr = frame->Commit();
        if (FAILED(hr)) {
            return false;
        }

        hr = encoder->Commit();
        return SUCCEEDED(hr);
    }

    std::wstring GetTestImagePath(const std::wstring& filename) {
        return (temp_dir_ / filename).wstring();
    }
};

// ---- 正常系: 画像フォーマット ----

TEST_F(ImageLoaderTest, LoadPng) {
    ASSERT_TRUE(CreateTestImage(L"test.png", GUID_ContainerFormatPng, 100, 80));
    DiagramEntry entry;
    EXPECT_TRUE(loader_.LoadImage(GetTestImagePath(L"test.png"), entry));
    EXPECT_TRUE(entry.bitmap);
    EXPECT_FLOAT_EQ(entry.width, 100.0f);
    EXPECT_FLOAT_EQ(entry.height, 80.0f);
}

TEST_F(ImageLoaderTest, LoadBmp) {
    ASSERT_TRUE(CreateTestImage(L"test.bmp", GUID_ContainerFormatBmp, 50, 50));
    DiagramEntry entry;
    EXPECT_TRUE(loader_.LoadImage(GetTestImagePath(L"test.bmp"), entry));
    EXPECT_TRUE(entry.bitmap);
    EXPECT_FLOAT_EQ(entry.width, 50.0f);
    EXPECT_FLOAT_EQ(entry.height, 50.0f);
}

TEST_F(ImageLoaderTest, LoadJpeg) {
    ASSERT_TRUE(CreateTestImage(L"test.jpg", GUID_ContainerFormatJpeg, 200, 150));
    DiagramEntry entry;
    EXPECT_TRUE(loader_.LoadImage(GetTestImagePath(L"test.jpg"), entry));
    EXPECT_TRUE(entry.bitmap);
    EXPECT_FLOAT_EQ(entry.width, 200.0f);
    EXPECT_FLOAT_EQ(entry.height, 150.0f);
}

TEST_F(ImageLoaderTest, LoadLargeImage) {
    ASSERT_TRUE(CreateTestImage(L"large.png", GUID_ContainerFormatPng, 4096, 2048));
    DiagramEntry entry;
    EXPECT_TRUE(loader_.LoadImage(GetTestImagePath(L"large.png"), entry));
    EXPECT_TRUE(entry.bitmap);
    EXPECT_FLOAT_EQ(entry.width, 4096.0f);
    EXPECT_FLOAT_EQ(entry.height, 2048.0f);
}

TEST_F(ImageLoaderTest, LoadOneByOneImage) {
    ASSERT_TRUE(CreateTestImage(L"tiny.png", GUID_ContainerFormatPng, 1, 1));
    DiagramEntry entry;
    EXPECT_TRUE(loader_.LoadImage(GetTestImagePath(L"tiny.png"), entry));
    EXPECT_TRUE(entry.bitmap);
    EXPECT_FLOAT_EQ(entry.width, 1.0f);
    EXPECT_FLOAT_EQ(entry.height, 1.0f);
}

// ---- 正常系: キャッシュ動作 ----

TEST_F(ImageLoaderTest, CacheHitReturnsSameBitmap) {
    ASSERT_TRUE(CreateTestImage(L"cached.png", GUID_ContainerFormatPng, 64, 64));
    auto path = GetTestImagePath(L"cached.png");

    DiagramEntry entry1;
    EXPECT_TRUE(loader_.LoadImage(path, entry1));

    DiagramEntry entry2;
    EXPECT_TRUE(loader_.LoadImage(path, entry2));

    // 同じビットマップオブジェクトが返されること
    EXPECT_EQ(entry1.bitmap.Get(), entry2.bitmap.Get());
}

TEST_F(ImageLoaderTest, DifferentPathsDifferentBitmaps) {
    ASSERT_TRUE(CreateTestImage(L"img1.png", GUID_ContainerFormatPng, 32, 32));
    ASSERT_TRUE(CreateTestImage(L"img2.png", GUID_ContainerFormatPng, 64, 64));

    DiagramEntry entry1, entry2;
    EXPECT_TRUE(loader_.LoadImage(GetTestImagePath(L"img1.png"), entry1));
    EXPECT_TRUE(loader_.LoadImage(GetTestImagePath(L"img2.png"), entry2));

    EXPECT_NE(entry1.bitmap.Get(), entry2.bitmap.Get());
    EXPECT_FLOAT_EQ(entry1.width, 32.0f);
    EXPECT_FLOAT_EQ(entry2.width, 64.0f);
}

TEST_F(ImageLoaderTest, ClearCacheInvalidatesEntries) {
    ASSERT_TRUE(CreateTestImage(L"clear.png", GUID_ContainerFormatPng, 40, 40));
    auto path = GetTestImagePath(L"clear.png");

    DiagramEntry entry1;
    EXPECT_TRUE(loader_.LoadImage(path, entry1));
    auto* first_bitmap = entry1.bitmap.Get();

    loader_.ClearCache();

    DiagramEntry entry2;
    EXPECT_TRUE(loader_.LoadImage(path, entry2));

    // キャッシュクリア後は新しいビットマップが作成される
    EXPECT_NE(first_bitmap, entry2.bitmap.Get());
}

// ---- 異常系: ファイルが存在しない ----

TEST_F(ImageLoaderTest, NonExistentFileReturnsFalse) {
    DiagramEntry entry;
    EXPECT_FALSE(loader_.LoadImage(L"C:\\nonexistent\\path\\image.png", entry));
    EXPECT_FALSE(entry.bitmap);
}

TEST_F(ImageLoaderTest, EmptyPathReturnsFalse) {
    DiagramEntry entry;
    EXPECT_FALSE(loader_.LoadImage(L"", entry));
    EXPECT_FALSE(entry.bitmap);
}

// ---- 異常系: 壊れた画像ファイル ----

TEST_F(ImageLoaderTest, CorruptedPngReturnsFalse) {
    auto path = temp_dir_ / L"corrupt.png";
    // PNGヘッダの途中で切れたデータ
    std::ofstream f(path, std::ios::binary);
    f.write("\x89PNG\r\n\x1a\n\x00\x00", 10);
    f.close();

    DiagramEntry entry;
    EXPECT_FALSE(loader_.LoadImage(path.wstring(), entry));
    EXPECT_FALSE(entry.bitmap);
}

TEST_F(ImageLoaderTest, EmptyFileReturnsFalse) {
    auto path = temp_dir_ / L"empty.png";
    std::ofstream f(path, std::ios::binary);
    f.close();

    DiagramEntry entry;
    EXPECT_FALSE(loader_.LoadImage(path.wstring(), entry));
    EXPECT_FALSE(entry.bitmap);
}

TEST_F(ImageLoaderTest, RandomBytesReturnsFalse) {
    auto path = temp_dir_ / L"random.png";
    std::ofstream f(path, std::ios::binary);
    const char garbage[] = "This is not an image file at all!";
    f.write(garbage, sizeof(garbage));
    f.close();

    DiagramEntry entry;
    EXPECT_FALSE(loader_.LoadImage(path.wstring(), entry));
    EXPECT_FALSE(entry.bitmap);
}

TEST_F(ImageLoaderTest, TruncatedJpegReturnsFalse) {
    // JPEG SOIマーカーのみの不完全データ
    auto path = temp_dir_ / L"truncated.jpg";
    std::ofstream f(path, std::ios::binary);
    f.write("\xFF\xD8\xFF\xE0\x00\x10", 6);
    f.close();

    DiagramEntry entry;
    EXPECT_FALSE(loader_.LoadImage(path.wstring(), entry));
    EXPECT_FALSE(entry.bitmap);
}

// ---- 異常系: 非画像ファイル ----

TEST_F(ImageLoaderTest, TextFileReturnsFalse) {
    auto path = temp_dir_ / L"readme.txt";
    std::ofstream f(path);
    f << "Hello, World!";
    f.close();

    DiagramEntry entry;
    EXPECT_FALSE(loader_.LoadImage(path.wstring(), entry));
    EXPECT_FALSE(entry.bitmap);
}

TEST_F(ImageLoaderTest, HtmlFileReturnsFalse) {
    auto path = temp_dir_ / L"page.html";
    std::ofstream f(path);
    f << "<html><body>test</body></html>";
    f.close();

    DiagramEntry entry;
    EXPECT_FALSE(loader_.LoadImage(path.wstring(), entry));
    EXPECT_FALSE(entry.bitmap);
}

// ---- 異常系: 未初期化の状態 ----

TEST_F(ImageLoaderTest, UninitializedLoaderReturnsFalse) {
    ImageLoader uninitialized;
    ASSERT_TRUE(CreateTestImage(L"valid.png", GUID_ContainerFormatPng, 10, 10));

    DiagramEntry entry;
    EXPECT_FALSE(uninitialized.LoadImage(GetTestImagePath(L"valid.png"), entry));
    EXPECT_FALSE(entry.bitmap);
}

TEST_F(ImageLoaderTest, NullRenderTargetReturnsFalse) {
    ImageLoader loader_null;
    loader_null.Init(nullptr);

    DiagramEntry entry;
    EXPECT_FALSE(loader_null.LoadImage(GetTestImagePath(L"valid.png"), entry));
}

// ---- 異常系: エントリの状態保証 ----

TEST_F(ImageLoaderTest, FailedLoadDoesNotModifyExistingEntry) {
    DiagramEntry entry;
    entry.width = 999.0f;
    entry.height = 888.0f;

    // 存在しないファイルのロードを試みる
    EXPECT_FALSE(loader_.LoadImage(L"C:\\no_such_file.png", entry));

    // 既存の値が変更されていないこと
    EXPECT_FLOAT_EQ(entry.width, 999.0f);
    EXPECT_FLOAT_EQ(entry.height, 888.0f);
    EXPECT_FALSE(entry.bitmap);
}

// ---- GetCachedImage テスト ----

TEST_F(ImageLoaderTest, GetCachedImageReturnsFalseWhenNotCached) {
    DiagramEntry entry;
    EXPECT_FALSE(loader_.GetCachedImage(L"C:\\not_cached.png", entry));
}

TEST_F(ImageLoaderTest, GetCachedImageReturnsTrueAfterLoadImage) {
    ASSERT_TRUE(CreateTestImage(L"cached_test.png", GUID_ContainerFormatPng, 120, 90));
    auto path = GetTestImagePath(L"cached_test.png");

    // LoadImage でキャッシュに格納
    DiagramEntry entry1;
    ASSERT_TRUE(loader_.LoadImage(path, entry1));

    // GetCachedImage でキャッシュから取得できること
    DiagramEntry entry2;
    EXPECT_TRUE(loader_.GetCachedImage(path, entry2));
    EXPECT_EQ(entry1.bitmap.Get(), entry2.bitmap.Get());
    EXPECT_FLOAT_EQ(entry2.width, 120.0f);
    EXPECT_FLOAT_EQ(entry2.height, 90.0f);
}

TEST_F(ImageLoaderTest, GetCachedImageReturnsFalseAfterClearCache) {
    ASSERT_TRUE(CreateTestImage(L"clear_test.png", GUID_ContainerFormatPng, 30, 30));
    auto path = GetTestImagePath(L"clear_test.png");

    DiagramEntry entry;
    ASSERT_TRUE(loader_.LoadImage(path, entry));
    loader_.ClearCache();

    EXPECT_FALSE(loader_.GetCachedImage(path, entry));
}

// ---- Shutdown テスト ----

TEST_F(ImageLoaderTest, ShutdownWithoutInitAsyncIsNoOp) {
    ImageLoader loader;
    loader.Init(render_target_.Get());
    loader.Shutdown();  // InitAsync 未呼び出しでもクラッシュしないこと
}

TEST_F(ImageLoaderTest, CancelPendingIsNoOp) {
    // CancelPending が空の状態でもクラッシュしないこと
    loader_.CancelPending();
}

// ---- ファイルロック回避テスト ----

TEST_F(ImageLoaderTest, FileNotLockedAfterSyncLoad) {
    ASSERT_TRUE(CreateTestImage(L"lock_test.png", GUID_ContainerFormatPng, 64, 64));
    auto path = GetTestImagePath(L"lock_test.png");

    DiagramEntry entry;
    ASSERT_TRUE(loader_.LoadImage(path, entry));

    // 読み込み後、外部プロセスと同様に書き込みモードでファイルを開けること
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_NE(hFile, INVALID_HANDLE_VALUE)
        << "画像読み込み後にファイルが書き込みロックされている";
    if (hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(hFile);
    }
}

TEST_F(ImageLoaderTest, FileCanBeDeletedAfterLoad) {
    ASSERT_TRUE(CreateTestImage(L"deletable.png", GUID_ContainerFormatPng, 32, 32));
    auto path = GetTestImagePath(L"deletable.png");

    DiagramEntry entry;
    ASSERT_TRUE(loader_.LoadImage(path, entry));

    // 読み込み後にファイルを削除できること（ロックされていない証拠）
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::remove(path, ec))
        << "画像読み込み後にファイルを削除できなかった: " << ec.message();
}

TEST_F(ImageLoaderTest, FileNotLockedAfterFailedLoad) {
    // 壊れた画像でも読み込み後にファイルがロックされないこと
    auto path = temp_dir_ / L"bad_lock.png";
    {
        std::ofstream f(path, std::ios::binary);
        f.write("\x89PNG\r\n\x1a\n\x00\x00", 10);
    }

    DiagramEntry entry;
    loader_.LoadImage(path.wstring(), entry);

    HANDLE hFile = CreateFileW(path.wstring().c_str(), GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_NE(hFile, INVALID_HANDLE_VALUE)
        << "失敗した読み込み後にファイルがロックされている";
    if (hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(hFile);
    }
}

// ============================================================
// 非同期読み込みテスト
// ============================================================

class ImageLoaderAsyncTest : public ImageLoaderTest {
protected:
    // コールバック発火を検知するためのカウンター
    static std::atomic<int> callback_count_;

    static void OnComplete(void* /*ctx*/) {
        callback_count_.fetch_add(1);
    }

    void SetUp() override {
        ImageLoaderTest::SetUp();
        callback_count_.store(0);
        // InitAsync にはウィンドウハンドルが必要だが、テストでは PostMessage を
        // 受け取れないため HWND は nullptr で起動し、手動で ProcessCompletedDecodes を呼ぶ
        loader_.InitAsync(nullptr, 0);
    }

    void TearDown() override {
        loader_.Shutdown();
        ImageLoaderTest::TearDown();
    }

    // ワーカーの処理完了を待つ（最大 timeout_ms ミリ秒）
    bool WaitForResults(int expected_count, int timeout_ms = 5000) {
        auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            loader_.ProcessCompletedDecodes();
            if (callback_count_.load() >= expected_count) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // 最後にもう一度試行
        loader_.ProcessCompletedDecodes();
        return callback_count_.load() >= expected_count;
    }
};

std::atomic<int> ImageLoaderAsyncTest::callback_count_{0};

TEST_F(ImageLoaderAsyncTest, AsyncLoadPopulatesCache) {
    ASSERT_TRUE(CreateTestImage(L"async.png", GUID_ContainerFormatPng, 120, 90));
    auto path = GetTestImagePath(L"async.png");

    loader_.RequestLoadAsync(path, OnComplete, nullptr);
    ASSERT_TRUE(WaitForResults(1)) << "非同期読み込みが完了しなかった";

    DiagramEntry entry;
    EXPECT_TRUE(loader_.GetCachedImage(path, entry));
    EXPECT_TRUE(entry.bitmap);
    EXPECT_FLOAT_EQ(entry.width, 120.0f);
    EXPECT_FLOAT_EQ(entry.height, 90.0f);
}

TEST_F(ImageLoaderAsyncTest, DuplicateRequestIsIgnored) {
    ASSERT_TRUE(CreateTestImage(L"dup.png", GUID_ContainerFormatPng, 50, 50));
    auto path = GetTestImagePath(L"dup.png");

    // 同じパスを2回リクエスト — 重複は無視される
    loader_.RequestLoadAsync(path, OnComplete, nullptr);
    loader_.RequestLoadAsync(path, OnComplete, nullptr);

    ASSERT_TRUE(WaitForResults(1));

    // コールバックは1回のみ（バッチの最後の1件）
    EXPECT_EQ(callback_count_.load(), 1);
}

TEST_F(ImageLoaderAsyncTest, MultiplePathsAllCached) {
    ASSERT_TRUE(CreateTestImage(L"a.png", GUID_ContainerFormatPng, 10, 10));
    ASSERT_TRUE(CreateTestImage(L"b.png", GUID_ContainerFormatPng, 20, 20));
    auto path_a = GetTestImagePath(L"a.png");
    auto path_b = GetTestImagePath(L"b.png");

    loader_.RequestLoadAsync(path_a, OnComplete, nullptr);
    loader_.RequestLoadAsync(path_b, OnComplete, nullptr);

    // 少なくとも1回コールバックが来るのを待つ
    ASSERT_TRUE(WaitForResults(1));

    DiagramEntry entry_a, entry_b;
    EXPECT_TRUE(loader_.GetCachedImage(path_a, entry_a));
    EXPECT_TRUE(loader_.GetCachedImage(path_b, entry_b));
    EXPECT_FLOAT_EQ(entry_a.width, 10.0f);
    EXPECT_FLOAT_EQ(entry_b.width, 20.0f);
}

TEST_F(ImageLoaderAsyncTest, FileNotLockedAfterAsyncLoad) {
    ASSERT_TRUE(CreateTestImage(L"async_lock.png", GUID_ContainerFormatPng, 80, 60));
    auto path = GetTestImagePath(L"async_lock.png");

    loader_.RequestLoadAsync(path, OnComplete, nullptr);
    ASSERT_TRUE(WaitForResults(1)) << "非同期読み込みが完了しなかった";

    // 非同期読み込み完了後、書き込みモードでファイルを開けること
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_NE(hFile, INVALID_HANDLE_VALUE)
        << "非同期読み込み後にファイルが書き込みロックされている";
    if (hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(hFile);
    }
}

TEST_F(ImageLoaderAsyncTest, CancelPendingClearsQueue) {
    ASSERT_TRUE(CreateTestImage(L"cancel.png", GUID_ContainerFormatPng, 30, 30));
    auto path = GetTestImagePath(L"cancel.png");

    loader_.RequestLoadAsync(path, OnComplete, nullptr);
    loader_.CancelPending();

    // キャンセル後、短時間待ってもコールバックが来ないこと
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    loader_.ProcessCompletedDecodes();

    // キャンセルのタイミングにより結果はゼロまたはキャッシュ済みになりうるが、
    // コールバックは発火しないこと
    EXPECT_EQ(callback_count_.load(), 0);
}
