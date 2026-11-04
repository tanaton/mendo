#include <gtest/gtest.h>
#include "diagram_raster.h"
#include "test_helpers.h"
#include "wic_util.h"
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

class DiagramRasterTest : public ComApartmentTest {
protected:
    void SetUp() override
    {
        ASSERT_HRESULT_SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, d2d_.GetAddressOf()));
        ASSERT_HRESULT_SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw_.GetAddressOf())));
        wic_ = wic_util::CreateWicFactory("DiagramRasterTest");
        ASSERT_NE(wic_, nullptr);
    }

    ComPtr<IWICBitmap> Rasterize(const mermaid_util::DiagramDrawing& drawing, UINT width, UINT height)
    {
        return RasterizeDiagram(d2d_.Get(), dw_.Get(), wic_.Get(), drawing, width, height, L"ja-jp");
    }

    // 32bpp PBGRA の 1 画素を 0xAARRGGBB で返す。
    static uint32_t Pixel(IWICBitmap* bitmap, UINT x, UINT y)
    {
        UINT w = 0;
        UINT h = 0;
        bitmap->GetSize(&w, &h);
        std::vector<uint8_t> bytes(size_t{ w } * h * 4);
        bitmap->CopyPixels(nullptr, w * 4, static_cast<UINT>(bytes.size()), bytes.data());
        const uint8_t* p = &bytes[(size_t{ y } * w + x) * 4];
        return (uint32_t{ p[3] } << 24) | (uint32_t{ p[2] } << 16) | (uint32_t{ p[1] } << 8) | p[0];
    }

    // 範囲の中で背景と違う画素の数。
    static int InkPixels(IWICBitmap* bitmap, UINT x0, UINT y0, UINT x1, UINT y1, uint32_t background)
    {
        int count = 0;
        for (UINT y = y0; y < y1; ++y) {
            for (UINT x = x0; x < x1; ++x) {
                count += Pixel(bitmap, x, y) != background;
            }
        }
        return count;
    }

    static mermaid_util::DrawingText Text(std::wstring text, float x, float y)
    {
        return { .x = x, .y = y, .font_size = 16.0f, .family = L"\"Segoe UI\", \"Yu Gothic UI\"", .text = std::move(text) };
    }

    ComPtr<ID2D1Factory1> d2d_;
    ComPtr<IDWriteFactory> dw_;
    ComPtr<IWICImagingFactory> wic_;
};

// 図形は SVG の属性どおりに、CSS px を dpr 倍した画素へ描く。背景は不透明に塗る。
TEST_F(DiagramRasterTest, DrawsShapesScaledByDprOnOpaqueBackground)
{
    const mermaid_util::DiagramDrawing drawing{
        .size = { 20.0f, 10.0f, 2.0f },
        .background = 0x1E1E1E,
        .layers = { { .shapes_svg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="10" viewBox="0 0 20 10">)svg"
                                    R"svg(<rect transform="matrix(1 0 0 1 10 0)" x="0" y="0" width="10" height="10" fill="#ff0000"/></svg>)svg" } },
    };
    const auto bitmap = Rasterize(drawing, 40, 20);
    ASSERT_NE(bitmap, nullptr);
    EXPECT_EQ(Pixel(bitmap.Get(), 5, 5), 0xFF1E1E1Eu);
    // CSS px の x=10〜20 は物理 px の 20〜40
    EXPECT_EQ(Pixel(bitmap.Get(), 19, 10), 0xFF1E1E1Eu);
    EXPECT_EQ(Pixel(bitmap.Get(), 21, 10), 0xFFFF0000u);
    EXPECT_EQ(Pixel(bitmap.Get(), 39, 19), 0xFFFF0000u);
}

// 文字はベースラインの始点に置く。変換で平行移動した先に描かれる。
TEST_F(DiagramRasterTest, DrawsTextAtBaselineWithTransform)
{
    auto text = Text(L"日本語 ABC", 0.0f, 16.0f);
    text.transform = { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 20.0f };
    const mermaid_util::DiagramDrawing drawing{
        .size = { 100.0f, 40.0f, 1.0f },
        .background = 0xFFFFFF,
        .layers = { { .texts = { text } } },
    };
    const auto bitmap = Rasterize(drawing, 100, 40);
    ASSERT_NE(bitmap, nullptr);
    // 変換で 20px 下げたので上半分には何も無く、ベースライン (y=36) の上に字がある
    EXPECT_EQ(InkPixels(bitmap.Get(), 0, 0, 100, 18, 0xFFFFFFFFu), 0);
    EXPECT_GT(InkPixels(bitmap.Get(), 0, 22, 100, 37, 0xFFFFFFFFu), 50);
}

// 後の層の図形は前の層の文字の上に描く (SVG の文書の順の重なり)。
TEST_F(DiagramRasterTest, LaterLayerShapesCoverEarlierText)
{
    const mermaid_util::DiagramDrawing drawing{
        .size = { 100.0f, 40.0f, 1.0f },
        .background = 0xFFFFFF,
        .layers = {
            { .texts = { Text(L"ABCDEF", 0.0f, 30.0f) } },
            { .shapes_svg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="40" viewBox="0 0 100 40">)svg"
                            R"svg(<rect x="0" y="0" width="100" height="40" fill="#00ff00"/></svg>)svg" },
        },
    };
    const auto bitmap = Rasterize(drawing, 100, 40);
    ASSERT_NE(bitmap, nullptr);
    EXPECT_EQ(InkPixels(bitmap.Get(), 0, 0, 100, 40, 0xFF00FF00u), 0);
}

// 壊れた SVG は描けない (キャプチャで描き直させるため null を返す)。
TEST_F(DiagramRasterTest, ReturnsNullForMalformedSvg)
{
    const mermaid_util::DiagramDrawing drawing{
        .size = { 10.0f, 10.0f, 1.0f },
        .layers = { { .shapes_svg = "<svg" } },
    };
    EXPECT_EQ(Rasterize(drawing, 10, 10), nullptr);
}

TEST_F(DiagramRasterTest, ResolvesFirstInstalledFamily)
{
    ComPtr<IDWriteFontCollection> fonts;
    ASSERT_HRESULT_SUCCEEDED(dw_->GetSystemFontCollection(&fonts));
    EXPECT_EQ(ResolveFontFamily(fonts.Get(), L"\"No Such Font 12345\", 'Arial', sans-serif"), L"Arial");
    EXPECT_EQ(ResolveFontFamily(fonts.Get(), L"No Such Font 12345, monospace"), L"Consolas");
    EXPECT_EQ(ResolveFontFamily(fonts.Get(), L" sans-serif "), L"Segoe UI");
    EXPECT_EQ(ResolveFontFamily(fonts.Get(), L""), L"Segoe UI");
    EXPECT_EQ(ResolveFontFamily(fonts.Get(), L"No Such Font 12345"), L"Segoe UI");
}
