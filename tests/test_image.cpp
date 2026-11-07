#include <gtest/gtest.h>
#include <filesystem>
#include "parser.h"
#include "layout.h"
#include "layout_cache.h"
#include "mock_text_measurer.h"
#include "file_io.h"
#include "image_loader.h"
#include "layout_invariants.h"
#include "parse_invariants.h"
#include "wic_util.h"
#include "task_scheduler.h"
#include "test_helpers.h"
#include "test_nodes.h"
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <shlwapi.h>
#include <deque>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "document_test_helpers.h"
#include <algorithm>
#include <ranges>

namespace {

// FindCachedImage の結果 (bitmap と、今の DPI での寸法)。
struct LoadedImage {
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    float width = 0.0f;
    float height = 0.0f;
};

inline bool GetCachedImage(ImageLoader& loader, const std::wstring& path, LoadedImage& out)
{
    const auto* cached = loader.FindCachedImage(path);
    if (!cached) {
        return false;
    }
    out.bitmap = cached->bitmap;
    const auto size = loader.DipSize(*cached);
    out.width = size.width;
    out.height = size.height;
    return true;
}

// 共有なしで書き込み用に開ければ、ローダーがファイルを開いたままにしていない。
bool CanOpenExclusively(const std::wstring& path)
{
    const UniqueHandle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    return static_cast<bool>(file);
}

} // namespace

using Microsoft::WRL::ComPtr;

// ============================================================
// パーサーテスト: 本文中の画像 (TextRun::kImage)
// ============================================================

// 画像は段落を画像のノードに変えず、alt の範囲の run にする (GitHub と同じく文と並べて描く)。
TEST(ParserImage, ImageBecomesRunInParagraph)
{
    struct Case {
        const char* md;
        const char* src;
        const char* alt;
    };
    const Case cases[] = {
        {"![alt text](image.png)", "image.png", "alt text"},
        {"![photo](./images/photo.jpg)", "./images/photo.jpg", "photo"},
        {"![img](C:/Users/test/pic.png)", "C:/Users/test/pic.png", "img"},
        {"![logo](https://example.com/logo.png)", "https://example.com/logo.png", "logo"},
        {"![alt](image.png \"My Title\")", "image.png", "alt"},
        {"> ![alt](img.png)", "img.png", "alt"},
        // CommonMark では画像説明内に画像記法を書ける。src は最外側を採用し、内側は alt の文字になる。
        {"![outer ![inner](inner.png)](outer.png)", "outer.png", "outer inner"},
        {"![alt](path%20with%20spaces/img.png)", "path%20with%20spaces/img.png", "alt"},
        // エスケープを含む src は md4c が一時バッファで組み立てて渡すので、原文ではなく arena へ写す。
        {"![alt](img\\_x.png)", "img_x.png", "alt"},
        // 参照形式の画像の src は原文の参照定義から取る。
        {"![alt][logo]\n\n[logo]: ref.png", "ref.png", "alt"},
        {"![日本語のaltテキスト](img.png)", "img.png", "日本語のaltテキスト"},
        {"![alt](画像/テスト.png)", "画像/テスト.png", "alt"},
        // alt が空なら置き換える文字を置き、画像を置く範囲を空にしない。
        {"![](image.png)", "image.png", "\xEF\xBF\xBC"},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.md);
        const auto nodes = ParseNodes(c.md);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
        EXPECT_EQ(nodes[0].GetText(), c.alt);
        const auto images = ImagesOf(nodes[0]);
        ASSERT_EQ(images.size(), 1u);
        EXPECT_EQ(images[0].first, c.alt);
        EXPECT_EQ(images[0].second, c.src);
        EXPECT_TRUE(ParseInvariantsHold(c.md, nodes));
    }
}

// 文と画像の並ぶ段落は、文も全部の画像も残す。同じ src は 1 つの slot にまとめ、隣り合っても別の画像にする。
TEST(ParserImage, ParagraphKeepsTextAndEveryImage)
{
    const char* md = "before ![a](first.png) ![b](second.png)![c](first.png) after";
    const auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_EQ(nodes[0].GetText(), "before a bc after");
    const auto images = ImagesOf(nodes[0]);
    ASSERT_EQ(images.size(), 3u);
    EXPECT_EQ(images[0], (std::pair<std::string, std::string_view>{ "a", "first.png" }));
    EXPECT_EQ(images[1], (std::pair<std::string, std::string_view>{ "b", "second.png" }));
    EXPECT_EQ(images[2], (std::pair<std::string, std::string_view>{ "c", "first.png" }));
    const auto runs = nodes[0].runs();
    ASSERT_EQ(runs.size(), 3u);
    EXPECT_EQ(runs[0].image_slot, runs[2].image_slot);
    EXPECT_NE(runs[0].image_slot, runs[1].image_slot);
    EXPECT_TRUE(ParseInvariantsHold(md, nodes));
}

// alt は平文で、中の装飾・リンク・数式があっても画像 1 つが run 1 つになる。外側の装飾は画像の run に付く。
TEST(ParserImage, AltIsPlainTextInOneRun)
{
    struct Case {
        const char* md;
        const char* alt;
        uint8_t flags;
    };
    const Case cases[] = {
        { "![a *b* c](x.png)", "a b c", TextRun::kImage },
        { "![a [b](u) `c`](x.png)", "a b c", TextRun::kImage },
        { "![a $x$](x.png)", "a x", TextRun::kImage },
        { "*![a **b**](x.png)*", "a b", TextRun::kImage | TextRun::kItalic },
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.md);
        const auto nodes = ParseNodes(c.md);
        ASSERT_EQ(nodes.size(), 1u);
        const auto runs = nodes[0].runs();
        ASSERT_EQ(runs.size(), 1u);
        EXPECT_EQ(runs[0].flags, c.flags);
        EXPECT_FALSE(runs[0].has_link());
        EXPECT_EQ(ImagesOf(nodes[0]), (std::vector<std::pair<std::string, std::string_view>>{ { c.alt, "x.png" } }));
        EXPECT_TRUE(ParseInvariantsHold(c.md, nodes));
    }
}

// リンクの付いた画像 (バッジ) は、画像の run がリンクも持つ。
TEST(ParserImage, LinkedImageKeepsLink)
{
    const char* md = "[![build](b1.svg)](https://x.example/build) [![license](b2.svg)](https://x.example/license)";
    const auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 1u);
    const auto runs = nodes[0].runs();
    const auto urls = nodes[0].view_link_urls();
    std::vector<std::pair<std::string_view, std::string_view>> found;
    for (const auto& r : runs) {
        if (r.image()) {
            ASSERT_TRUE(r.has_link());
            found.emplace_back(nodes[0].image_src(r), urls[static_cast<size_t>(r.link_url_index)]);
        }
    }
    ASSERT_EQ(found.size(), 2u);
    EXPECT_EQ(found[0], (std::pair<std::string_view, std::string_view>{ "b1.svg", "https://x.example/build" }));
    EXPECT_EQ(found[1], (std::pair<std::string_view, std::string_view>{ "b2.svg", "https://x.example/license" }));
    EXPECT_TRUE(ParseInvariantsHold(md, nodes));
}

// src の無い画像は描けないので alt を文字のまま残す。
TEST(ParserImage, ImageWithoutSrcStaysText)
{
    const auto nodes = ParseNodes("![alt]()");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].GetText(), "alt");
    EXPECT_TRUE(nodes[0].runs().empty());
}

// 1 ノードの src は kMaxImageSlots 個まで。超えた画像は alt を文字のまま出す。
TEST(ParserImage, ImagesBeyondSlotLimitStayText)
{
    std::string md;
    for (size_t i = 0; i < kMaxImageSlots + 2; ++i) {
        md += std::format("![{}](i{}.png) ", i, i);
    }
    const auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 1u);
    const auto images = ImagesOf(nodes[0]);
    ASSERT_EQ(images.size(), kMaxImageSlots);
    EXPECT_EQ(images.back().second, std::format("i{}.png", kMaxImageSlots - 1));
    EXPECT_TRUE(nodes[0].GetText().ends_with(std::format("{} {}", kMaxImageSlots, kMaxImageSlots + 1)));
    EXPECT_TRUE(ParseInvariantsHold(md, nodes));
}

// 長い段落は区切るが、画像の途中では区切らない (1 つの画像が前後のノードに 1 つずつ出る)。
TEST(ParserImage, LongAltIsNotSplit)
{
    const std::string alt(kTextChunkTarget + kTextChunkWindow * 2, 'a');
    const std::string md = "![" + alt + "](x.png)";
    const auto nodes = ParseNodes(md);
    ASSERT_EQ(nodes.size(), 1u);
    const auto images = ImagesOf(nodes[0]);
    ASSERT_EQ(images.size(), 1u);
    EXPECT_EQ(images[0].first, alt);
    EXPECT_TRUE(ParseInvariantsHold(md, nodes));
}

// ---- ブロック要素内の画像 ----

// テーブルセル内の画像はセルの run になり、src は表のノードで引く。NodeTableData を壊さない。
TEST(ParserImage, ImageInTableCell)
{
    const char* md = "| ![badge](a.png) text | b |\n|---|---|\n| c | ![](d.png) |";
    const auto result = ParseMarkdown(md);
    const auto& nodes = result.nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Table);
    const auto* tbl = nodes[0].table_data();
    ASSERT_NE(tbl, nullptr);
    EXPECT_EQ(tbl->row_count, 2u);
    EXPECT_EQ(ImagesOf(nodes[0], tbl->GetCellText(0, 0), tbl->GetCellRuns(0, 0)),
              (std::vector<std::pair<std::string, std::string_view>>{ { "badge", "a.png" } }));
    EXPECT_EQ(ImagesOf(nodes[0], tbl->GetCellText(1, 1), tbl->GetCellRuns(1, 1)),
              (std::vector<std::pair<std::string, std::string_view>>{ { "\xEF\xBF\xBC", "d.png" } }));
    EXPECT_EQ(result.indices.image_indices, std::vector<size_t>{ 0 });
    EXPECT_TRUE(ParseInvariantsHold(md, nodes));
}

// 段落以外のブロックの画像も run になり、ブロックの種類と番号を保つ (#298)。
TEST(ParserImage, ImageInOtherBlocksKeepsItsKind)
{
    struct Case {
        std::string_view md;
        NodeType type;
        int heading_level = 0;
        int list_number = 0;
        bool checked = false;
    };
    constexpr Case kCases[] = {
        { "# ![logo](l.png) Title", NodeType::Heading, 1 },
        { "- ![alt](img.png)", NodeType::ListItem },
        { "3. ![alt](img.png)", NodeType::ListItem, 0, 3 },
        { "- [x] ![alt](img.png)", NodeType::TaskListItem, 0, 0, true },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.md);
        const auto nodes = ParseNodes(c.md);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_EQ(nodes[0].type, c.type);
        EXPECT_EQ(ImagesOf(nodes[0]).size(), 1u);
        EXPECT_EQ(nodes[0].heading_level, c.heading_level);
        if (c.type == NodeType::ListItem) {
            EXPECT_EQ(nodes[0].list_ordered(), c.list_number > 0);
            EXPECT_EQ(nodes[0].list_number(), c.list_number);
        }
        if (c.type == NodeType::TaskListItem) {
            EXPECT_EQ(nodes[0].task_checked(), c.checked);
        }
    }
}

// 脚注の定義は戻りのリンク "↩" をパースの後で足す。リンク URL を足しても画像の src を引ける。
TEST(ParserImage, ImageInFootnoteDefKeepsNumberAndSrc)
{
    const char* md = "text[^1] and[^1]\n\n[^1]: ![alt](img.png) [l](u.md)";
    const auto result = ParseMarkdown(md);
    ASSERT_EQ(result.indices.footnote_def_indices.size(), 1u);
    const auto& def = result.nodes[result.indices.footnote_def_indices[0]];
    EXPECT_EQ(def.type, NodeType::ListItem);
    EXPECT_TRUE(def.list_ordered());
    EXPECT_EQ(def.list_number(), 1);
    EXPECT_EQ(ImagesOf(def), (std::vector<std::pair<std::string, std::string_view>>{ { "alt", "img.png" } }));
    EXPECT_EQ(def.view_link_urls().size(), 3u) << "本文のリンクと戻りのリンク 2 つ";
    EXPECT_TRUE(ParseInvariantsHold(md, result.nodes));
}

// 画像を含むリスト項目の後の段落に画像が漏れない。
TEST(ParserImage, ImageInTightListDoesNotLeakToNextParagraph)
{
    const auto result = ParseMarkdown("- ![alt](img.png)\n\nNormal paragraph");
    const auto& nodes = result.nodes;
    ASSERT_EQ(nodes.size(), 2u);
    EXPECT_EQ(ImagesOf(nodes[0]).size(), 1u);
    EXPECT_EQ(nodes[1].type, NodeType::Paragraph);
    EXPECT_TRUE(nodes[1].runs().empty());
    EXPECT_EQ(result.indices.image_indices, std::vector<size_t>{ 0 });
}

TEST(ParserImage, ImageParagraphsAreIndexed)
{
    const auto result = ParseMarkdown("Before\n\n![a](a.png)\n\n![b](b.png)\n\nAfter");
    const auto& nodes = result.nodes;
    ASSERT_EQ(nodes.size(), 4u);
    for (const auto& node : nodes) {
        EXPECT_EQ(node.type, NodeType::Paragraph);
    }
    EXPECT_EQ(result.indices.image_indices, (std::vector<size_t>{ 1, 2 }));
    EXPECT_EQ(ImagesOf(nodes[1])[0].second, "a.png");
    EXPECT_EQ(ImagesOf(nodes[2])[0].second, "b.png");
}

// ============================================================
// ImageLoader テスト: WIC による画像読み込み
// ============================================================

class ImageLoaderTest : public ComApartmentTest {
protected:
    // 新しいファクトリでの描画先の作成は重い (毎回作ると 1 本あたり約 3.5ms、共有すると約 1ms) ので、スイートで共有する。
    // COM の初期化より後に作り、CoUninitialize より前に手放す。
    static void SetUpTestSuite()
    {
        ComApartmentTest::SetUpTestSuite();
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_factory_.GetAddressOf());
        wic_factory_ = wic_util::CreateWicFactory("ImageLoaderTest");
    }

    static void TearDownTestSuite()
    {
        wic_factory_.Reset();
        d2d_factory_.Reset();
        ComApartmentTest::TearDownTestSuite();
    }

    static inline ComPtr<ID2D1Factory> d2d_factory_;
    static inline ComPtr<IWICImagingFactory> wic_factory_;

    // loader_ や COM オブジェクトより後に破棄し、ファイル削除を最後に行う
    ScopedTempDir temp_dir_;
    ComPtr<ID2D1RenderTarget> render_target_;
    ImageLoader loader_;
    TaskScheduler scheduler_{ 2 };

    void SetUp() override
    {
        ASSERT_TRUE(d2d_factory_) << "D2Dファクトリ作成に失敗";
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

    // 積んだデコードを終わらせ、届いた結果をすべて取り込む。取り込んだものがあれば true (失敗の結果も数える)。
    bool DrainDecodes(ImageLoader& loader)
    {
        return DrainScheduler(scheduler_) && loader.ProcessCompletedDecodes(kNoDeadline);
    }
    bool DrainDecodes()
    {
        return DrainDecodes(loader_);
    }

    // 非同期経路でデコードさせ、完了後にキャッシュされた結果を取り出す。キャッシュに当たれば取り込むものは無い。
    bool LoadAndGet(ImageLoader& loader, const std::wstring& path, LoadedImage& out)
    {
        loader.RequestLoadAsync(path, 0.0f);
        (void)DrainDecodes(loader);
        return GetCachedImage(loader, path, out);
    }
    bool LoadAndGet(const std::wstring& path, LoadedImage& out)
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

    LoadedImage entry;
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
        { L"tiny.png", GUID_ContainerFormatPng, 1, 1 },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ c.filename }));
        ASSERT_TRUE(CreateTestImage(c.filename, c.container, c.width, c.height));
        LoadedImage entry;
        EXPECT_TRUE(LoadAndGet(GetTestImagePath(c.filename), entry));
        ASSERT_TRUE(entry.bitmap);
        EXPECT_FLOAT_EQ(entry.width, static_cast<float>(c.width));
        EXPECT_FLOAT_EQ(entry.height, static_cast<float>(c.height));
        const auto px = entry.bitmap->GetPixelSize();
        EXPECT_EQ(px.width, c.width);
        EXPECT_EQ(px.height, c.height);
    }
}

// ---- 正常系: キャッシュ動作 ----

TEST_F(ImageLoaderTest, CacheHitsUntilClearCache)
{
    ASSERT_TRUE(CreateTestImage(L"clear.png", GUID_ContainerFormatPng, 40, 40));
    auto path = GetTestImagePath(L"clear.png");

    LoadedImage entry1;
    EXPECT_TRUE(LoadAndGet(path, entry1));
    auto* first_bitmap = entry1.bitmap.Get();

    // 消す前は同じビットマップをキャッシュから返す
    LoadedImage hit;
    EXPECT_TRUE(GetCachedImage(loader_, path, hit));
    EXPECT_EQ(hit.bitmap.Get(), first_bitmap);

    loader_.ClearCache();

    LoadedImage entry2;
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
        WorkerGate gate(scheduler_, 2);
        loader_.RequestLoadAsync(path, 0.0f);
        loader_.DropQueued();
        loader_.RequestLoadAsync(path, 0.0f);
        gate.Open();
        EXPECT_TRUE(DrainDecodes());
        EXPECT_EQ(loader_.CacheSize(), 0u);
    }

    LoadedImage entry;
    EXPECT_TRUE(LoadAndGet(path, entry));
}

// 文書の切り替えで捨てた要求の結果は、要求し直されていなければ読み直させない。
TEST_F(ImageLoaderTest, DropQueuedWithoutRerequestDoesNotAskToRequestAgain)
{
    ASSERT_TRUE(CreateTestImage(L"drop.png", GUID_ContainerFormatPng, 40, 40));

    WorkerGate gate(scheduler_, 2);
    loader_.RequestLoadAsync(GetTestImagePath(L"drop.png"), 0.0f);
    loader_.DropQueued();
    gate.Open();

    // Shutdown はキュー済みタスクを実行し切ってから戻るので、捨てた要求の結果が届いている。
    scheduler_.Shutdown();
    EXPECT_FALSE(loader_.ProcessCompletedDecodes());
    EXPECT_EQ(loader_.CacheSize(), 0u);
}

// ---- 異常系: ファイルが存在しない ----

TEST_F(ImageLoaderTest, EmptyPathReturnsFalse)
{
    LoadedImage entry;
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
        LoadedImage entry;
        EXPECT_FALSE(LoadAndGet(path.wstring(), entry));
        EXPECT_FALSE(entry.bitmap);
    }
}

// ---- 異常系: 未初期化の状態 ----

TEST_F(ImageLoaderTest, NullRenderTargetReturnsFalse)
{
    ImageLoader loader_null;
    loader_null.Init(wic_factory_.Get(), nullptr, 0, scheduler_);
    ASSERT_TRUE(CreateTestImage(L"valid.png", GUID_ContainerFormatPng, 10, 10));

    LoadedImage entry;
    EXPECT_FALSE(LoadAndGet(loader_null, GetTestImagePath(L"valid.png"), entry));
}

// ---- Shutdown テスト ----

TEST_F(ImageLoaderTest, ShutdownWithoutInitIsNoOp)
{
    ImageLoader loader;
    loader.SetRenderTarget(render_target_.Get());
    loader.Shutdown(); // Init 未呼び出しでもクラッシュしないこと
}

// ---- ファイルロック回避テスト ----

TEST_F(ImageLoaderTest, FileNotLockedAfterFailedLoad)
{
    // 壊れた画像でも読み込み後にファイルがロックされないこと
    const auto path = temp_dir_.Write(L"bad_lock.png", std::string_view("\x89PNG\r\n\x1a\n\x00\x00", 10));

    LoadedImage entry;
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
        LoadedImage loaded;
        ASSERT_TRUE(LoadAndGet(loader, path, loaded));
        EXPECT_NEAR(loaded.width, c.dip_width, tolerance);
        EXPECT_NEAR(loaded.height, c.dip_height, tolerance);

        LoadedImage cached;
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
    // path の非同期ロードを 1 件ずつ完了させながら times 回失敗させる。
    bool FailLoads(const std::wstring& path, int times)
    {
        for (int i = 0; i < times; ++i) {
            loader_.RequestLoadAsync(path, 0.0f);
            if (!DrainDecodes()) {
                return false;
            }
        }
        return true;
    }
};

TEST_F(ImageLoaderAsyncTest, DuplicateRequestIsIgnored)
{
    ASSERT_TRUE(CreateTestImage(L"dup.png", GUID_ContainerFormatPng, 50, 50));
    auto path = GetTestImagePath(L"dup.png");

    // 同じパスを2回リクエスト — 重複は無視される
    loader_.RequestLoadAsync(path, 0.0f);
    loader_.RequestLoadAsync(path, 0.0f);

    ASSERT_TRUE(DrainScheduler(scheduler_));
    // 予算が無くても 1 件は取り込むので、重複のデコードが走っていれば次の結果が残る。
    EXPECT_TRUE(loader_.ProcessCompletedDecodes(kExpiredDeadline));
    EXPECT_FALSE(loader_.HasLeftoverUploads()) << "結果は 1 件だけ届く";
    EXPECT_EQ(loader_.CacheSize(), 1u);
}

TEST_F(ImageLoaderAsyncTest, FileNotLockedAfterAsyncLoad)
{
    ASSERT_TRUE(CreateTestImage(L"async_lock.png", GUID_ContainerFormatPng, 80, 60));
    auto path = GetTestImagePath(L"async_lock.png");

    LoadedImage entry;
    ASSERT_TRUE(LoadAndGet(path, entry)) << "非同期読み込みが完了しなかった";
    EXPECT_TRUE(CanOpenExclusively(path)) << "非同期読み込み後にファイルが書き込みロックされている";
}

TEST_F(ImageLoaderAsyncTest, CancelPendingClearsQueue)
{
    ASSERT_TRUE(CreateTestImage(L"cancel.png", GUID_ContainerFormatPng, 30, 30));
    auto path = GetTestImagePath(L"cancel.png");

    // 空でも取り消せる
    loader_.CancelPending();

    // 要求をキュー待ちのまま取り消す。
    WorkerGate gate(scheduler_, 2);
    loader_.RequestLoadAsync(path, 0.0f);
    loader_.CancelPending();
    gate.Open();

    // Shutdown はキュー済みタスクを実行し切ってから戻るので、以降に結果が届くことはない
    scheduler_.Shutdown();
    EXPECT_FALSE(loader_.ProcessCompletedDecodes());
    EXPECT_EQ(loader_.CacheSize(), 0u);
}

// デバイスの作り直しで旧 rt から新 rt へ直に差し替えても、受け箱の結果を捨てずに新しい rt に載せる。
TEST_F(ImageLoaderAsyncTest, RenderTargetSwapKeepsCompletedDecodes)
{
    ASSERT_TRUE(CreateTestImage(L"swap.png", GUID_ContainerFormatPng, 40, 30));
    const auto path = GetTestImagePath(L"swap.png");
    loader_.RequestLoadAsync(path, 0.0f);
    ASSERT_TRUE(DrainScheduler(scheduler_));

    const auto rt = CreateRenderTarget();
    ASSERT_TRUE(rt);
    loader_.SetRenderTarget(rt.Get());
    EXPECT_TRUE(loader_.ProcessCompletedDecodes(kNoDeadline));
    LoadedImage entry;
    EXPECT_TRUE(GetCachedImage(loader_, path, entry));
    loader_.SetRenderTarget(render_target_.Get());
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

    ASSERT_TRUE(DrainDecodes()) << "非同期読み込みが完了しなかった";
    EXPECT_EQ(loader_.CacheSize(), paths.size());

    for (int i = 0; i < kImageCount; ++i) {
        LoadedImage entry;
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
    EXPECT_TRUE(loader_.ProcessCompletedDecodes(kExpiredDeadline));
    EXPECT_EQ(loader_.CacheSize(), 1u);
    EXPECT_TRUE(loader_.HasLeftoverUploads());

    loader_.ProcessCompletedDecodes(kNoDeadline);
    EXPECT_EQ(loader_.CacheSize(), static_cast<size_t>(kImageCount));
    EXPECT_FALSE(loader_.HasLeftoverUploads());
}

TEST_F(ImageLoaderAsyncTest, LoadAfterShutdownWithHeavyLoad)
{
    // 大量のリクエスト中に Shutdown してもデッドロック・クラッシュせず、その後も同じローダーで読み込めること
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

    LoadedImage entry;
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

    // キャンセル前の要求の結果も一緒に届くので、対象画像のキャッシュで確かめる
    ASSERT_TRUE(DrainDecodes());
    LoadedImage entry;
    ASSERT_TRUE(GetCachedImage(loader_, path, entry)) << "キャンセル後のリクエストが完了しなかった";
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

    LoadedImage entry;
    EXPECT_FALSE(GetCachedImage(loader_, path, entry));
}

// 失敗の記録を消す操作の後は、同じパスを読み直せる。
TEST_F(ImageLoaderAsyncTest, ClearingFailedPathsAllowsRetry)
{
    struct Case {
        std::wstring_view filename;
        void (ImageLoader::*clear)();
    };
    const Case kCases[] = {
        { L"fail_cancel.png", &ImageLoader::CancelPending },
        { L"fail_clear_cache.png", &ImageLoader::ClearCache },
        { L"fail_reset.png", &ImageLoader::ResetFailedPaths },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ c.filename }));
        const auto path = GetTestImagePath(c.filename);
        ASSERT_TRUE(FailLoads(path, 3));

        (loader_.*c.clear)();

        ASSERT_TRUE(CreateTestImage(c.filename, GUID_ContainerFormatPng, 40, 30));
        LoadedImage entry;
        ASSERT_TRUE(LoadAndGet(path, entry)) << "記録を消した後のリトライが完了しなかった";
        EXPECT_FLOAT_EQ(entry.width, 40.0f);
        EXPECT_FLOAT_EQ(entry.height, 30.0f);
    }
}

TEST_F(ImageLoaderAsyncTest, TransientFailureRetries)
{
    const auto path = GetTestImagePath(L"transient.png");

    ASSERT_TRUE(FailLoads(path, 1));

    ASSERT_TRUE(CreateTestImage(L"transient.png", GUID_ContainerFormatPng, 70, 50));

    LoadedImage entry;
    ASSERT_TRUE(LoadAndGet(path, entry)) << "一時障害後のリトライが成功しなかった";
    EXPECT_FLOAT_EQ(entry.width, 70.0f);
    EXPECT_FLOAT_EQ(entry.height, 50.0f);
}

TEST_F(ImageLoaderAsyncTest, DecodesUpToQuantizedDisplayWidth)
{
    // 見るのは幅だけなので、作って縮める手間を減らすために低くする。
    ASSERT_TRUE(CreateTestImage(L"wide.png", GUID_ContainerFormatPng, 1000, 20));
    const auto path = GetTestImagePath(L"wide.png");

    // 300 DIP (96 DPI で 300px) は 256px 単位に切り上げて 512px でデコードする。
    loader_.RequestLoadAsync(path, 300.0f);
    ASSERT_TRUE(DrainDecodes());
    const auto* cached = loader_.FindCachedImage(path);
    ASSERT_NE(cached, nullptr);
    ASSERT_TRUE(cached->bitmap);
    EXPECT_EQ(cached->bitmap->GetPixelSize().width, 512u);
    // レイアウトは原寸基準のまま。
    EXPECT_FLOAT_EQ(cached->pixel_width, 1000.0f);
    EXPECT_FLOAT_EQ(cached->pixel_height, 20.0f);
    EXPECT_EQ(cached->sharp_width_px, 512u);
    EXPECT_FALSE(loader_.NeedsSharperDecode(*cached, 500.0f));
    EXPECT_TRUE(loader_.NeedsSharperDecode(*cached, 600.0f));

    // 表示幅が広がったら、高い解像度で読み直してキャッシュを差し替える。
    loader_.RequestLoadAsync(path, 600.0f);
    ASSERT_TRUE(DrainDecodes());
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
    ASSERT_TRUE(DrainDecodes());
    const auto* cached = loader_.FindCachedImage(path);
    ASSERT_NE(cached, nullptr);
    EXPECT_EQ(cached->bitmap->GetPixelSize().width, 100u);
    EXPECT_FALSE(loader_.NeedsSharperDecode(*cached, 5000.0f));
}

// ============================================================
// デコードサイズ計算
// ============================================================

TEST(ComputeDecodeSize, KeepsAspectWithinLimits)
{
    struct Case {
        UINT width;
        UINT height;
        UINT max_width;
        UINT max_dim;
        wic_util::PixelSize expected;
    };
    constexpr Case kCases[] = {
        { 800, 600, 1920, 16384, { 800, 600 } },
        { 6000, 4000, 1920, 16384, { 1920, 1280 } },
        { 1000, 40000, 1920, 16384, { 410, 16384 } },
        // 0 は制限なし
        { 6000, 4000, 0, 0, { 6000, 4000 } },
        // 辺が 0 にならない
        { 100000, 1, 1000, 0, { 1000, 1 } },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.width);
        EXPECT_EQ(wic_util::ComputeDecodeSize(c.width, c.height, c.max_width, c.max_dim), c.expected);
    }
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

// ============================================================
// ImageLoader キャッシュテスト
// ============================================================

class ImageLoaderCacheTest : public ::testing::Test {
protected:
    // FindCachedImage / InsertCacheEntryForTest だけを使うので Init は不要。
    ImageLoader loader_;
};

// 並べ替えの規則は TranspositionCache のテストで見る。ここでは容量が MAX_CACHE_ENTRIES (128) であることと、
// FindCachedImage のヒットで順位が上がることだけを確かめる。
TEST_F(ImageLoaderCacheTest, HoldsMaxEntriesAndFindPromotes)
{
    constexpr size_t kMaxEntries = 128;
    EXPECT_EQ(loader_.FindCachedImage(L"img_0.png"), nullptr);
    for (size_t i = 0; i < kMaxEntries; i++) {
        loader_.InsertCacheEntryForTest(L"img_" + std::to_wstring(i) + L".png", 100.0f, 100.0f);
    }
    EXPECT_EQ(loader_.CacheSize(), kMaxEntries);

    // 最も古い img_0 はヒットで末尾から外れるので、溢れたときは 1 つ前の img_1 が捨てられる
    ASSERT_NE(loader_.FindCachedImage(L"img_0.png"), nullptr);
    loader_.InsertCacheEntryForTest(L"overflow.png", 100.0f, 100.0f);
    EXPECT_EQ(loader_.CacheSize(), kMaxEntries);
    EXPECT_NE(loader_.FindCachedImage(L"img_0.png"), nullptr);
    EXPECT_EQ(loader_.FindCachedImage(L"img_1.png"), nullptr);
    EXPECT_NE(loader_.FindCachedImage(L"overflow.png"), nullptr);
}

TEST_F(ImageLoaderCacheTest, ClearCacheRemovesAllEntries)
{
    loader_.InsertCacheEntryForTest(L"a.png", 100.0f, 100.0f);
    loader_.InsertCacheEntryForTest(L"b.png", 100.0f, 100.0f);
    loader_.ClearCache();
    EXPECT_EQ(loader_.CacheSize(), 0u);
    EXPECT_EQ(loader_.FindCachedImage(L"a.png"), nullptr);
    EXPECT_EQ(loader_.FindCachedImage(L"b.png"), nullptr);
}
