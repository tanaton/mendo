#include <gtest/gtest.h>
#include "diagram_raster.h"
#include "layout_cache.h"
#include "test_helpers.h"
#include "theme_palette.h"
#include "wic_util.h"
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

class DiagramRasterTest : public ComApartmentTest {
protected:
    void SetUp() override
    {
        ASSERT_HRESULT_SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, d2d_.GetAddressOf()));
        dw_ = SharedDWriteFactory();
        ASSERT_NE(dw_, nullptr);
        wic_ = wic_util::CreateWicFactory("DiagramRasterTest");
        ASSERT_NE(wic_, nullptr);
    }

    ComPtr<IWICBitmap> Rasterize(const mermaid_util::DiagramDrawing& drawing, float dpr = 1.0f)
    {
        return RasterizeDiagram(d2d_.Get(), dw_.Get(), wic_.Get(), drawing, dpr, L"ja-jp");
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
        return { .at = { .x = x, .y = y, .font_size = 16.0f }, .family = L"\"Segoe UI\", \"Yu Gothic UI\"", .text = std::move(text) };
    }

    ComPtr<ID2D1Factory1> d2d_;
    ComPtr<IDWriteFactory> dw_;
    ComPtr<IWICImagingFactory> wic_;
};

// 図形は SVG の属性どおりに、CSS px を dpr 倍した画素へ描く。背景は不透明に塗る。
TEST_F(DiagramRasterTest, DrawsShapesScaledByDprOnOpaqueBackground)
{
    const mermaid_util::DiagramDrawing drawing{
        .width = 20.0f,
        .height = 10.0f,
        .background = 0x1E1E1E,
        .layers = { { .shapes = R"svg(<rect transform="translate(10 0)" x="0" y="0" width="10" height="10" fill="#ff0000"/>)svg" } },
    };
    const auto bitmap = Rasterize(drawing, 2.0f);
    ASSERT_NE(bitmap, nullptr);
    EXPECT_EQ(Pixel(bitmap.Get(), 5, 5), 0xFF1E1E1Eu);
    // CSS px の x=10〜20 は物理 px の 20〜40
    EXPECT_EQ(Pixel(bitmap.Get(), 19, 10), 0xFF1E1E1Eu);
    EXPECT_EQ(Pixel(bitmap.Get(), 21, 10), 0xFFFF0000u);
    EXPECT_EQ(Pixel(bitmap.Get(), 39, 19), 0xFFFF0000u);
}

// 層の図形は図に 1 つの <defs> を参照できる。
TEST_F(DiagramRasterTest, DrawsLayersWithSharedDefs)
{
    const mermaid_util::DiagramDrawing drawing{
        .width = 20.0f,
        .height = 10.0f,
        .background = 0xFFFFFF,
        .defs = R"svg(<linearGradient id="g0"><stop offset="0" stop-color="#00ff00"/></linearGradient>)svg",
        .layers = {
            { .texts = { Text(L"A", 0.0f, 8.0f) } },
            { .shapes = R"svg(<rect x="0" y="0" width="20" height="10" fill="url(#g0)"/>)svg" },
        },
    };
    const auto bitmap = Rasterize(drawing);
    ASSERT_NE(bitmap, nullptr);
    EXPECT_EQ(InkPixels(bitmap.Get(), 0, 0, 20, 10, 0xFF00FF00u), 0);
}

// 文字はベースラインの始点に置く。変換で平行移動した先に描かれる。
TEST_F(DiagramRasterTest, DrawsTextAtBaselineWithTransform)
{
    auto text = Text(L"日本語 ABC", 0.0f, 16.0f);
    text.at.transform = { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 20.0f };
    const mermaid_util::DiagramDrawing drawing{
        .width = 100.0f,
        .height = 40.0f,
        .background = 0xFFFFFF,
        .layers = { { .texts = { text } } },
    };
    const auto bitmap = Rasterize(drawing);
    ASSERT_NE(bitmap, nullptr);
    // 変換で 20px 下げたので上半分には何も無く、ベースライン (y=36) の上に字がある
    EXPECT_EQ(InkPixels(bitmap.Get(), 0, 0, 100, 18, 0xFFFFFFFFu), 0);
    EXPECT_GT(InkPixels(bitmap.Get(), 0, 22, 100, 37, 0xFFFFFFFFu), 50);
}

// 後の層の図形は前の層の文字の上に描く (SVG の文書の順の重なり)。
TEST_F(DiagramRasterTest, LaterLayerShapesCoverEarlierText)
{
    const mermaid_util::DiagramDrawing drawing{
        .width = 100.0f,
        .height = 40.0f,
        .background = 0xFFFFFF,
        .layers = {
            { .texts = { Text(L"ABCDEF", 0.0f, 30.0f) } },
            { .shapes = R"svg(<rect x="0" y="0" width="100" height="40" fill="#00ff00"/>)svg" },
        },
    };
    const auto bitmap = Rasterize(drawing);
    ASSERT_NE(bitmap, nullptr);
    EXPECT_EQ(InkPixels(bitmap.Get(), 0, 0, 100, 40, 0xFF00FF00u), 0);
}

// 壊れた SVG は描けない (null を返し、描けない図として知らせる)。
TEST_F(DiagramRasterTest, ReturnsNullForMalformedSvg)
{
    const mermaid_util::DiagramDrawing drawing{
        .width = 10.0f,
        .height = 10.0f,
        .layers = { { .shapes = "<rect" } },
    };
    EXPECT_EQ(Rasterize(drawing), nullptr);
}

// クリップボードの SVG は <defs> を 1 つにし、層の図形と文字を順に並べる。初期値と同じ属性は書かない。
TEST_F(DiagramRasterTest, WritesDrawingAsSingleSvg)
{
    const mermaid_util::DiagramDrawing drawing{
        .width = 20.0f,
        .height = 10.0f,
        .defs = R"svg(<linearGradient id="g0"/>)svg",
        .layers = {
            { .shapes = R"svg(<rect width="1" height="1"/>)svg", .texts = { Text(L"A<", 1.0f, 2.0f) } },
            { .shapes = R"svg(<circle r="1"/>)svg" },
        },
    };
    EXPECT_EQ(DrawingToSvg(drawing),
              R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="10" viewBox="0 0 20 10"><defs><linearGradient id="g0"/></defs>)svg"
              R"svg(<rect width="1" height="1"/><text x="1" y="2" font-family="&quot;Segoe UI&quot;, &quot;Yu Gothic UI&quot;" font-size="16")svg"
              R"svg( fill="#000000" xml:space="preserve">A&lt;</text><circle r="1"/></svg>)svg");
}

// 入っている候補を順に、重なりを除いて返す (先頭で組み、残りをフォールバック先にする)。
TEST_F(DiagramRasterTest, ResolvesInstalledFamiliesInOrder)
{
    using Families = std::vector<std::wstring>;
    ComPtr<IDWriteFontCollection> fonts;
    ASSERT_HRESULT_SUCCEEDED(dw_->GetSystemFontCollection(&fonts));
    EXPECT_EQ(ResolveFontFamilies(fonts.Get(), L"\"No Such Font 12345\", 'Arial', sans-serif"), (Families{ L"Arial", L"Segoe UI" }));
    EXPECT_EQ(ResolveFontFamilies(fonts.Get(), L"\"Segoe UI\", sans-serif, \"Yu Gothic UI\""), (Families{ L"Segoe UI", L"Yu Gothic UI" }));
    // 等幅は本文と同じく、候補のうち入っている最初のもの
    const auto mono = std::ranges::find_if(theme_palette::kMonospaceFontCandidates, [&](const wchar_t* name) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        return SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists;
    });
    ASSERT_NE(mono, std::end(theme_palette::kMonospaceFontCandidates));
    EXPECT_EQ(ResolveFontFamilies(fonts.Get(), L"No Such Font 12345, monospace"), Families{ *mono });
    EXPECT_EQ(ResolveFontFamilies(fonts.Get(), L" sans-serif "), Families{ L"Segoe UI" });
    EXPECT_TRUE(ResolveFontFamilies(fonts.Get(), L"").empty());
    EXPECT_TRUE(ResolveFontFamilies(fonts.Get(), L"No Such Font 12345").empty());
}

// 描画命令の図はビットマップを持たないので、画面外へ出ても追い出さない (見えたときに記録し直さない)。
// デバイスロストでは旧デバイスのコマンドリストを捨てる。
TEST_F(DiagramRasterTest, VectorDiagramSurvivesOffscreenEvictionButNotDeviceLoss)
{
    ComPtr<IWICBitmap> bitmap;
    ASSERT_HRESULT_SUCCEEDED(wic_->CreateBitmap(4, 4, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap));
    ComPtr<ID2D1RenderTarget> rt;
    ASSERT_HRESULT_SUCCEEDED(d2d_->CreateWicBitmapRenderTarget(bitmap.Get(), D2D1::RenderTargetProperties(), &rt));
    ComPtr<ID2D1DeviceContext> dc;
    ASSERT_HRESULT_SUCCEEDED(rt.As(&dc));
    ComPtr<ID2D1CommandList> list;
    ASSERT_HRESULT_SUCCEEDED(dc->CreateCommandList(&list));

    DiagramEntry entry;
    entry.commands = list;
    entry.width = 10.0f;
    entry.height = 10.0f;
    entry.drawing = std::make_shared<const mermaid_util::DiagramDrawing>();
    EXPECT_TRUE(entry.HasImage());
    EXPECT_TRUE(entry.CanExport());
    EXPECT_FALSE(entry.NeedsRender());

    entry.EvictBitmap();
    EXPECT_TRUE(entry.HasImage());
    EXPECT_TRUE(entry.CanExport());

    entry.EvictForRetry();
    EXPECT_FALSE(entry.HasImage());
    EXPECT_FALSE(entry.CanExport());
    EXPECT_TRUE(entry.NeedsRender());
    EXPECT_EQ(entry.width, 10.0f);
}

// ---- pele の SVG の解決 ----

class PeleSvgTest : public DiagramRasterTest {
protected:
    void SetUp() override
    {
        DiagramRasterTest::SetUp();
        measurer_.emplace(dw_.Get(), L"ja-jp");
    }

    std::optional<mermaid_util::DiagramDrawing> Resolve(std::string_view body, bool dark = false, float max_width = 800.0f)
    {
        std::string svg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 100" width="200" height="100")svg"
                          R"svg( style="max-width:100%;height:auto;--_bg:var(--pele-bg,#fff);--_fg:var(--pele-fg,#1f1f1f);)svg"
                          R"svg(--_s:var(--pele-surface,#f3f3f3);--_tw:var(--pele-title-weight,bold);color:var(--_fg);font:16px sans-serif;)svg"
                          R"svg(font-family:var(--pele-font,&quot;Segoe UI&quot;, &quot;Yu Gothic UI&quot;)" fill="var(--_fg)">)svg";
        svg += body;
        svg += "</svg>";
        return DrawingFromPeleSvg(svg, dark, max_width, *measurer_);
    }

    float Width(std::wstring_view text, float size = 16.0f)
    {
        return measurer_->Width(text, { .family = L"\"Segoe UI\", \"Yu Gothic UI\"", .size = size });
    }

    std::optional<DiagramTextMeasurer> measurer_;
};

// CSS 変数 (入れ子と既定値)・color-mix・ダークの配色を解いて、図形の属性に色を確定させる。
TEST_F(PeleSvgTest, ResolvesVariablesAndColorMix)
{
    const std::string_view body =
        R"svg(<rect x="0" y="0" width="10" height="10" fill="var(--_s)" stroke="color-mix(in srgb,#000 50%,#fff)" style="rx:4px"/>)svg";
    const auto light = Resolve(body);
    ASSERT_TRUE(light);
    EXPECT_EQ(light->background, 0xFFFFFFu);
    ASSERT_EQ(light->layers.size(), 1u);
    const auto& shapes = light->layers[0].shapes;
    // 不透明度 1・線の太さ 1 などの初期値は書かない。
    EXPECT_NE(shapes.find(R"svg(fill="#f3f3f3" stroke="#808080"/>)svg"), std::string::npos) << shapes;
    EXPECT_NE(shapes.find(R"svg(rx="4" ry="4")svg"), std::string::npos) << shapes;
    const auto dark = Resolve(body, true);
    ASSERT_TRUE(dark);
    EXPECT_EQ(dark->background, 0x1E1E1Eu);
    EXPECT_NE(dark->layers[0].shapes.find(R"svg(fill="#2b2b2b")svg"), std::string::npos);
}

// 文字は text-anchor の分だけ塊の幅で戻す。tspan は前の文字の後ろに続き、下付きは小さくして下げる。
TEST_F(PeleSvgTest, PlacesTextChunksByAnchor)
{
    const auto drawing = Resolve(R"svg(<g transform="translate(100,50)"><text text-anchor="middle" xml:space="preserve">)svg"
                                 R"svg(<tspan x="0" y="5">H</tspan><tspan baseline-shift="sub" font-size="smaller">2</tspan><tspan>O</tspan></text>)svg"
                                 R"svg(<text x="10" y="40" font-weight="var(--_tw)" fill="var(--_bg)">B</text></g>)svg");
    ASSERT_TRUE(drawing);
    const auto& texts = drawing->layers.back().texts;
    ASSERT_EQ(texts.size(), 4u);
    const float h = Width(L"H");
    const float two = Width(L"2", 16.0f / 1.2f);
    const float o = Width(L"O");
    const float left = -(h + two + o) / 2.0f;
    EXPECT_NEAR(texts[0].at.x, left, 0.01f);
    EXPECT_NEAR(texts[1].at.x, left + h, 0.01f);
    EXPECT_NEAR(texts[2].at.x, left + h + two, 0.01f);
    EXPECT_EQ(texts[0].at.y, 5.0f);
    EXPECT_GT(texts[1].at.y, 5.0f);
    EXPECT_EQ(texts[2].at.y, 5.0f);
    EXPECT_NEAR(texts[1].at.font_size, 16.0f / 1.2f, 1e-4f);
    EXPECT_EQ(texts[0].at.rgb, 0x1F1F1Fu);
    EXPECT_EQ(texts[0].family, L"\"Segoe UI\", \"Yu Gothic UI\"");
    EXPECT_EQ(texts[0].at.transform[4], 100.0f);
    EXPECT_EQ(texts[0].at.transform[5], 50.0f);
    EXPECT_EQ(texts[3].at.x, 10.0f);
    EXPECT_EQ(texts[3].weight, 700);
    EXPECT_EQ(texts[3].at.rgb, 0xFFFFFFu);
}

// xml:space が無ければ空白を畳み、先頭と末尾の空白を除く。
TEST_F(PeleSvgTest, CollapsesWhitespace)
{
    const auto drawing = Resolve("<text x=\"0\" y=\"20\">  a \n\t b  </text>");
    ASSERT_TRUE(drawing);
    ASSERT_EQ(drawing->layers[0].texts.size(), 1u);
    EXPECT_EQ(drawing->layers[0].texts[0].text, L"a b");
}

// 図形が前の文字に重なるときだけ次の層にする。
TEST_F(PeleSvgTest, StartsLayerOnlyWhenShapeCoversText)
{
    const auto apart = Resolve(R"svg(<text x="0" y="20">AB</text><rect x="100" y="50" width="10" height="10"/>)svg");
    ASSERT_TRUE(apart);
    EXPECT_EQ(apart->layers.size(), 1u);
    const auto over = Resolve(R"svg(<text x="0" y="20">AB</text><rect x="0" y="0" width="30" height="30"/>)svg");
    ASSERT_TRUE(over);
    ASSERT_EQ(over->layers.size(), 2u);
    EXPECT_EQ(over->layers[0].texts.size(), 1u);
    EXPECT_FALSE(over->layers[1].shapes.empty());
}

// ページと同じく max-width:100% で幅の上限まで縮める。
TEST_F(PeleSvgTest, ShrinksToMaxWidth)
{
    const auto drawing = Resolve(R"svg(<rect x="0" y="0" width="10" height="10"/>)svg", false, 100.0f);
    ASSERT_TRUE(drawing);
    EXPECT_EQ(drawing->width, 100.0f);
    EXPECT_EQ(drawing->height, 50.0f);
    EXPECT_NE(drawing->layers[0].shapes.find(R"svg(transform="matrix(0.5 0 0 0.5 0 0)")svg"), std::string::npos)
        << drawing->layers[0].shapes;
}

// 入れ子の svg (アイコン) は viewBox で縮め、currentColor は継承した color で塗る。
TEST_F(PeleSvgTest, MapsNestedSvgViewport)
{
    const auto drawing = Resolve(
        R"svg(<svg x="10" y="20" width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor"><path d="M0 0L24 24"/></svg>)svg");
    ASSERT_TRUE(drawing);
    const auto& shapes = drawing->layers[0].shapes;
    EXPECT_NE(shapes.find(R"svg(transform="matrix(0.5 0 0 0.5 10 20)" fill="none" stroke="#1f1f1f")svg"), std::string::npos) << shapes;
}

// 平行移動だけの変換は translate にし、単位行列は書かない。線の属性は初期値と違うものだけ書く。
TEST_F(PeleSvgTest, WritesOnlyNonInitialAttributes)
{
    const auto drawing = Resolve(R"svg(<g transform="translate(5,6)"><rect x="0" y="0" width="10" height="10" stroke="red"/></g>)svg"
                                 R"svg(<rect x="0" y="0" width="10" height="10" fill="none" stroke="red" stroke-width="2" stroke-linecap="round")svg"
                                 R"svg( stroke-dasharray="4 2"/>)svg");
    ASSERT_TRUE(drawing);
    ASSERT_EQ(drawing->layers.size(), 1u);
    EXPECT_EQ(drawing->layers[0].shapes,
              R"svg(<rect x="0" y="0" width="10" height="10" transform="translate(5 6)" fill="#1f1f1f" stroke="#ff0000"/>)svg"
              R"svg(<rect x="0" y="0" width="10" height="10" fill="none" stroke="#ff0000" stroke-width="2" stroke-linecap="round")svg"
              R"svg( stroke-dasharray="4 2"/>)svg");
}

// D2D の SVG はグラデーションの塗りの不透明度を無視するので、止まりの不透明度に掛けて書く。
TEST_F(PeleSvgTest, FoldsPaintOpacityIntoGradientStops)
{
    const auto drawing = Resolve(R"svg(<defs><linearGradient id="g" gradientUnits="userSpaceOnUse" x1="0" x2="10">)svg"
                                 R"svg(<stop offset="0" stop-color="var(--_s)"/><stop offset="1" stop-color="red"/></linearGradient></defs>)svg"
                                 R"svg(<path d="M0 0H10V10Z" fill="url(#g) red" fill-opacity="0.5"/>)svg");
    ASSERT_TRUE(drawing);
    const auto& defs = drawing->defs;
    EXPECT_NE(defs.find(R"svg(<linearGradient id="g0" gradientUnits="userSpaceOnUse" x1="0" x2="10">)svg"), std::string::npos) << defs;
    EXPECT_NE(defs.find(R"svg(<stop offset="0" stop-color="#f3f3f3" stop-opacity="0.5"/>)svg"), std::string::npos) << defs;
    const auto& shapes = drawing->layers[0].shapes;
    EXPECT_NE(shapes.find(R"svg(fill="url(#g0)"/>)svg"), std::string::npos) << shapes;
}

// 書き出せない要素 (foreignObject) と壊れた SVG は描画命令にしない (描けない図として知らせる)。
TEST_F(PeleSvgTest, RejectsUnsupportedOrMalformedSvg)
{
    EXPECT_FALSE(Resolve(R"svg(<foreignObject x="0" y="0" width="10" height="10"/>)svg"));
    EXPECT_FALSE(Resolve("<g><rect/>"));
    EXPECT_FALSE(DrawingFromPeleSvg("not svg", false, 800.0f, *measurer_));
}

// pele は小さな箱の角丸を min() で縮める。
TEST_F(PeleSvgTest, ResolvesMinInLengths)
{
    const auto drawing = Resolve(R"svg(<rect x="0" y="0" width="10" height="6" style="rx:min(var(--undefined, 4px),3px)"/>)svg");
    ASSERT_TRUE(drawing);
    EXPECT_NE(drawing->layers[0].shapes.find(R"svg(rx="3" ry="3")svg"), std::string::npos) << drawing->layers[0].shapes;
}
