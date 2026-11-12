#include <gtest/gtest.h>
#include "diagram_raster.h"
#include "layout_cache.h"
#include "mermaid_util.h"
#include "string_convert.h"
#include "test_helpers.h"
#include "theme_service.h"
#include "wic_util.h"
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
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
        return RasterizeDiagram(d2d_.Get(), dw_.Get(), wic_.Get(), drawing, { drawing.width, drawing.height }, dpr, L"ja-jp");
    }

    // 32bpp PBGRA の 1 画素を 0xAARRGGBB で返す。
    static uint32_t Pixel(IWICBitmap* bitmap, UINT x, UINT y)
    {
        const WICRect rect{ static_cast<INT>(x), static_cast<INT>(y), 1, 1 };
        std::array<uint8_t, 4> p{};
        bitmap->CopyPixels(&rect, 4, static_cast<UINT>(p.size()), p.data());
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

// 幅の上限に収まらない図の保存・コピーの画像は、縮めた大きさ x dpr の画素数で、図全体をその大きさに縮めて描く。
TEST_F(DiagramRasterTest, DrawsWholeDrawingShrunkToGivenSize)
{
    const mermaid_util::DiagramDrawing drawing{
        .width = 40.0f,
        .height = 20.0f,
        .background = 0x1E1E1E,
        .layers = { { .shapes = R"svg(<rect x="20" y="0" width="20" height="20" fill="#ff0000"/>)svg" } },
    };
    const auto bitmap = RasterizeDiagram(d2d_.Get(), dw_.Get(), wic_.Get(), drawing, mermaid_util::FittedDiagramSize(drawing, 10.0f), 2.0f, L"ja-jp");
    ASSERT_NE(bitmap, nullptr);
    UINT w = 0;
    UINT h = 0;
    ASSERT_HRESULT_SUCCEEDED(bitmap->GetSize(&w, &h));
    EXPECT_EQ(w, 20u);
    EXPECT_EQ(h, 10u);
    // 図の右半分 (x=20〜40) は物理 px の 10〜20。下端まで背景か図形で埋まる。
    EXPECT_EQ(Pixel(bitmap.Get(), 8, 9), 0xFF1E1E1Eu);
    EXPECT_EQ(Pixel(bitmap.Get(), 11, 0), 0xFFFF0000u);
    EXPECT_EQ(Pixel(bitmap.Get(), 19, 9), 0xFFFF0000u);
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
    // 等幅は本文と同じもの
    ThemeService themes;
    themes.Init(false, {});
    const std::wstring& mono = themes.GetTheme().monospace_font;
    EXPECT_EQ(ResolveFontFamilies(fonts.Get(), L"No Such Font 12345, monospace"), Families{ mono });
    EXPECT_EQ(ResolveFontFamilies(fonts.Get(), L" sans-serif "), Families{ L"Segoe UI" });
    // 総称名は CSS と同じく大文字小文字を区別しない。
    EXPECT_EQ(ResolveFontFamilies(fonts.Get(), L"Sans-Serif, MONOSPACE"), (Families{ L"Segoe UI", mono }));
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

    std::optional<mermaid_util::DiagramDrawing> Resolve(std::string_view body, bool dark = false, std::string_view root_class = "pele")
    {
        std::string svg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" class=")svg";
        svg += root_class;
        svg += R"svg(" viewBox="0 0 200 100" width="200" height="100")svg"
               R"svg( style="max-width:100%;height:auto;--_bg:var(--pele-bg,#fff);--_fg:var(--pele-fg,#1f1f1f);)svg"
               R"svg(--_s:var(--pele-surface,#f3f3f3);--_tw:var(--pele-title-weight,bold);color:var(--_fg);font:16px sans-serif;)svg"
               R"svg(font-family:var(--pele-font,&quot;Segoe UI&quot;, &quot;Yu Gothic UI&quot;)" fill="var(--_fg)">)svg";
        svg += body;
        svg += "</svg>";
        return DrawingFromPeleSvg(svg, dark, *measurer_);
    }

    // 根の <svg> から書いた SVG をそのまま解く。
    std::optional<mermaid_util::DiagramDrawing> ResolveRaw(std::string_view svg)
    {
        return DrawingFromPeleSvg(svg, false, *measurer_);
    }
    // body の図形を書き出した SVG。解けなければ nullopt。
    std::optional<std::string> ShapesOf(std::string_view body)
    {
        const auto drawing = Resolve(body);
        if (!drawing || drawing->layers.empty()) {
            return std::nullopt;
        }
        return drawing->layers[0].shapes;
    }
    // body の文字がちょうど 1 本のときの、その文字。
    std::optional<mermaid_util::DrawingText> OnlyTextOf(std::string_view body)
    {
        const auto drawing = Resolve(body);
        if (!drawing || drawing->layers.size() != 1 || drawing->layers[0].texts.size() != 1) {
            return std::nullopt;
        }
        return drawing->layers[0].texts[0];
    }
    // 図形が前の文字に重なって次の層へ送られたか (図形の範囲は外から見えないので、層の数で確かめる)。
    bool SplitsLayer(std::string_view body)
    {
        const auto drawing = Resolve(body);
        EXPECT_TRUE(drawing) << body;
        return drawing && drawing->layers.size() == 2;
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
    EXPECT_NE(shapes.find(R"svg(fill="#ececff" stroke="#808080"/>)svg"), std::string::npos) << shapes;
    EXPECT_NE(shapes.find(R"svg(rx="4" ry="4")svg"), std::string::npos) << shapes;
    const auto dark = Resolve(body, true);
    ASSERT_TRUE(dark);
    EXPECT_EQ(dark->background, 0x1E1E1Eu);
    EXPECT_NE(dark->layers[0].shapes.find(R"svg(fill="#2b2b2b")svg"), std::string::npos);
}

// ライトでは mermaid の既定のテーマに合わせて、枠 (flowchart と block だけ)・ノート・辺のラベル・活性区間の色を class ごとに差し替える。
// ダークは差し替えない。
TEST_F(PeleSvgTest, LightThemeRecolorsPartsLikeMermaid)
{
    const std::string_view body =
        R"svg(<g class="pele-cluster x"><rect x="0" y="0" width="10" height="10" fill="var(--_a,#e6e6e6)" stroke="var(--_b,#8a8a8a)"/></g>)svg"
        R"svg(<g class="pele-notes"><rect x="0" y="20" width="10" height="10" fill="var(--_a,#e6e6e6)"/></g>)svg"
        R"svg(<g class="pele-edge-label"><rect x="0" y="40" width="10" height="10" fill="var(--_bg)"/></g>)svg"
        R"svg(<g class="pele-activations"><rect x="0" y="60" width="10" height="10" fill="var(--_s)" stroke="var(--_b,#8a8a8a)"/></g>)svg";
    const auto flowchart = Resolve(body, false, "pele pele-flowchart");
    ASSERT_TRUE(flowchart);
    EXPECT_EQ(flowchart->layers[0].shapes,
              R"svg(<rect x="0" y="0" width="10" height="10" fill="#ffffbd" stroke="#aaaa33"/>)svg"
              R"svg(<rect x="0" y="20" width="10" height="10" fill="#fff5ad"/>)svg"
              R"svg(<rect x="0" y="40" width="10" height="10" fill="#e8e8e8" fill-opacity="0.8"/>)svg"
              R"svg(<rect x="0" y="60" width="10" height="10" fill="#f4f4f4" stroke="#666666"/>)svg");
    const auto state = Resolve(body, false, "pele pele-state");
    ASSERT_TRUE(state);
    EXPECT_NE(state->layers[0].shapes.find(R"svg(height="10" fill="#e6e6e6" stroke="#8a8a8a"/>)svg"), std::string::npos);
    EXPECT_NE(state->layers[0].shapes.find(R"svg(fill="#fff5ad")svg"), std::string::npos);
    const auto dark = Resolve(body, true, "pele pele-flowchart");
    ASSERT_TRUE(dark);
    for (const auto* color : { "#ffffbd", "#aaaa33", "#fff5ad", "#e8e8e8", "#f4f4f4" }) {
        EXPECT_EQ(dark->layers[0].shapes.find(color), std::string::npos) << color;
    }
}

// text-decoration の取り消し線も下線と同じく描く。祖先の線は子孫の文字にも重なる (CSS の装飾の伝播)。
TEST_F(PeleSvgTest, TextDecorationLinesPropagate)
{
    const auto drawing = Resolve(R"svg(<text x="0" y="20" style="text-decoration:line-through">a</text>)svg"
                                 R"svg(<text x="0" y="40" style="text-decoration:underline">b<tspan style="text-decoration:line-through">c</tspan></text>)svg"
                                 R"svg(<text x="0" y="60" style="text-decoration:none">d</text>)svg");
    ASSERT_TRUE(drawing);
    ASSERT_EQ(drawing->layers.size(), 1u);
    const auto& texts = drawing->layers[0].texts;
    ASSERT_EQ(texts.size(), 4u);
    EXPECT_EQ(texts[0].decoration, mermaid_util::kTextLineThrough);
    EXPECT_EQ(texts[1].decoration, mermaid_util::kTextUnderline);
    EXPECT_EQ(texts[2].decoration, mermaid_util::kTextUnderline | mermaid_util::kTextLineThrough);
    EXPECT_EQ(texts[3].decoration, 0);
    const auto svg = DrawingToSvg(*drawing);
    EXPECT_NE(svg.find(R"svg(text-decoration="line-through")svg"), std::string::npos) << svg;
    EXPECT_NE(svg.find(R"svg(text-decoration="underline line-through")svg"), std::string::npos) << svg;
}

// クリップボードの SVG は文字参照を解いた文字を書き直す。XML は本文に "]]>" を書けないので > も変える。
TEST_F(PeleSvgTest, WritesDecodedLabelsEscaped)
{
    const auto drawing = Resolve(R"svg(<text x="0" y="20">a]]&gt;b&apos;c&amp;</text>)svg");
    ASSERT_TRUE(drawing);
    ASSERT_EQ(drawing->layers.size(), 1u);
    ASSERT_EQ(drawing->layers[0].texts.size(), 1u);
    EXPECT_EQ(drawing->layers[0].texts[0].text, L"a]]>b'c&");
    const auto svg = DrawingToSvg(*drawing);
    EXPECT_NE(svg.find(R"svg(xml:space="preserve">a]]&gt;b&#39;c&amp;</text>)svg"), std::string::npos) << svg;
}

// pele は px 以外の線の太さの縁取りを calc(<太さ> + 4px) と書く。足し算と引き算を読み、読めない項があれば宣言ごと無効にする。
TEST_F(PeleSvgTest, ReadsCalcSumsOfLengths)
{
    const auto drawing = Resolve(R"svg(<path d="M0 0L10 0" stroke="#000" style="stroke-width:calc(3pt + 4px)"/>)svg"
                                 R"svg(<path d="M0 0L10 0" stroke="#000" style="stroke-width:calc(1em - 6px + min(2px, 3px))"/>)svg"
                                 R"svg(<g style="stroke-width:3"><path d="M0 0L10 0" stroke="#000" style="stroke-width:calc(50% + 1px)"/></g>)svg");
    ASSERT_TRUE(drawing);
    ASSERT_EQ(drawing->layers.size(), 1u);
    const auto& shapes = drawing->layers[0].shapes;
    EXPECT_NE(shapes.find(R"svg(stroke-width="8")svg"), std::string::npos) << shapes;
    EXPECT_NE(shapes.find(R"svg(stroke-width="12")svg"), std::string::npos) << shapes;
    EXPECT_NE(shapes.find(R"svg(stroke-width="3")svg"), std::string::npos) << shapes;
}

// calc()・min()・max() と color-mix() は入れ子にできる。深すぎる入れ子は読めない宣言として捨て、再帰でスタックを使い切らない
// (図の style や classDef に書いた値が SVG に届くと、数万段でワーカーのスタックがあふれてプロセスごと落ちる)。
TEST_F(PeleSvgTest, DeeplyNestedFunctionsAreInvalidNotStackOverflow)
{
    const auto nested = [](std::string_view open, std::string_view innermost, std::string_view close, size_t depth) {
        std::string s;
        for (size_t i = 0; i < depth; ++i) {
            s += open;
        }
        s += innermost;
        for (size_t i = 0; i < depth; ++i) {
            s += close;
        }
        return s;
    };
    const auto path = [](std::string_view width, std::string_view stroke) {
        return std::format(R"svg(<path d="M0 0L10 0" stroke="{}" style="stroke-width:3;stroke-width:{}"/>)svg", stroke, width);
    };
    // 浅い入れ子は読む。
    const auto shallow = Resolve(path(nested("calc(", "5px", ")", 4), nested("color-mix(in srgb,", "#f00", ",#f00)", 4)));
    ASSERT_TRUE(shallow);
    EXPECT_NE(shallow->layers[0].shapes.find(R"svg(stroke="#ff0000" stroke-width="5")svg"), std::string::npos) << shallow->layers[0].shapes;

    constexpr size_t kDeep = 30000;
    for (const auto& width : { nested("calc(", "5px", ")", kDeep), nested("min(", "5px", ")", kDeep), nested("max(1px,", "5px", ")", kDeep) }) {
        const auto drawing = Resolve(path(width, "#000"));
        ASSERT_TRUE(drawing);
        EXPECT_NE(drawing->layers[0].shapes.find(R"svg(stroke-width="3")svg"), std::string::npos);
    }
    const auto mixed = Resolve(path("3", nested("color-mix(in srgb,", "#f00", ",#f00)", kDeep)));
    ASSERT_TRUE(mixed);
    EXPECT_EQ(mixed->layers[0].shapes.find("stroke="), std::string::npos);
}

// 読めない宣言は CSS と同じく捨て、同じ性質の前の宣言か表示の属性を使う。var() の宣言は解けなければ、前の宣言ではなく
// 親の値になる (計算値の時点で無効)。大きさの無い font の一括指定は宣言ごと捨てる。
TEST_F(PeleSvgTest, InvalidDeclarationFallsBackToEarlierOne)
{
    const auto drawing = Resolve(R"svg(<path d="M0 0L10 0" stroke="#000" style="stroke-width:3;stroke-width:calc(50% + 1px)"/>)svg"
                                 R"svg(<path d="M0 0L10 0" stroke="#000" stroke-width="5" style="stroke-width:foo;fill:#f00;fill:notacolor"/>)svg"
                                 R"svg(<g style="stroke-width:7"><path d="M0 0L10 0" stroke="#000" style="stroke-width:2;stroke-width:var(--missing)"/></g>)svg"
                                 R"svg(<text x="0" y="20" style="font-weight:bold;font:italic">a</text>)svg");
    ASSERT_TRUE(drawing);
    ASSERT_EQ(drawing->layers.size(), 1u);
    const auto& shapes = drawing->layers[0].shapes;
    EXPECT_NE(shapes.find(R"svg(stroke="#000000" stroke-width="3")svg"), std::string::npos) << shapes;
    EXPECT_NE(shapes.find(R"svg(fill="#ff0000" stroke="#000000" stroke-width="5")svg"), std::string::npos) << shapes;
    EXPECT_NE(shapes.find(R"svg(stroke="#000000" stroke-width="7")svg"), std::string::npos) << shapes;
    EXPECT_EQ(shapes.find(R"svg(stroke-width="2")svg"), std::string::npos) << shapes;
    ASSERT_EQ(drawing->layers[0].texts.size(), 1u);
    EXPECT_EQ(drawing->layers[0].texts[0].weight, 700);
    EXPECT_FALSE(drawing->layers[0].texts[0].italic);
}

// 対応の無い ) はその宣言だけを無効にし、後ろの宣言は効かせる (Chrome と同じ)。
TEST_F(PeleSvgTest, UnmatchedParenInvalidatesOnlyItsDeclaration)
{
    const auto drawing = Resolve(R"svg(<rect x="0" y="0" width="10" height="10" style="fill:#fcc);stroke:#00f;stroke-width:4px"/>)svg");
    ASSERT_TRUE(drawing);
    ASSERT_EQ(drawing->layers.size(), 1u);
    const auto& shapes = drawing->layers[0].shapes;
    EXPECT_NE(shapes.find(R"svg(fill="#1f1f1f")svg"), std::string::npos) << shapes;
    EXPECT_NE(shapes.find(R"svg(stroke="#0000ff" stroke-width="4")svg"), std::string::npos) << shapes;
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

// 根の max-width:100% では縮めず、縮めない大きさの描画命令にする (縮めるのは描くとき)。図形にも文字にも拡大率を書かない
// ので、描くときに同じ拡大率が掛かる。
TEST_F(PeleSvgTest, KeepsNaturalSizeDespiteMaxWidthStyle)
{
    const auto drawing = Resolve(R"svg(<rect x="0" y="0" width="10" height="10"/><text x="20" y="40">AB</text>)svg");
    ASSERT_TRUE(drawing);
    EXPECT_EQ(drawing->width, 200.0f);
    EXPECT_EQ(drawing->height, 100.0f);
    EXPECT_EQ(drawing->layers[0].shapes, R"svg(<rect x="0" y="0" width="10" height="10" fill="#1f1f1f"/>)svg");
    ASSERT_EQ(drawing->layers[0].texts.size(), 1u);
    EXPECT_EQ(drawing->layers[0].texts[0].at.transform, (std::array<float, 6>{ 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f }));
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
    EXPECT_NE(defs.find(R"svg(<stop offset="0" stop-color="#ececff" stop-opacity="0.5"/>)svg"), std::string::npos) << defs;
    const auto& shapes = drawing->layers[0].shapes;
    EXPECT_NE(shapes.find(R"svg(fill="url(#g0)"/>)svg"), std::string::npos) << shapes;
}

// 書き出せない要素 (foreignObject) と壊れた SVG は描画命令にしない (描けない図として知らせる)。
TEST_F(PeleSvgTest, RejectsUnsupportedOrMalformedSvg)
{
    EXPECT_FALSE(Resolve(R"svg(<foreignObject x="0" y="0" width="10" height="10"/>)svg"));
    EXPECT_FALSE(Resolve("<g><rect/>"));
    EXPECT_FALSE(DrawingFromPeleSvg("not svg", false, *measurer_));
}

// pele は小さな箱の角丸を min() で縮める。
TEST_F(PeleSvgTest, ResolvesMinInLengths)
{
    const auto drawing = Resolve(R"svg(<rect x="0" y="0" width="10" height="6" style="rx:min(var(--undefined, 4px),3px)"/>)svg");
    ASSERT_TRUE(drawing);
    EXPECT_NE(drawing->layers[0].shapes.find(R"svg(rx="3" ry="3")svg"), std::string::npos) << drawing->layers[0].shapes;
}

// 大きさ 0 の文字や、変換が float に収まらない文字は描けないので書かない。書くとディスクキャッシュから読み戻せず、
// 開くたびに描き直す。
TEST_F(PeleSvgTest, SkipsUnstorableTextAndParsesBack)
{
    const auto drawing = Resolve(R"svg(<text x="0" y="20" style="font-size:0">a</text><text x="0" y="40">b</text>)svg"
                                 R"svg(<g transform="scale(1e39)"><text x="0" y="20">c</text></g>)svg");
    ASSERT_TRUE(drawing);
    ASSERT_EQ(drawing->layers[0].texts.size(), 1u);
    EXPECT_EQ(drawing->layers[0].texts[0].text, L"b");
    EXPECT_TRUE(mermaid_util::ParseDrawing(string_convert::Utf8ToWide(mermaid_util::SerializeDrawing(*drawing))));
}

// 大きすぎる文字はディスクキャッシュが読める上限に丸める。丸めるのは計算値なので、子の em も丸めた値から求める。
// float に収まらない百分率は読めない値として親の大きさを継ぐ。
TEST_F(PeleSvgTest, ClampsFontSizeAndParsesBack)
{
    const auto drawing = Resolve(R"svg(<text x="0" y="20" style="font-size:2000px">a</text>)svg"
                                 R"svg(<g style="font-size:1e30%"><text x="0" y="40" style="font-size:0.5em">b</text></g>)svg"
                                 R"svg(<text x="0" y="60" style="font-size:1e40%">c</text>)svg");
    ASSERT_TRUE(drawing);
    const auto& texts = drawing->layers[0].texts;
    ASSERT_EQ(texts.size(), 3u);
    EXPECT_FLOAT_EQ(texts[0].at.font_size, mermaid_util::kMaxFontSize);
    EXPECT_FLOAT_EQ(texts[1].at.font_size, mermaid_util::kMaxFontSize * 0.5f);
    EXPECT_FLOAT_EQ(texts[2].at.font_size, 16.0f);
    EXPECT_TRUE(mermaid_util::ParseDrawing(string_convert::Utf8ToWide(mermaid_util::SerializeDrawing(*drawing))));
}

// 読めない長さは前の値を残す (float に収まらない値で inf を書き出さない)。
TEST_F(PeleSvgTest, KeepsLengthWhenValueOverflows)
{
    const auto drawing = Resolve(R"svg(<path d="M0 0L10 0" stroke="#000" style="stroke-dasharray:2;stroke-dashoffset:1e39"/>)svg");
    ASSERT_TRUE(drawing);
    const auto& shapes = drawing->layers[0].shapes;
    EXPECT_EQ(shapes.find("stroke-dashoffset"), std::string::npos) << shapes;
}

// transform の関数ごとの行列と、並べたときの掛ける順。引数の数が合わない関数や知らない関数があれば、その transform は無かったものとする。
TEST_F(PeleSvgTest, ResolvesTransformFunctions)
{
    struct Case {
        std::string_view transform;
        std::string_view written; // 空なら transform を書かない
    };
    constexpr Case kCases[] = {
        { "matrix(1 2 3 4 5 6)", "matrix(1 2 3 4 5 6)" },
        { "translate(5)", "translate(5 0)" },
        { "translate(5 6)", "translate(5 6)" },
        { "scale(2)", "matrix(2 0 0 2 0 0)" },
        { "scale(2 3)", "matrix(2 0 0 3 0 0)" },
        { "rotate(90)", "matrix(0 1 -1 0 0 0)" },
        // (10, 20) を中心に回す。
        { "rotate(90 10 20)", "matrix(0 1 -1 0 30 10)" },
        { "skewX(45)", "matrix(1 0 1 1 0 0)" },
        { "skewY(45)", "matrix(1 1 0 1 0 0)" },
        // 左から順に掛ける (後ろの関数が先に点へ掛かる)。
        { "translate(10 20) scale(2)", "matrix(2 0 0 2 10 20)" },
        { "scale(2) translate(10 20)", "matrix(2 0 0 2 20 40)" },
        { "translate(1,2),translate(3,4)", "translate(4 6)" },
        { " translate( 1 2 ) ", "translate(1 2)" },
        { "", "" },
        { "rotate(1 2)", "" },
        { "matrix(1 2 3)", "" },
        { "matrix(1 2 3 4 5 6 7)", "" },
        { "scale()", "" },
        { "scale(1 2 3)", "" },
        { "translate(1 2 3)", "" },
        { "skewX(1 2)", "" },
        { "skewY(1 2)", "" },
        { "foo(1)", "" },
        { "translate(a)", "" },
        { "translate(1", "" },
        { "translate 1)", "" },
        // 途中に 1 つでも解けない関数があれば全体を捨てる。
        { "translate(1 2) foo(3)", "" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.transform);
        const std::string written = c.written.empty() ? std::string{} : std::format(R"svg( transform="{}")svg", c.written);
        EXPECT_EQ(ShapesOf(std::format(R"svg(<g transform="{}"><rect x="0" y="0" width="10" height="10"/></g>)svg", c.transform)),
                  std::format(R"svg(<rect x="0" y="0" width="10" height="10"{} fill="#1f1f1f"/>)svg", written));
    }
}

// 色の書き方ごとの解決。アルファは fill-opacity に分けて書く。解けない色は宣言が無かったものとし、継承した色 (#1f1f1f) のままにする。
TEST_F(PeleSvgTest, ResolvesColorNotations)
{
    struct Case {
        std::string_view color;
        std::string_view written;
    };
    constexpr Case kCases[] = {
        { "#abc", R"svg(fill="#aabbcc")svg" },
        { "#AbC", R"svg(fill="#aabbcc")svg" },
        { "#abcd", R"svg(fill="#aabbcc" fill-opacity="0.867")svg" },
        { "#aabbcc", R"svg(fill="#aabbcc")svg" },
        { "#aabbcc80", R"svg(fill="#aabbcc" fill-opacity="0.502")svg" },
        { "rgb(255,0,0)", R"svg(fill="#ff0000")svg" },
        { "rgb(255 0 0)", R"svg(fill="#ff0000")svg" },
        { "rgb(100% 0% 50%)", R"svg(fill="#ff0080")svg" },
        { "rgba(0,0,255,0.5)", R"svg(fill="#0000ff" fill-opacity="0.5")svg" },
        { "rgb(0 0 255 / 50%)", R"svg(fill="#0000ff" fill-opacity="0.5")svg" },
        // 範囲の外の成分とアルファは端に寄せる。
        { "rgb(300 -5 0)", R"svg(fill="#ff0000")svg" },
        { "rgb(0 0 0 / 2)", R"svg(fill="#000000")svg" },
        { "rgb(0 0 0 / -1)", R"svg(fill="#000000" fill-opacity="0")svg" },
        // 色相は 60 度ごとに赤・黄・緑・シアン・青・マゼンタ。負や 360 以上は 1 周分ずらす。
        { "hsl(0 100% 50%)", R"svg(fill="#ff0000")svg" },
        { "hsl(60 100% 50%)", R"svg(fill="#ffff00")svg" },
        { "hsl(90deg 100% 50%)", R"svg(fill="#80ff00")svg" },
        { "hsl(120,100%,50%)", R"svg(fill="#00ff00")svg" },
        { "hsl(180 50% 50%)", R"svg(fill="#40bfbf")svg" },
        { "hsl(240 100% 25%)", R"svg(fill="#000080")svg" },
        { "hsl(300 100% 50% / 0.25)", R"svg(fill="#ff00ff" fill-opacity="0.25")svg" },
        { "hsl(360 100% 50%)", R"svg(fill="#ff0000")svg" },
        { "hsl(-120 100% 50%)", R"svg(fill="#0000ff")svg" },
        { "hsla(0,0%,50%,0.5)", R"svg(fill="#808080" fill-opacity="0.5")svg" },
        { "red", R"svg(fill="#ff0000")svg" },
        { "RED", R"svg(fill="#ff0000")svg" },
        { "white", R"svg(fill="#ffffff")svg" },
        { "black", R"svg(fill="#000000")svg" },
        { "transparent", R"svg(fill="#000000" fill-opacity="0")svg" },
        { "currentColor", R"svg(fill="#1f1f1f")svg" },
        { "none", R"svg(fill="none")svg" },
        // 割合は片方だけなら残りを相手に回し、どちらも無ければ半々。合計が 100% に満たない分はアルファに掛ける。
        { "color-mix(in srgb, red 25%, blue)", R"svg(fill="#4000bf")svg" },
        { "color-mix(in srgb, red, blue 25%)", R"svg(fill="#bf0040")svg" },
        { "color-mix(in srgb, red 50%, blue 50%)", R"svg(fill="#800080")svg" },
        { "color-mix(in srgb, red, blue)", R"svg(fill="#800080")svg" },
        { "color-mix(in srgb, red 20%, blue 20%)", R"svg(fill="#800080" fill-opacity="0.4")svg" },
        { "color-mix(in srgb, currentColor 50%, #fff)", R"svg(fill="#8f8f8f")svg" },
        // 半透明の色はアルファを掛けてから混ぜる。
        { "color-mix(in srgb, rgb(0 0 0 / 0.5), #fff)", R"svg(fill="#aaaaaa" fill-opacity="0.75")svg" },
        { "color-mix(in srgb, red 0%, blue 0%)", R"svg(fill="#1f1f1f")svg" },
        { "color-mix(in srgb, red 150%, blue)", R"svg(fill="#1f1f1f")svg" },
        { "color-mix(in srgb, red)", R"svg(fill="#1f1f1f")svg" },
        { "color-mix(in hsl, red, blue)", R"svg(fill="#1f1f1f")svg" },
        { "#", R"svg(fill="#1f1f1f")svg" },
        { "#ab", R"svg(fill="#1f1f1f")svg" },
        { "#abcde", R"svg(fill="#1f1f1f")svg" },
        { "#ggg", R"svg(fill="#1f1f1f")svg" },
        { "rgb(1,2)", R"svg(fill="#1f1f1f")svg" },
        { "rgb(1,2,3,4,5)", R"svg(fill="#1f1f1f")svg" },
        { "rgb(a,b,c)", R"svg(fill="#1f1f1f")svg" },
        { "rgb(1 2 3", R"svg(fill="#1f1f1f")svg" },
        { "hsl(1)", R"svg(fill="#1f1f1f")svg" },
        { "foo", R"svg(fill="#1f1f1f")svg" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.color);
        EXPECT_EQ(ShapesOf(std::format(R"svg(<rect x="0" y="0" width="10" height="10" stroke="#010203" fill="{}"/>)svg", c.color)),
                  std::format(R"svg(<rect x="0" y="0" width="10" height="10" {} stroke="#010203"/>)svg", c.written));
    }
}

// path の外接矩形を、命令ごとに確かめる。矩形そのものは外から見えないので、小さな文字を矩形の辺の内と外に置き、path が次の層へ
// 送られるか (文字に重なるか) で見る。曲線は制御点を含め、弧は楕円全体を含める。読めない命令の手前までを範囲にする。
TEST_F(PeleSvgTest, PathBoundsFollowEachCommand)
{
    struct Case {
        std::string_view d;
        double left, top, right, bottom;
    };
    constexpr Case kCases[] = {
        // 直線。M に続く座標は L として読み、小文字は今の位置からの相対。
        { "M20 20 L60 50", 20, 20, 60, 50 },
        { "M20 20 l40 30", 20, 20, 60, 50 },
        { "M20 20 60 50", 20, 20, 60, 50 },
        { "m20 20 40 30", 20, 20, 60, 50 },
        { "M20 20 H60 V50", 20, 20, 60, 50 },
        { "M20 20 h40 v30", 20, 20, 60, 50 },
        { "M60 50 H20 V20", 20, 20, 60, 50 },
        { "M60 50 h-40 v-30", 20, 20, 60, 50 },
        // 数の書き方: , 区切り、符号が区切りを兼ねる、指数、先頭の + と小数点。
        { "M20,20L60,50", 20, 20, 60, 50 },
        { "M60 50l-40-30", 20, 20, 60, 50 },
        { "M2e1 20L+60 50", 20, 20, 60, 50 },
        { "M.2e2 20 L60 50.0", 20, 20, 60, 50 },
        // 曲線は制御点までを範囲にする。
        { "M20 50 C30 20 50 20 60 50", 20, 20, 60, 50 },
        { "M20 50 c10 -30 30 -30 40 0", 20, 20, 60, 50 },
        { "M20 50 Q40 20 60 50", 20, 20, 60, 50 },
        { "M20 50 q20 -30 40 0", 20, 20, 60, 50 },
        // S と T の 1 つ目の制御点は、直前の同じ種類の曲線の制御点を今の位置で折り返した点 ((60,80) → (20,20)、(30,80) → (50,20))。
        { "M30 50 C30 50 60 80 40 50 S45 50 50 50", 20, 20, 60, 80 },
        { "M30 50 c0 0 30 30 10 0 s5 0 10 0", 20, 20, 60, 80 },
        { "M20 50 Q30 80 40 50 T60 50", 20, 20, 60, 80 },
        { "M20 50 q10 30 20 0 t20 0", 20, 20, 60, 80 },
        // 直前が同じ種類の曲線でなければ、今の位置を制御点にする。
        { "M20 50 S60 20 60 50", 20, 20, 60, 50 },
        { "M20 20 T60 50", 20, 20, 60, 50 },
        { "M20 50 Q40 20 60 50 S60 30 30 40", 20, 20, 60, 50 },
        { "M20 50 C30 20 50 20 60 50 T30 40", 20, 20, 60, 50 },
        // Z は図形の始点へ戻る。
        { "M20 20 L40 20 L40 40 Z l40 30", 20, 20, 60, 50 },
        { "M20 20 L40 20 L40 40 z m10 10 l30 20", 20, 20, 60, 50 },
        // 弧は楕円全体。半円は中心が弦の中点。
        { "M20 50 A20 20 0 0 1 60 50", 20, 30, 60, 70 },
        { "M20 50 a20 20 0 0 1 40 0", 20, 30, 60, 70 },
        { "M20 50 A-20 -20 0 0 1 60 50", 20, 30, 60, 70 },
        // フラグは区切り無しで続けて書ける。
        { "M20 50 A20 20 0 0160 50", 20, 30, 60, 70 },
        // 半径 25 で弦 40 の弧は中心が弦から 15 離れる。large と sweep が同じなら上 (40,35)、違えば下 (40,65)。
        { "M20 50 A25 25 0 0 0 60 50", 15, 10, 65, 60 },
        { "M20 50 A25 25 0 1 1 60 50", 15, 10, 65, 60 },
        { "M20 50 A25 25 0 0 1 60 50", 15, 40, 65, 90 },
        { "M20 50 A25 25 0 1 0 60 50", 15, 40, 65, 90 },
        // 届かない半径は弦がちょうど直径になるまで広げる。
        { "M20 50 A5 5 0 0 1 60 50", 20, 30, 60, 70 },
        { "M20 50 A20 10 0 0 1 60 50", 20, 40, 60, 60 },
        // 90 度回した楕円は幅と高さが入れ替わる。
        { "M40 30 A20 10 90 0 1 40 70", 30, 30, 50, 70 },
        // 半径 0 の弧は終点までの直線。
        { "M20 20 A0 20 0 0 1 60 50", 20, 20, 60, 50 },
        { "M20 20 A20 0 0 0 1 60 50", 20, 20, 60, 50 },
        // 読めない命令・足りない引数・0 と 1 以外のフラグは、その手前まで。
        { "M20 20 L60 50 X90 90", 20, 20, 60, 50 },
        { "M20 20 L60 50 L90", 20, 20, 60, 50 },
        { "M20 20 L60 50 A20 20 0 2 1 90 90", 20, 20, 60, 50 },
        { "M20 20 L60 50 A20 20 0 0 1 90", 20, 20, 60, 50 },
        { "M20 20 L60 50 C90 90 90 90", 20, 20, 60, 50 },
        { "M20 20 L60 50 Q90 90 90", 20, 20, 60, 50 },
        { "M20 20 L60 50 H", 20, 20, 60, 50 },
    };
    const auto covers = [&](std::string_view d, double x, double y) {
        return SplitsLayer(std::format(R"svg(<text x="{}" y="{}" font-size="1">.</text><path d="{}"/>)svg", x, y, d));
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.d);
        const double cx = (c.left + c.right) / 2.0;
        const double cy = (c.top + c.bottom) / 2.0;
        EXPECT_TRUE(covers(c.d, c.left + 3.0, cy)) << "左の辺の内";
        EXPECT_TRUE(covers(c.d, c.right - 3.0, cy)) << "右の辺の内";
        EXPECT_TRUE(covers(c.d, cx, c.top + 3.0)) << "上の辺の内";
        EXPECT_TRUE(covers(c.d, cx, c.bottom - 3.0)) << "下の辺の内";
        EXPECT_FALSE(covers(c.d, c.left - 5.0, cy)) << "左の辺の外";
        EXPECT_FALSE(covers(c.d, c.right + 5.0, cy)) << "右の辺の外";
        EXPECT_FALSE(covers(c.d, cx, c.top - 5.0)) << "上の辺の外";
        EXPECT_FALSE(covers(c.d, cx, c.bottom + 5.0)) << "下の辺の外";
    }
    // 命令の前に数が来る d は 1 つも読めず、範囲が無いのでどの文字にも重ならない。
    EXPECT_FALSE(covers("20 20 L60 50", 40.0, 35.0));
    EXPECT_FALSE(covers("", 40.0, 35.0));
}

// polygon と polyline の範囲は points の座標の外接矩形。座標が奇数個でも、読めたところまでを範囲にする。読めない文字があれば範囲を持たない。
TEST_F(PeleSvgTest, PointsBoundsCoverAllPoints)
{
    const auto covers = [&](std::string_view points, double x, double y) {
        return SplitsLayer(std::format(R"svg(<text x="{}" y="{}" font-size="1">.</text><polygon points="{}"/>)svg", x, y, points));
    };
    for (const std::string_view points : { "20,20 60,20 40,50", "40 50 20 20 60 20", "20,20 60,50 99" }) {
        SCOPED_TRACE(points);
        EXPECT_TRUE(covers(points, 23.0, 35.0));
        EXPECT_TRUE(covers(points, 57.0, 35.0));
        EXPECT_TRUE(covers(points, 40.0, 23.0));
        EXPECT_TRUE(covers(points, 40.0, 47.0));
        EXPECT_FALSE(covers(points, 15.0, 35.0));
        EXPECT_FALSE(covers(points, 65.0, 35.0));
        EXPECT_FALSE(covers(points, 40.0, 15.0));
        EXPECT_FALSE(covers(points, 40.0, 55.0));
    }
    EXPECT_FALSE(covers("20,20 60,50 x", 40.0, 35.0));
}

// 図形の宣言ごとの解決。初期値と同じ値は書かない。読めない値の宣言は無かったものとし、継承する性質は親の値、しない性質は初期値に戻る。
// 描かない図形 (display:none・見えない・不透明度 0) は何も書かない。
TEST_F(PeleSvgTest, ResolvesShapeStyleDeclarations)
{
    struct Case {
        std::string_view parent_style;
        std::string_view style;
        std::string_view written; // height の後ろに書く属性。空なら図形を書かない
    };
    constexpr Case kCases[] = {
        { "", "stroke:red;stroke-width:2", R"svg( fill="#1f1f1f" stroke="#ff0000" stroke-width="2")svg" },
        { "", "stroke:red;stroke-width:2px !important", R"svg( fill="#1f1f1f" stroke="#ff0000" stroke-width="2")svg" },
        { "", "stroke:red;stroke-width:-1", R"svg( fill="#1f1f1f" stroke="#ff0000")svg" },
        { "", "stroke:red;stroke-width:0", R"svg( fill="#1f1f1f" stroke="#ff0000" stroke-width="0")svg" },
        { "", "stroke:red;stroke-width:1.5em", R"svg( fill="#1f1f1f" stroke="#ff0000" stroke-width="24")svg" },
        { "", "stroke:red;stroke-width:abc", R"svg( fill="#1f1f1f" stroke="#ff0000")svg" },
        { "", "stroke:red;stroke-width:3 ! IMPORTANT", R"svg( fill="#1f1f1f" stroke="#ff0000" stroke-width="3")svg" },
        { "", "stroke:red;stroke-width:3 !x", R"svg( fill="#1f1f1f" stroke="#ff0000")svg" },
        { "", "stroke:red;stroke-width:", R"svg( fill="#1f1f1f" stroke="#ff0000")svg" },
        { "stroke:red;stroke-width:3", "stroke-width:inherit", R"svg( fill="#1f1f1f" stroke="#ff0000" stroke-width="3")svg" },
        { "stroke:red;stroke-width:3", "", R"svg( fill="#1f1f1f" stroke="#ff0000" stroke-width="3")svg" },
        { "", "stroke:red;stroke-dasharray:4 2", R"svg( fill="#1f1f1f" stroke="#ff0000" stroke-dasharray="4 2")svg" },
        { "", "stroke:red;stroke-dasharray:4,2", R"svg( fill="#1f1f1f" stroke="#ff0000" stroke-dasharray="4 2")svg" },
        { "", "stroke:red;stroke-dasharray:none", R"svg( fill="#1f1f1f" stroke="#ff0000")svg" },
        { "", "stroke:red;stroke-dasharray:0 0", R"svg( fill="#1f1f1f" stroke="#ff0000")svg" },
        { "", "stroke:red;stroke-dasharray:4 -2", R"svg( fill="#1f1f1f" stroke="#ff0000")svg" },
        { "", "stroke:red;stroke-dasharray:0 3", R"svg( fill="#1f1f1f" stroke="#ff0000" stroke-dasharray="0 3")svg" },
        { "", "stroke:red;stroke-dasharray:1em 2px", R"svg( fill="#1f1f1f" stroke="#ff0000" stroke-dasharray="16 2")svg" },
        { "", "stroke:red;stroke-dasharray:x", R"svg( fill="#1f1f1f" stroke="#ff0000")svg" },
        { "stroke-dasharray:4 2", "stroke:red;stroke-dasharray:NONE", R"svg( fill="#1f1f1f" stroke="#ff0000")svg" },
        { "stroke-dasharray:4 2", "stroke:red;stroke-dasharray:0", R"svg( fill="#1f1f1f" stroke="#ff0000")svg" },
        { "", "stroke:red;stroke-miterlimit:2", R"svg( fill="#1f1f1f" stroke="#ff0000" stroke-miterlimit="2")svg" },
        { "", "stroke:red;stroke-miterlimit:1", R"svg( fill="#1f1f1f" stroke="#ff0000" stroke-miterlimit="1")svg" },
        { "", "stroke:red;stroke-miterlimit:0.5", R"svg( fill="#1f1f1f" stroke="#ff0000")svg" },
        { "", "stroke:red;stroke-miterlimit:4", R"svg( fill="#1f1f1f" stroke="#ff0000")svg" },
        { "", "stroke:red;stroke-linecap:round", R"svg( fill="#1f1f1f" stroke="#ff0000" stroke-linecap="round")svg" },
        { "", "stroke:red;stroke-linecap:SQUARE", R"svg( fill="#1f1f1f" stroke="#ff0000" stroke-linecap="square")svg" },
        { "", "stroke:red;stroke-linecap:butt", R"svg( fill="#1f1f1f" stroke="#ff0000")svg" },
        { "", "stroke:red;stroke-linecap:bevel", R"svg( fill="#1f1f1f" stroke="#ff0000")svg" },
        { "", "stroke:red;stroke-linejoin:round", R"svg( fill="#1f1f1f" stroke="#ff0000" stroke-linejoin="round")svg" },
        { "", "stroke:red;stroke-linejoin:bevel", R"svg( fill="#1f1f1f" stroke="#ff0000" stroke-linejoin="bevel")svg" },
        { "", "stroke:red;stroke-linejoin:miter", R"svg( fill="#1f1f1f" stroke="#ff0000")svg" },
        { "", "stroke:red;stroke-linejoin:square", R"svg( fill="#1f1f1f" stroke="#ff0000")svg" },
        { "stroke-linecap:round", "stroke:red;stroke-linecap:x", R"svg( fill="#1f1f1f" stroke="#ff0000" stroke-linecap="round")svg" },
        { "", "fill-rule:evenodd", R"svg( fill="#1f1f1f" fill-rule="evenodd")svg" },
        { "", "fill-rule:nonzero", R"svg( fill="#1f1f1f")svg" },
        { "fill-rule:evenodd", "fill-rule:x", R"svg( fill="#1f1f1f" fill-rule="evenodd")svg" },
        { "fill-rule:evenodd", "fill-rule:NONZERO", R"svg( fill="#1f1f1f")svg" },
        { "", "fill-opacity:0.5", R"svg( fill="#1f1f1f" fill-opacity="0.5")svg" },
        { "", "fill-opacity:50%", R"svg( fill="#1f1f1f" fill-opacity="0.5")svg" },
        { "", "fill-opacity:2", R"svg( fill="#1f1f1f")svg" },
        { "", "fill-opacity:-1", R"svg( fill="#1f1f1f" fill-opacity="0")svg" },
        { "", "fill-opacity:x", R"svg( fill="#1f1f1f")svg" },
        { "", "stroke:red;stroke-opacity:0.25", R"svg( fill="#1f1f1f" stroke="#ff0000" stroke-opacity="0.25")svg" },
        { "", "opacity:0.5", R"svg( fill="#1f1f1f" opacity="0.5")svg" },
        { "opacity:0.5", "opacity:0.5", R"svg( fill="#1f1f1f" opacity="0.25")svg" },
        { "opacity:0.5", "", R"svg( fill="#1f1f1f" opacity="0.5")svg" },
        { "opacity:0.5", "opacity:inherit", R"svg( fill="#1f1f1f" opacity="0.25")svg" },
        { "", "opacity:0", "" },
        { "", "display:none", "" },
        { "", "display:NONE", "" },
        { "", "display:block", R"svg( fill="#1f1f1f")svg" },
        { "display:none", "display:block", "" },
        { "", "visibility:hidden", "" },
        { "", "visibility:collapse", "" },
        { "visibility:hidden", "visibility:visible", R"svg( fill="#1f1f1f")svg" },
        { "visibility:hidden", "visibility:x", "" },
        { "visibility:hidden", "", "" },
        { "", "rx:3", R"svg( rx="3" ry="3" fill="#1f1f1f")svg" },
        { "", "rx:3;ry:5", R"svg( rx="3" ry="5" fill="#1f1f1f")svg" },
        { "", "ry:5", R"svg( rx="5" ry="5" fill="#1f1f1f")svg" },
        { "", "rx:-1", R"svg( fill="#1f1f1f")svg" },
        { "", "rx:3;ry:auto", R"svg( rx="3" ry="3" fill="#1f1f1f")svg" },
        { "rx:3", "rx:inherit", R"svg( rx="3" ry="3" fill="#1f1f1f")svg" },
        { "rx:3", "", R"svg( fill="#1f1f1f")svg" },
        { "rx:3", "ry:inherit", R"svg( fill="#1f1f1f")svg" },
        { "ry:4", "ry:inherit", R"svg( rx="4" ry="4" fill="#1f1f1f")svg" },
        { "", "rx:1em", R"svg( rx="16" ry="16" fill="#1f1f1f")svg" },
        { "", "rx:x", R"svg( fill="#1f1f1f")svg" },
        { "", "rx:20", R"svg( rx="20" ry="20" fill="#1f1f1f")svg" },
        { "", "color:red;fill:currentColor", R"svg( fill="#ff0000")svg" },
        { "color:red", "fill:currentColor", R"svg( fill="#ff0000")svg" },
        { "color:red", "color:inherit;fill:currentColor", R"svg( fill="#ff0000")svg" },
        { "fill:red", "fill:inherit", R"svg( fill="#ff0000")svg" },
        { "", "fill:red !important", R"svg( fill="#ff0000")svg" },
        { "", "fill:red!important", R"svg( fill="#ff0000")svg" },
        { "", "fill:inherit", R"svg( fill="#1f1f1f")svg" },
        { "", "FILL:red", R"svg( fill="#ff0000")svg" },
        { "", "fill:red;fill:blue", R"svg( fill="#0000ff")svg" },
        { "", "fill:red;fill:bogus", R"svg( fill="#ff0000")svg" },
        { "", "fill: red ; ; stroke : blue", R"svg( fill="#ff0000" stroke="#0000ff")svg" },
        // 線のずれは破線のときだけ書く。
        { "", "stroke:red;stroke-dasharray:4 2;stroke-dashoffset:3", R"svg( fill="#1f1f1f" stroke="#ff0000" stroke-dasharray="4 2" stroke-dashoffset="3")svg" },
        { "", "stroke:red;stroke-dashoffset:3", R"svg( fill="#1f1f1f" stroke="#ff0000")svg" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(testing::Message() << "[" << c.parent_style << "] [" << c.style << "]");
        const std::string expected = c.written.empty() ? std::string{} : std::format(R"svg(<rect x="0" y="0" width="50" height="50"{}/>)svg", c.written);
        EXPECT_EQ(ShapesOf(std::format(R"svg(<g style="{}"><rect x="0" y="0" width="50" height="50" style="{}"/></g>)svg", c.parent_style, c.style)),
                  expected);
    }
}

// 文字の宣言ごとの解決: 太さ (bolder・lighter は親の太さから決める)・斜体・大きさ・線・そろえる位置。
TEST_F(PeleSvgTest, ResolvesTextStyleDeclarations)
{
    struct Case {
        std::string_view parent_style;
        std::string_view style;
        uint16_t weight;
        bool italic;
        uint8_t decoration;
        float font_size;
        float anchor; // x から左へずらす量の、文字の幅に対する割合
    };
    constexpr Case kCases[] = {
        { "", "", 400, false, 0, 16.0f, 0.0f },
        { "", "font-weight:bold", 700, false, 0, 16.0f, 0.0f },
        { "", "font-weight:normal", 400, false, 0, 16.0f, 0.0f },
        { "", "font-weight:600", 600, false, 0, 16.0f, 0.0f },
        { "", "font-weight:0", 400, false, 0, 16.0f, 0.0f },
        { "", "font-weight:1", 1, false, 0, 16.0f, 0.0f },
        { "", "font-weight:1000", 1000, false, 0, 16.0f, 0.0f },
        { "", "font-weight:1001", 400, false, 0, 16.0f, 0.0f },
        { "", "font-weight:600x", 400, false, 0, 16.0f, 0.0f },
        { "", "font-weight:BOLD", 700, false, 0, 16.0f, 0.0f },
        { "", "font-weight:bolder", 700, false, 0, 16.0f, 0.0f },
        { "", "font-weight:lighter", 100, false, 0, 16.0f, 0.0f },
        { "font-weight:100", "font-weight:bolder", 400, false, 0, 16.0f, 0.0f },
        { "font-weight:349", "font-weight:bolder", 400, false, 0, 16.0f, 0.0f },
        { "font-weight:350", "font-weight:bolder", 700, false, 0, 16.0f, 0.0f },
        { "font-weight:549", "font-weight:bolder", 700, false, 0, 16.0f, 0.0f },
        { "font-weight:550", "font-weight:bolder", 900, false, 0, 16.0f, 0.0f },
        { "font-weight:900", "font-weight:bolder", 900, false, 0, 16.0f, 0.0f },
        { "font-weight:100", "font-weight:lighter", 100, false, 0, 16.0f, 0.0f },
        { "font-weight:549", "font-weight:lighter", 100, false, 0, 16.0f, 0.0f },
        { "font-weight:550", "font-weight:lighter", 400, false, 0, 16.0f, 0.0f },
        { "font-weight:749", "font-weight:lighter", 400, false, 0, 16.0f, 0.0f },
        { "font-weight:750", "font-weight:lighter", 700, false, 0, 16.0f, 0.0f },
        { "font-weight:900", "font-weight:LIGHTER", 700, false, 0, 16.0f, 0.0f },
        { "font-weight:bold", "font-weight:x", 700, false, 0, 16.0f, 0.0f },
        { "font-weight:bold", "", 700, false, 0, 16.0f, 0.0f },
        { "", "font-style:italic", 400, true, 0, 16.0f, 0.0f },
        { "", "font-style:oblique", 400, true, 0, 16.0f, 0.0f },
        { "", "font-style:oblique 10deg", 400, true, 0, 16.0f, 0.0f },
        { "", "font-style:normal", 400, false, 0, 16.0f, 0.0f },
        { "font-style:italic", "font-style:normal", 400, false, 0, 16.0f, 0.0f },
        { "font-style:italic", "font-style:x", 400, true, 0, 16.0f, 0.0f },
        { "font-style:italic", "", 400, true, 0, 16.0f, 0.0f },
        { "", "font-style:ITALIC", 400, true, 0, 16.0f, 0.0f },
        { "", "font-size:20px", 400, false, 0, 20.0f, 0.0f },
        { "", "font-size:2em", 400, false, 0, 32.0f, 0.0f },
        { "", "font-size:50%", 400, false, 0, 8.0f, 0.0f },
        { "", "font-size:smaller", 400, false, 0, 13.3333f, 0.0f },
        { "", "font-size:larger", 400, false, 0, 19.2f, 0.0f },
        { "", "font-size:xx-small", 400, false, 0, 9.0f, 0.0f },
        { "", "font-size:x-small", 400, false, 0, 10.0f, 0.0f },
        { "", "font-size:small", 400, false, 0, 13.0f, 0.0f },
        { "", "font-size:medium", 400, false, 0, 16.0f, 0.0f },
        { "", "font-size:large", 400, false, 0, 18.0f, 0.0f },
        { "", "font-size:x-large", 400, false, 0, 24.0f, 0.0f },
        { "", "font-size:xx-large", 400, false, 0, 32.0f, 0.0f },
        { "", "font-size:LARGE", 400, false, 0, 18.0f, 0.0f },
        { "", "font-size:-5px", 400, false, 0, 16.0f, 0.0f },
        { "", "font-size:x", 400, false, 0, 16.0f, 0.0f },
        { "font-size:20px", "font-size:1.5em", 400, false, 0, 30.0f, 0.0f },
        { "font-size:20px", "font-size:larger", 400, false, 0, 24.0f, 0.0f },
        { "font-size:20px", "font-size:inherit", 400, false, 0, 20.0f, 0.0f },
        { "", "text-decoration:underline", 400, false, 1, 16.0f, 0.0f },
        { "", "text-decoration:line-through", 400, false, 2, 16.0f, 0.0f },
        { "", "text-decoration:underline line-through red", 400, false, 3, 16.0f, 0.0f },
        { "", "text-decoration:none", 400, false, 0, 16.0f, 0.0f },
        { "", "text-anchor:middle", 400, false, 0, 16.0f, 0.5f },
        { "", "text-anchor:end", 400, false, 0, 16.0f, 1.0f },
        { "", "text-anchor:start", 400, false, 0, 16.0f, 0.0f },
        { "text-anchor:end", "text-anchor:x", 400, false, 0, 16.0f, 1.0f },
        { "text-anchor:end", "text-anchor:START", 400, false, 0, 16.0f, 0.0f },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(testing::Message() << "[" << c.parent_style << "] [" << c.style << "]");
        const auto text = OnlyTextOf(std::format(R"svg(<g style="{}"><text x="100" y="20" style="{}">AB</text></g>)svg", c.parent_style, c.style));
        ASSERT_TRUE(text);
        EXPECT_EQ(text->weight, c.weight);
        EXPECT_EQ(text->italic, c.italic);
        EXPECT_EQ(text->decoration, c.decoration);
        EXPECT_NEAR(text->at.font_size, c.font_size, 0.001f);
        EXPECT_NEAR(100.0f - text->at.x, c.anchor * Width(L"AB", text->at.font_size), 0.01f);
        EXPECT_EQ(text->at.y, 20.0f);
    }
}

// 文字の色は塗りの色、不透明度は塗りと要素の不透明度の積。描かない文字 (見えない・塗りが無い・不透明度 0) は置かない。
TEST_F(PeleSvgTest, ResolvesTextPaintAndVisibility)
{
    const auto shown = Resolve(R"svg(<text x="0" y="20" style="fill:red;fill-opacity:0.5;opacity:0.5;font-family:Arial, 'Yu Gothic'">AB</text>)svg");
    ASSERT_TRUE(shown);
    ASSERT_EQ(shown->layers[0].texts.size(), 1u);
    EXPECT_EQ(shown->layers[0].texts[0].at.rgb, 0xFF0000u);
    EXPECT_EQ(shown->layers[0].texts[0].at.alpha, 0.25f);
    EXPECT_EQ(shown->layers[0].texts[0].family, L"Arial, 'Yu Gothic'");
    for (const std::string_view style : { "visibility:hidden", "display:none", "fill:none", "opacity:0" }) {
        SCOPED_TRACE(style);
        const auto hidden = Resolve(std::format(R"svg(<text x="0" y="20" style="{}">AB</text>)svg", style));
        ASSERT_TRUE(hidden);
        ASSERT_EQ(hidden->layers.size(), 1u);
        EXPECT_TRUE(hidden->layers[0].texts.empty());
    }
}

// baseline-shift は tspan を上へずらす量 (y は下向きなので引く)。sub と super は行の高さの半分ずつ、逆向きにずらす。
TEST_F(PeleSvgTest, BaselineShiftMovesSpan)
{
    const auto y_of = [&](std::string_view shift) {
        const auto text = OnlyTextOf(std::format(R"svg(<text x="0" y="20"><tspan style="baseline-shift:{}">AB</tspan></text>)svg", shift));
        EXPECT_TRUE(text) << shift;
        return text ? text->at.y : 0.0f;
    };
    EXPECT_EQ(y_of("baseline"), 20.0f);
    EXPECT_EQ(y_of("4px"), 16.0f);
    EXPECT_EQ(y_of("-4px"), 24.0f);
    EXPECT_EQ(y_of("50%"), 12.0f);
    EXPECT_EQ(y_of("bogus"), 20.0f);
    const float sub = y_of("sub") - 20.0f;
    const float super = 20.0f - y_of("SUPER");
    EXPECT_GT(sub, 4.0f);
    EXPECT_NEAR(sub, super, 0.001f);
}

// 文字と属性値の実体参照を解く。名前付きは XML の 5 つだけで、数値は 10 進と 16 進。解けない参照があれば SVG ごと読まない。
TEST_F(PeleSvgTest, DecodesEntityReferences)
{
    struct Case {
        std::string_view text;
        std::optional<std::wstring_view> decoded;
    };
    constexpr Case kCases[] = {
        { "a&amp;b", L"a&b" },
        { "&lt;&gt;&quot;&apos;", L"<>\"'" },
        { "&#65;&#x42;&#X43;", L"ABC" },
        { "&#x3042;", L"あ" },
        { "&#x1F600;", L"\U0001F600" },
        // 参照は & から ; まで 12 文字以内。
        { "&#x000000041;", L"A" },
        { "&#x0000000041;", std::nullopt },
        { "&bogus;", std::nullopt },
        { "&AMP;", std::nullopt },
        { "a &amp b", std::nullopt },
        { "&;", std::nullopt },
        { "&#;", std::nullopt },
        { "&#x;", std::nullopt },
        { "&#0;", std::nullopt },
        { "&#x110000;", std::nullopt },
        { "&#12a;", std::nullopt },
        { "&#xZZ;", std::nullopt },
        { "&#-1;", std::nullopt },
        // CDATA の中は参照もタグも解かない。コメントと処理命令は読み飛ばす。
        { "<![CDATA[a<b&amp;]]>", L"a<b&amp;" },
        { "a<!-- x > y -->b", L"ab" },
        { "a<?pi x > y?>b", L"ab" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.text);
        const auto drawing = Resolve(std::format(R"svg(<text x="0" y="20">{}</text>)svg", c.text));
        ASSERT_EQ(drawing.has_value(), c.decoded.has_value());
        if (drawing) {
            ASSERT_EQ(drawing->layers.size(), 1u);
            // コメントや CDATA で分かれた文字は別の並びになるので、つないで比べる。
            std::wstring joined;
            for (const auto& text : drawing->layers[0].texts) {
                joined += text.text;
            }
            EXPECT_EQ(joined, *c.decoded);
        }
    }
    // 属性値の中の参照。
    EXPECT_EQ(ShapesOf(R"svg(<rect x="0" y="0" width="10" height="10" fill="&#x23;f00"/>)svg"),
              R"svg(<rect x="0" y="0" width="10" height="10" fill="#ff0000"/>)svg");
    EXPECT_EQ(ShapesOf(R"svg(<rect x="0" y="0" width="10" height="10" fill="&bogus;"/>)svg"), std::nullopt);
}

// 要素と属性の書き方の揺れ: 引用符は " と '、= の前後と属性の間の空白、閉じタグの名前の後ろの空白、値の中の > と別の引用符。
TEST_F(PeleSvgTest, ReadsXmlSyntaxVariants)
{
    constexpr std::string_view kExpected = R"svg(<rect x="0" y="0" width="10" height="10" fill="#ff0000"/>)svg";
    constexpr std::string_view kBodies[] = {
        R"svg(<rect x="0" y="0" width="10" height="10" fill="red"/>)svg",
        R"svg(<rect x='0' y='0' width='10' height='10' fill='red'/>)svg",
        R"svg(<rect x = "0" y= "0" width ="10" height="10" fill="red" />)svg",
        "<rect\n\tx=\"0\"\r\ny=\"0\" width=\"10\" height=\"10\" fill=\"red\"\n/>",
        R"svg(<rect x="0" y="0" width="10" height="10" fill="red"></rect>)svg",
        R"svg(<rect x="0" y="0" width="10" height="10" fill="red"></rect >)svg",
        R"svg(<g data-a="x>y" data-b='say "hi"' data-c="it's"><rect x="0" y="0" width="10" height="10" fill="red"/></g >)svg",
        R"svg(<!-- <rect/> --><rect x="0" y="0" width="10" height="10" fill="red"/><!---->)svg",
        R"svg(<?pi?><rect x="0" y="0" width="10" height="10" fill="red"/>)svg",
    };
    for (const auto body : kBodies) {
        SCOPED_TRACE(body);
        EXPECT_EQ(ShapesOf(body), kExpected);
    }
}

// 壊れた XML は読まない (途中まで描かない)。
TEST_F(PeleSvgTest, RejectsMalformedXml)
{
    constexpr std::string_view kBodies[] = {
        R"svg(<rect x="0")svg",        // 閉じない開始タグ
        R"svg(<rect x=0/>)svg",        // 引用符の無い値
        R"svg(<rect x/>)svg",          // 値の無い属性
        R"svg(<rect x y="0"/>)svg",
        R"svg(<rect x="0/>)svg",       // 閉じない値
        R"svg(<rect =""/>)svg",        // 名前の無い属性
        R"svg(<rect / >)svg",          // / の後ろが > でない
        R"svg(< rect/>)svg",           // 名前の無い要素
        R"svg(<g></h>)svg",            // 対応しない閉じタグ
        R"svg(<g>)svg",                // 閉じない要素
        R"svg(</g>)svg",
        R"svg(<!-- 閉じないコメント)svg",
        R"svg(<![CDATA[ 閉じない)svg",
        R"svg(<?pi 閉じない)svg",
        R"svg(<!DOCTYPE 閉じない)svg",
    };
    ASSERT_TRUE(Resolve(R"svg(<g></g>)svg"));
    for (const auto body : kBodies) {
        SCOPED_TRACE(body);
        EXPECT_FALSE(Resolve(body));
    }

    // 文書全体: 根の要素は 1 つ。前後の宣言・コメント・文字は読み飛ばす。
    constexpr std::string_view kRoot = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="10"><rect x="0" y="0" width="10" height="10"/></svg>)svg";
    EXPECT_TRUE(ResolveRaw(kRoot));
    EXPECT_TRUE(ResolveRaw(std::format("<?xml version=\"1.0\"?>\n<!DOCTYPE svg>\n<!-- c -->\n{}\n<!-- d -->\n", kRoot)));
    EXPECT_TRUE(ResolveRaw(std::format("text{}text", kRoot)));
    EXPECT_FALSE(ResolveRaw(std::format("{}{}", kRoot, kRoot)));
    EXPECT_FALSE(ResolveRaw(std::format("{}<g/>", kRoot)));
    EXPECT_FALSE(ResolveRaw(""));
    EXPECT_FALSE(ResolveRaw("text only"));
    EXPECT_FALSE(ResolveRaw("<!-- only a comment -->"));
    // テキストの途中で終わる SVG。
    for (const std::string_view cut : { "<svg", "<svg ", "<svg/", "<svg a", "<svg a=", "<svg a=\"1", "<svg a=\"1\"", "<svg>", "<svg><", "<svg></svg" }) {
        SCOPED_TRACE(cut);
        EXPECT_FALSE(ResolveRaw(cut));
    }
}

// 入れ子の svg は viewBox を x・y・width・height の枠へ写す。preserveAspectRatio の既定は縦横比を保って収め、中央に寄せる。
TEST_F(PeleSvgTest, NestedSvgViewportFollowsPreserveAspectRatio)
{
    struct Case {
        std::string_view attrs;
        std::optional<std::string_view> transform; // nullopt は図ごと描けない
    };
    constexpr Case kCases[] = {
        // 幅 40・高さ 20 の枠に 10x10 の viewBox。収める拡大率は 2 で、横に 20 余る。
        { R"svg(width="40" height="20" viewBox="0 0 10 10")svg", "matrix(2 0 0 2 20 20)" },
        { R"svg(width="40" height="20" viewBox="0 0 10 10" preserveAspectRatio="xMidYMid")svg", "matrix(2 0 0 2 20 20)" },
        { R"svg(width="40" height="20" viewBox="0 0 10 10" preserveAspectRatio="xMinYMin")svg", "matrix(2 0 0 2 10 20)" },
        { R"svg(width="40" height="20" viewBox="0 0 10 10" preserveAspectRatio="xMaxYMax meet")svg", "matrix(2 0 0 2 30 20)" },
        { R"svg(width="40" height="20" viewBox="0 0 10 10" preserveAspectRatio=" xMaxYMin  meet ")svg", "matrix(2 0 0 2 30 20)" },
        // slice は枠を覆う拡大率 4 で、縦に 20 はみ出す。
        { R"svg(width="40" height="20" viewBox="0 0 10 10" preserveAspectRatio="xMidYMid slice")svg", "matrix(4 0 0 4 10 10)" },
        { R"svg(width="40" height="20" viewBox="0 0 10 10" preserveAspectRatio="xMinYMin  slice")svg", "matrix(4 0 0 4 10 20)" },
        { R"svg(width="40" height="20" viewBox="0 0 10 10" preserveAspectRatio="xMaxYMax slice")svg", "matrix(4 0 0 4 10 0)" },
        { R"svg(width="40" height="20" viewBox="0 0 10 10" preserveAspectRatio="xMinYMin slicex")svg", "matrix(2 0 0 2 10 20)" },
        // none は縦と横を別々に伸ばす。
        { R"svg(width="40" height="20" viewBox="0 0 10 10" preserveAspectRatio="none")svg", "matrix(4 0 0 2 10 20)" },
        // 縦長の枠では縦に余る。
        { R"svg(width="20" height="40" viewBox="0 0 10 10")svg", "matrix(2 0 0 2 10 30)" },
        { R"svg(width="20" height="40" viewBox="0 0 10 10" preserveAspectRatio="xMidYMin")svg", "matrix(2 0 0 2 10 20)" },
        { R"svg(width="20" height="40" viewBox="0 0 10 10" preserveAspectRatio="xMidYMax")svg", "matrix(2 0 0 2 10 40)" },
        // viewBox の原点は枠の左上へ写る。
        { R"svg(width="40" height="20" viewBox="5 5 10 10" preserveAspectRatio="xMinYMin")svg", "matrix(2 0 0 2 0 10)" },
        { R"svg(width="40" height="20" viewBox="5,5,10,10" preserveAspectRatio="none")svg", "matrix(4 0 0 2 -10 10)" },
        // viewBox が無ければ平行移動だけ。
        { R"svg(width="40" height="20")svg", "translate(10 20)" },
        { R"svg(width="40" height="20" viewBox="0 0 10")svg", std::nullopt },
        { R"svg(width="40" height="20" viewBox="0 0 10 10 10")svg", std::nullopt },
        { R"svg(width="40" height="20" viewBox="0 0 0 10")svg", std::nullopt },
        { R"svg(width="40" height="20" viewBox="0 0 10 0")svg", std::nullopt },
        { R"svg(width="40" height="20" viewBox="0 0 -10 10")svg", std::nullopt },
        { R"svg(width="40" height="20" viewBox="a b c d")svg", std::nullopt },
        { R"svg(width="40" viewBox="0 0 10 10")svg", std::nullopt },
        { R"svg(height="20" viewBox="0 0 10 10")svg", std::nullopt },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.attrs);
        const auto shapes = ShapesOf(std::format(R"svg(<svg x="10" y="20" {}><path d="M0 0L5 5"/></svg>)svg", c.attrs));
        ASSERT_EQ(shapes.has_value(), c.transform.has_value());
        if (shapes) {
            EXPECT_EQ(*shapes, std::format(R"svg(<path d="M0 0L5 5" transform="{}" fill="#1f1f1f"/>)svg", *c.transform));
        }
    }
}

// 根の svg は width と height が図の大きさ (1/64 px に切り捨ててから切り上げ) で、viewBox をその大きさへ写す。
TEST_F(PeleSvgTest, RootSvgSizeAndViewBox)
{
    const auto resolve = [&](std::string_view attrs) {
        return ResolveRaw(std::format(R"svg(<svg xmlns="http://www.w3.org/2000/svg" {}><rect x="0" y="0" width="10" height="10" fill="red"/></svg>)svg", attrs));
    };
    const auto scaled = resolve(R"svg(width="100" height="50" viewBox="0 0 200 100")svg");
    ASSERT_TRUE(scaled);
    EXPECT_EQ(scaled->width, 100.0f);
    EXPECT_EQ(scaled->height, 50.0f);
    ASSERT_EQ(scaled->layers.size(), 1u);
    EXPECT_EQ(scaled->layers[0].shapes, R"svg(<rect x="0" y="0" width="10" height="10" transform="matrix(0.5 0 0 0.5 0 0)" fill="#ff0000"/>)svg");

    const auto plain = resolve(R"svg(width="100.4" height="50.02")svg");
    ASSERT_TRUE(plain);
    EXPECT_EQ(plain->width, 101.0f);
    EXPECT_EQ(plain->height, 51.0f);
    EXPECT_EQ(plain->layers[0].shapes, R"svg(<rect x="0" y="0" width="10" height="10" fill="#ff0000"/>)svg");
    // 1/64 px に満たない端数は切り捨てるので、切り上げない。
    const auto tiny_fraction = resolve(R"svg(width="100.01" height="50.015")svg");
    ASSERT_TRUE(tiny_fraction);
    EXPECT_EQ(tiny_fraction->width, 100.0f);
    EXPECT_EQ(tiny_fraction->height, 50.0f);

    for (const std::string_view attrs : {
             R"svg(height="50")svg", R"svg(width="100")svg", R"svg(width="0" height="50")svg", R"svg(width="100" height="0")svg",
             R"svg(width="-1" height="50")svg", R"svg(width="x" height="50")svg", R"svg(width="100" height="50" viewBox="0 0 0 0")svg" }) {
        SCOPED_TRACE(attrs);
        EXPECT_FALSE(resolve(attrs));
    }
    EXPECT_FALSE(ResolveRaw(R"svg(<g width="100" height="50"><rect x="0" y="0" width="10" height="10"/></g>)svg")) << "根が svg でない";
}

// 図形の要素ごとに書き出す属性。大きさが 0 の図形は書かない。読み飛ばす要素の中身は描かず、扱えない要素があれば図ごと描かない。
TEST_F(PeleSvgTest, WritesEachShapeElement)
{
    struct Case {
        std::string_view body;
        std::optional<std::string_view> shapes;
    };
    constexpr Case kCases[] = {
        { R"svg(<rect x="1" y="2" width="3" height="4"/>)svg", R"svg(<rect x="1" y="2" width="3" height="4" fill="#1f1f1f"/>)svg" },
        { R"svg(<rect width="3" height="4"/>)svg", R"svg(<rect x="0" y="0" width="3" height="4" fill="#1f1f1f"/>)svg" },
        { R"svg(<rect x="1em" y="2" width="2em" height="4"/>)svg", R"svg(<rect x="16" y="2" width="32" height="4" fill="#1f1f1f"/>)svg" },
        { R"svg(<rect x="1" y="2" width="3" height="4" rx="1"/>)svg", R"svg(<rect x="1" y="2" width="3" height="4" rx="1" ry="1" fill="#1f1f1f"/>)svg" },
        { R"svg(<rect x="1" y="2" width="0" height="4"/>)svg", "" },
        { R"svg(<rect x="1" y="2" width="3" height="0"/>)svg", "" },
        { R"svg(<rect x="1" y="2" width="3"/>)svg", "" },
        { R"svg(<rect x="1" y="2" width="-3" height="4"/>)svg", "" },
        { R"svg(<circle cx="5" cy="6" r="7"/>)svg", R"svg(<circle cx="5" cy="6" r="7" fill="#1f1f1f"/>)svg" },
        { R"svg(<circle r="7"/>)svg", R"svg(<circle cx="0" cy="0" r="7" fill="#1f1f1f"/>)svg" },
        { R"svg(<circle cx="5" cy="6" r="0"/>)svg", "" },
        { R"svg(<circle cx="5" cy="6"/>)svg", "" },
        { R"svg(<ellipse cx="5" cy="6" rx="7" ry="8"/>)svg", R"svg(<ellipse cx="5" cy="6" rx="7" ry="8" fill="#1f1f1f"/>)svg" },
        // rx・ry の片方だけなら、もう片方も同じ値。
        { R"svg(<ellipse cx="5" cy="6" rx="7"/>)svg", R"svg(<ellipse cx="5" cy="6" rx="7" ry="7" fill="#1f1f1f"/>)svg" },
        { R"svg(<ellipse cx="5" cy="6" ry="8"/>)svg", R"svg(<ellipse cx="5" cy="6" rx="8" ry="8" fill="#1f1f1f"/>)svg" },
        { R"svg(<ellipse cx="5" cy="6"/>)svg", "" },
        { R"svg(<ellipse cx="5" cy="6" rx="0" ry="8"/>)svg", "" },
        { R"svg(<ellipse cx="5" cy="6" rx="7" ry="0"/>)svg", "" },
        { R"svg(<line x1="1" y1="2" x2="3" y2="4" stroke="red"/>)svg", R"svg(<line x1="1" y1="2" x2="3" y2="4" fill="#1f1f1f" stroke="#ff0000"/>)svg" },
        { R"svg(<polygon points="0,0 10,0 5,5"/>)svg", R"svg(<polygon points="0,0 10,0 5,5" fill="#1f1f1f"/>)svg" },
        { R"svg(<polyline points="0,0 10,0 5,5" fill="none" stroke="red"/>)svg", R"svg(<polyline points="0,0 10,0 5,5" fill="none" stroke="#ff0000"/>)svg" },
        { R"svg(<path d="M0 0L5 5"/>)svg", R"svg(<path d="M0 0L5 5" fill="#1f1f1f"/>)svg" },
        // g と a は中身を描く。defs・title・desc・metadata・symbol・グラデーションの定義は読み飛ばす。
        { R"svg(<a href="x"><g><circle r="7"/></g></a>)svg", R"svg(<circle cx="0" cy="0" r="7" fill="#1f1f1f"/>)svg" },
        { R"svg(<defs><circle r="7"/></defs><title>t</title><desc>d</desc><metadata><x/></metadata><symbol><circle r="7"/></symbol>)svg", "" },
        { R"svg(<linearGradient><circle r="7"/></linearGradient><radialGradient><circle r="7"/></radialGradient>)svg", "" },
        { R"svg(<circle r="7"/><image href="x"/>)svg", std::nullopt },
        { R"svg(<use href="#x"/>)svg", std::nullopt },
        { R"svg(<foreignObject><div/></foreignObject>)svg", std::nullopt },
        { R"svg(<style>.a{}</style>)svg", std::nullopt },
        { R"svg(<g><unknown/></g>)svg", std::nullopt },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.body);
        EXPECT_EQ(ShapesOf(c.body), c.shapes);
    }
}

// 図形の範囲 (層を分ける判断に使う) は、要素ごとの寸法から求める。文字 (50, 50) に掛かる図形だけが次の層へ行く。
TEST_F(PeleSvgTest, ShapeBoundsDecideLayerSplit)
{
    struct Case {
        std::string_view shape;
        bool covers;
    };
    constexpr Case kCases[] = {
        { R"svg(<rect x="40" y="40" width="20" height="20"/>)svg", true },
        { R"svg(<rect x="20" y="40" width="20" height="20"/>)svg", false },
        { R"svg(<rect x="40" y="20" width="20" height="20"/>)svg", false },
        { R"svg(<circle cx="61" cy="50" r="12"/>)svg", true },
        { R"svg(<circle cx="39" cy="50" r="12"/>)svg", true },
        { R"svg(<circle cx="50" cy="61" r="12"/>)svg", true },
        { R"svg(<circle cx="50" cy="39" r="12"/>)svg", true },
        { R"svg(<circle cx="61" cy="50" r="8"/>)svg", false },
        { R"svg(<circle cx="39" cy="50" r="8"/>)svg", false },
        { R"svg(<circle cx="50" cy="61" r="8"/>)svg", false },
        { R"svg(<circle cx="50" cy="39" r="8"/>)svg", false },
        { R"svg(<ellipse cx="61" cy="50" rx="12" ry="3"/>)svg", true },
        { R"svg(<ellipse cx="39" cy="50" rx="12" ry="3"/>)svg", true },
        { R"svg(<ellipse cx="50" cy="61" rx="3" ry="12"/>)svg", true },
        { R"svg(<ellipse cx="50" cy="39" rx="3" ry="12"/>)svg", true },
        { R"svg(<ellipse cx="61" cy="50" rx="8" ry="30"/>)svg", false },
        { R"svg(<ellipse cx="39" cy="50" rx="8" ry="30"/>)svg", false },
        { R"svg(<ellipse cx="50" cy="61" rx="30" ry="8"/>)svg", false },
        { R"svg(<ellipse cx="50" cy="39" rx="30" ry="8"/>)svg", false },
        { R"svg(<line x1="40" y1="40" x2="60" y2="60"/>)svg", true },
        { R"svg(<line x1="60" y1="60" x2="40" y2="40"/>)svg", true },
        { R"svg(<line x1="55" y1="40" x2="70" y2="60"/>)svg", false },
        { R"svg(<line x1="40" y1="55" x2="60" y2="70"/>)svg", false },
        { R"svg(<line x1="30" y1="40" x2="45" y2="60"/>)svg", false },
        { R"svg(<line x1="40" y1="30" x2="60" y2="45"/>)svg", false },
        // 変換した図形は、変換した後の範囲で比べる。
        { R"svg(<rect transform="translate(30 30)" x="10" y="10" width="20" height="20"/>)svg", true },
        { R"svg(<rect transform="translate(30 30)" x="40" y="40" width="20" height="20"/>)svg", false },
        // 線の太さの分だけ範囲を広げる。
        { R"svg(<rect x="54" y="40" width="20" height="20" stroke="red" stroke-width="10"/>)svg", true },
        { R"svg(<rect x="54" y="40" width="20" height="20" stroke="red" stroke-width="1"/>)svg", false },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.shape);
        EXPECT_EQ(SplitsLayer(std::format(R"svg(<text x="50" y="50" font-size="1">.</text>{})svg", c.shape)), c.covers);
    }
}

// 塗りの url(#id) はグラデーションの定義を指すときだけ使う。指す先が無いか別の要素なら、後ろに書いた色、それも無ければ塗らない。
TEST_F(PeleSvgTest, PaintUrlFallsBackWhenNotGradient)
{
    constexpr std::string_view kDefs = R"svg(<defs><linearGradient id="lin"><stop offset="0" stop-color="red"/></linearGradient>)svg"
                                       R"svg(<radialGradient id="rad"><stop offset="1" stop-color="blue"/></radialGradient></defs>)svg"
                                       R"svg(<g id="group"/>)svg";
    struct Case {
        std::string_view fill;
        std::string_view written;
    };
    constexpr Case kCases[] = {
        { "url(#lin)", "url(#g0)" },   { "url(#rad)", "url(#g0)" },    { "url(#lin) blue", "url(#g0)" }, { "url(#missing)", "none" },
        { "url(#missing) blue", "#0000ff" }, { "url(#group)", "none" }, { "url(#group) blue", "#0000ff" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.fill);
        const auto drawing = Resolve(std::format(R"svg({}<rect x="0" y="0" width="10" height="10" fill="{}"/>)svg", kDefs, c.fill));
        ASSERT_TRUE(drawing);
        ASSERT_EQ(drawing->layers.size(), 1u);
        EXPECT_EQ(drawing->layers[0].shapes, std::format(R"svg(<rect x="0" y="0" width="10" height="10" fill="{}"/>)svg", c.written));
        EXPECT_EQ(drawing->defs.empty(), !c.written.starts_with("url"));
    }
    // 同じ定義を同じ不透明度で 2 回使えば、書き出す定義は 1 つ。不透明度が違えば別の定義にする。
    const auto shared = Resolve(std::format(
        R"svg({}<rect x="0" y="0" width="10" height="10" fill="url(#lin)"/><rect x="0" y="0" width="10" height="10" fill="url(#lin)"/>)svg"
        R"svg(<rect x="0" y="0" width="10" height="10" fill="url(#lin)" fill-opacity="0.5"/>)svg",
        kDefs));
    ASSERT_TRUE(shared);
    const std::string& shapes = shared->layers[0].shapes;
    EXPECT_NE(shapes.find(R"svg(fill="url(#g0)"/><rect x="0" y="0" width="10" height="10" fill="url(#g0)"/>)svg"), std::string::npos) << shapes;
    EXPECT_NE(shapes.find(R"svg(fill="url(#g1)")svg"), std::string::npos) << shapes;
    EXPECT_EQ(shared->defs, R"svg(<linearGradient id="g0"><stop offset="0" stop-color="#ff0000" stop-opacity="1"/></linearGradient>)svg"
                            R"svg(<linearGradient id="g1"><stop offset="0" stop-color="#ff0000" stop-opacity="0.5"/></linearGradient>)svg");
}

// 同じ性質を 2 回書くと後ろの宣言が勝つ。後ろの値が読めなければ、その宣言だけを無かったものとして前の宣言を使う。
// 期待値は、宣言を 1 つだけ書いたときの結果。
TEST_F(PeleSvgTest, LaterValidDeclarationWinsPerProperty)
{
    struct Case {
        std::string_view context; // 性質の効き目を出力に出すための宣言
        std::string_view prop;
        std::string_view a, b, invalid;
    };
    constexpr Case kShapeCases[] = {
        { "stroke:red", "stroke-width", "2", "3", "-1" },
        { "stroke:red", "stroke-width", "2", "3", "x" },
        { "stroke:red;stroke-dasharray:4 2", "stroke-dashoffset", "3", "5", "x" },
        { "stroke:red", "stroke-miterlimit", "2", "3", "0.5" },
        { "stroke:red", "stroke-miterlimit", "2", "3", "x" },
        { "stroke:red", "stroke-dasharray", "4 2", "1 3", "4 -2" },
        { "stroke:red", "stroke-dasharray", "4 2", "4 0", "x" },
        { "stroke:red", "stroke-dasharray", "4 2", "0 4", "x" },
        { "stroke:red", "stroke-dasharray", "4 2", "none", "x" },
        { "stroke:red", "stroke-linecap", "round", "square", "x" },
        { "stroke:red", "stroke-linejoin", "round", "bevel", "x" },
        { "", "fill-rule", "evenodd", "nonzero", "x" },
        { "", "fill-rule", "nonzero", "evenodd", "x" },
        { "", "fill-opacity", "0.5", "0.25", "x" },
        { "stroke:red", "stroke-opacity", "0.5", "0.25", "x" },
        { "", "opacity", "0.5", "0.25", "x" },
        { "", "visibility", "hidden", "visible", "x" },
        { "", "visibility", "visible", "hidden", "x" },
        { "", "rx", "3", "5", "-1" },
        { "", "rx", "3", "auto", "x" },
        { "", "ry", "3", "5", "-1" },
        { "", "fill", "red", "blue", "bogus" },
        { "", "stroke", "red", "blue", "bogus" },
        { "fill:currentColor", "color", "red", "blue", "bogus" },
        { "stroke:red;stroke-width:1em", "font-size", "20px", "10px", "x" },
        { "stroke:red;stroke-width:1em", "font-size", "20px", "10px", "-5px" },
    };
    // render は宣言の並びから結果を作る (図形は書き出した SVG、文字は描画命令)。
    const auto expect_later_wins = [](const Case& c, auto&& render) {
        SCOPED_TRACE(testing::Message() << c.prop << ": " << c.a << " / " << c.b << " / " << c.invalid);
        const auto only_a = render(std::format("{}:{}", c.prop, c.a));
        const auto only_b = render(std::format("{}:{}", c.prop, c.b));
        ASSERT_TRUE(only_a && only_b);
        ASSERT_FALSE(*only_a == *only_b);
        EXPECT_TRUE(render(std::format("{0}:{1};{0}:{2}", c.prop, c.a, c.b)) == only_b);
        EXPECT_TRUE(render(std::format("{0}:{1};{0}:{2}", c.prop, c.b, c.a)) == only_a);
        EXPECT_TRUE(render(std::format("{0}:{1};{0}:{2}", c.prop, c.a, c.invalid)) == only_a);
        EXPECT_TRUE(render(std::format("{0}:{1};{0}:{2}", c.prop, c.b, c.invalid)) == only_b);
    };
    for (const auto& c : kShapeCases) {
        expect_later_wins(c, [&](std::string_view declarations) {
            return ShapesOf(std::format(R"svg(<rect x="0" y="0" width="50" height="50" style="{};{}"/>)svg", c.context, declarations));
        });
    }

    constexpr Case kTextCases[] = {
        { "", "font-weight", "bold", "600", "x" },   { "", "font-weight", "600", "lighter", "0" }, { "", "font-style", "italic", "normal", "x" },
        { "", "text-anchor", "end", "middle", "x" }, { "", "font-size", "20px", "10px", "x" },     { "", "fill", "red", "blue", "bogus" },
        { "", "fill-opacity", "0.5", "0.25", "x" },  { "", "opacity", "0.5", "0.25", "x" },
    };
    for (const auto& c : kTextCases) {
        expect_later_wins(c, [&](std::string_view declarations) {
            return OnlyTextOf(std::format(R"svg(<text x="100" y="20" style="{}">AB</text>)svg", declarations));
        });
    }
}

// inherit も読める値で、前の宣言に勝つ。継承しない性質 (opacity・rx・グラデーションの止まりの色) は親の値を写す。
TEST_F(PeleSvgTest, InheritKeywordTakesParentValue)
{
    const auto shapes = [&](std::string_view parent, std::string_view own) {
        return ShapesOf(std::format(R"svg(<g style="{}"><rect x="0" y="0" width="50" height="50" style="{}"/></g>)svg", parent, own));
    };
    EXPECT_EQ(shapes("fill:blue", "fill:red;fill:inherit"), R"svg(<rect x="0" y="0" width="50" height="50" fill="#0000ff"/>)svg");
    EXPECT_EQ(shapes("fill:blue", "fill:red;fill:INHERIT !important"), R"svg(<rect x="0" y="0" width="50" height="50" fill="#0000ff"/>)svg");
    EXPECT_EQ(shapes("opacity:0.5", "opacity:0.8;opacity:inherit"), R"svg(<rect x="0" y="0" width="50" height="50" fill="#1f1f1f" opacity="0.25"/>)svg");
    EXPECT_EQ(shapes("rx:3", "rx:5;rx:inherit"), R"svg(<rect x="0" y="0" width="50" height="50" rx="3" ry="3" fill="#1f1f1f"/>)svg");
    EXPECT_EQ(shapes("ry:3", "ry:5;ry:inherit"), R"svg(<rect x="0" y="0" width="50" height="50" rx="3" ry="3" fill="#1f1f1f"/>)svg");

    // 止まりの親は根の svg とみなすので、inherit は初期値 (黒・不透明) になる。
    const auto stops = Resolve(R"svg(<linearGradient id="lin"><stop offset="0" stop-color="red" stop-opacity="0.5"/>)svg"
                               R"svg(<stop offset="1" stop-color="red" stop-opacity="0.5" style="stop-color:inherit;stop-opacity:inherit"/></linearGradient>)svg"
                               R"svg(<rect x="0" y="0" width="10" height="10" fill="url(#lin)"/>)svg");
    ASSERT_TRUE(stops);
    EXPECT_EQ(stops->defs, R"svg(<linearGradient id="g0"><stop offset="0" stop-color="#ff0000" stop-opacity="0.5"/>)svg"
                           R"svg(<stop offset="1" stop-color="#000000" stop-opacity="1"/></linearGradient>)svg");

    // baseline-shift は継承しないが、inherit で親のずらし方を写す (親の分と合わせて 2 回ずれる)。
    const auto shifted = Resolve(R"svg(<text x="0" y="20"><tspan style="baseline-shift:4px">A<tspan style="baseline-shift:inherit">B</tspan>)svg"
                                 R"svg(<tspan>C</tspan></tspan></text>)svg");
    ASSERT_TRUE(shifted);
    ASSERT_EQ(shifted->layers[0].texts.size(), 3u);
    EXPECT_EQ(shifted->layers[0].texts[0].at.y, 16.0f);
    EXPECT_EQ(shifted->layers[0].texts[1].at.y, 12.0f);
    EXPECT_EQ(shifted->layers[0].texts[2].at.y, 16.0f);
}

// font の一括指定は [斜体] [太さ] 大きさ[/行の高さ] 書体 の順に読む。書かなかった斜体と太さは normal に戻す。大きさが無ければ読めない。
TEST_F(PeleSvgTest, FontShorthandSetsStyleWeightSizeFamily)
{
    struct Case {
        std::string_view font;
        uint16_t weight;
        bool italic;
        float size;
        std::wstring_view family;
    };
    constexpr Case kCases[] = {
        { "italic bold 20px Arial", 700, true, 20.0f, L"Arial" },
        { "bold italic 20px Arial", 700, true, 20.0f, L"Arial" },
        { "oblique 20px Arial", 400, true, 20.0f, L"Arial" },
        { "normal 20px Arial", 400, false, 20.0f, L"Arial" },
        // 単位の無い数は太さで、大きさにはしない。
        { "600 20px Arial", 600, false, 20.0f, L"Arial" },
        { "italic 300 20px/1.2 Arial", 300, true, 20.0f, L"Arial" },
        { "20px/1.5 Arial, 'Yu Gothic'", 400, false, 20.0f, L"Arial, 'Yu Gothic'" },
        { "bold  2em   Arial", 700, false, 32.0f, L"Arial" },
        { "ITALIC BOLD large Arial", 700, true, 18.0f, L"Arial" },
        // 大きさが無い宣言は読めないので、親の値 (font-weight:600 と斜体) のまま。
        { "bold Arial", 600, true, 16.0f, L"\"Segoe UI\", \"Yu Gothic UI\"" },
        { "600 Arial", 600, true, 16.0f, L"\"Segoe UI\", \"Yu Gothic UI\"" },
        { "600/1.2 Arial", 600, true, 16.0f, L"\"Segoe UI\", \"Yu Gothic UI\"" },
        { "bold 20/1.2 Arial", 600, true, 16.0f, L"\"Segoe UI\", \"Yu Gothic UI\"" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.font);
        const auto text = OnlyTextOf(std::format(R"svg(<g style="font-weight:600;font-style:italic"><text x="0" y="20" style="font:{}">A</text></g>)svg", c.font));
        ASSERT_TRUE(text);
        EXPECT_EQ(text->weight, c.weight);
        EXPECT_EQ(text->italic, c.italic);
        EXPECT_EQ(text->at.font_size, c.size);
        EXPECT_EQ(text->family, c.family);
    }
}

// tspan の x・y は塊の始まり。指定の無い座標は前の文字から続け、文字の無い要素の指定は捨て、空白だけで消えた並びの指定は次の文字へ送る。
TEST_F(PeleSvgTest, PlacesTspansByOwnPositions)
{
    const float a = Width(L"A"), b = Width(L"B"), c = Width(L"C");
    struct Placed {
        std::wstring_view text;
        float x, y;
    };
    struct Case {
        std::string_view body;
        std::vector<Placed> placed;
    };
    const Case kCases[] = {
        { R"svg(<text x="10" y="20">A<tspan x="30">B</tspan><tspan y="40">C</tspan>D</text>)svg",
          { { L"A", 10.0f, 20.0f }, { L"B", 30.0f, 20.0f }, { L"C", 30.0f + b, 40.0f }, { L"D", 30.0f + b + c, 40.0f } } },
        { R"svg(<text x="10" y="20">A<tspan x="50" y="60"></tspan>B</text>)svg", { { L"A", 10.0f, 20.0f }, { L"B", 10.0f + a, 20.0f } } },
        { R"svg(<text><tspan x="10" y="20"> </tspan><tspan>B</tspan></text>)svg", { { L"B", 10.0f, 20.0f } } },
        { R"svg(<text><tspan x="10" y="20"> </tspan><tspan x="30">B</tspan></text>)svg", { { L"B", 30.0f, 20.0f } } },
        { R"svg(<text><tspan x="10" y="20"> </tspan><tspan y="40">B</tspan></text>)svg", { { L"B", 10.0f, 40.0f } } },
        // 並びの末尾の空白は除く。途中の並びの末尾の空白は残す。
        { R"svg(<text x="0" y="20">A <tspan>B </tspan></text>)svg", { { L"A ", 0.0f, 20.0f }, { L"B", Width(L"A "), 20.0f } } },
        { R"svg(<text x="0" y="20">A<tspan> </tspan> </text>)svg", { { L"A", 0.0f, 20.0f } } },
        // title と desc は描かない。display:none の tspan は無かったものとして詰める。
        { R"svg(<text x="0" y="20"><title>t</title>A<desc>d</desc></text>)svg", { { L"A", 0.0f, 20.0f } } },
        { R"svg(<text x="0" y="20">A<tspan style="display:none">B</tspan>C</text>)svg", { { L"A", 0.0f, 20.0f }, { L"C", a, 20.0f } } },
        // text-anchor の塊は幅の分 (middle は半分) だけ戻す。y だけ指定した次の塊は、前の塊の文字の終わりから始める。
        { R"svg(<text text-anchor="end" x="100" y="20">A<tspan y="40">B</tspan></text>)svg", { { L"A", 100.0f - a, 20.0f }, { L"B", 100.0f - b, 40.0f } } },
        { R"svg(<text text-anchor="middle" x="100" y="20">A<tspan y="40">B</tspan></text>)svg",
          { { L"A", 100.0f - a / 2.0f, 20.0f }, { L"B", 100.0f + a / 2.0f - b / 2.0f, 40.0f } } },
    };
    for (const auto& k : kCases) {
        SCOPED_TRACE(k.body);
        const auto drawing = Resolve(k.body);
        ASSERT_TRUE(drawing);
        ASSERT_EQ(drawing->layers.size(), 1u);
        const auto& texts = drawing->layers[0].texts;
        ASSERT_EQ(texts.size(), k.placed.size());
        for (size_t i = 0; i < texts.size(); ++i) {
            SCOPED_TRACE(i);
            EXPECT_EQ(texts[i].text, k.placed[i].text);
            EXPECT_NEAR(texts[i].at.x, k.placed[i].x, 0.01f);
            EXPECT_EQ(texts[i].at.y, k.placed[i].y);
        }
    }

    // 1 行に置けない指定 (文字ごとのずらしと回転)、tspan でない子、読めない座標があれば図ごと描かない。
    for (const std::string_view body : {
             R"svg(<text x="0" y="20" dx="1">A</text>)svg", R"svg(<text x="0" y="20" dy="1">A</text>)svg", R"svg(<text x="0" y="20" rotate="10">A</text>)svg",
             R"svg(<text x="0" y="20" textLength="50">A</text>)svg", R"svg(<text x="0" y="20"><tspan dx="1">A</tspan></text>)svg",
             R"svg(<text x="0" y="20"><a>A</a></text>)svg", R"svg(<text x="abc" y="20">A</text>)svg", R"svg(<text x="0" y="abc">A</text>)svg" }) {
        SCOPED_TRACE(body);
        EXPECT_FALSE(Resolve(body));
    }
}

// xml:space="preserve" は続く空白も前後の空白も残す (改行とタブは空白にする)。それ以外は畳む。tspan の不透明度は親と掛け合わせる。
TEST_F(PeleSvgTest, PreserveSpaceAndSpanOpacity)
{
    const auto text_of = [&](std::string_view attrs) {
        const auto text = OnlyTextOf(std::format("<text x=\"0\" y=\"20\" {}> a \t\n b </text>", attrs));
        return text ? text->text : std::wstring{ L"?" };
    };
    EXPECT_EQ(text_of(""), L"a b");
    EXPECT_EQ(text_of(R"svg(xml:space="default")svg"), L"a b");
    EXPECT_EQ(text_of(R"svg(xml:space="preserve")svg"), L" a    b ");

    const auto faded = Resolve(R"svg(<text x="0" y="20" opacity="0.5">A<tspan opacity="0.5">B</tspan></text>)svg");
    ASSERT_TRUE(faded);
    ASSERT_EQ(faded->layers[0].texts.size(), 2u);
    EXPECT_EQ(faded->layers[0].texts[0].at.alpha, 0.5f);
    EXPECT_EQ(faded->layers[0].texts[1].at.alpha, 0.25f);
}

// 文字の範囲 (層を分ける判断に使う) は、左端から幅の分と、ベースラインから上へ ascent・下へ descent の分。
TEST_F(PeleSvgTest, TextBoundsCoverAscentDescentAndWidth)
{
    const float width = Width(L"MMMM");
    ASSERT_GT(width, 40.0f);
    struct Case {
        std::string shape;
        bool covers;
    };
    const Case kCases[] = {
        // 文字の上半分だけ・ベースラインのすぐ下だけ・右端の手前だけに掛かる図形も、文字に重なる。
        { R"svg(<rect x="50" y="38" width="5" height="4"/>)svg", true },
        { R"svg(<rect x="50" y="51.5" width="5" height="1"/>)svg", true },
        { std::format(R"svg(<rect x="{}" y="45" width="3" height="3"/>)svg", 50.0f + width - 5.0f), true },
        // 外側。
        { R"svg(<rect x="50" y="20" width="5" height="5"/>)svg", false },
        { R"svg(<rect x="50" y="60" width="5" height="5"/>)svg", false },
        { std::format(R"svg(<rect x="{}" y="45" width="3" height="3"/>)svg", 50.0f + width + 3.0f), false },
        { R"svg(<rect x="40" y="45" width="5" height="3"/>)svg", false },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.shape);
        EXPECT_EQ(SplitsLayer(std::format(R"svg(<text x="50" y="50">MMMM</text>{})svg", c.shape)), c.covers);
    }
}

// 長さの書き方: 単位 (CSS の絶対単位と em・ex)、calc() の足し算と引き算、min()・max()。読めない長さの宣言は無かったものとする
// (線の太さは初期値の 1 のままなので書かない)。
TEST_F(PeleSvgTest, ResolvesLengthNotations)
{
    struct Case {
        std::string_view value;
        std::string_view written; // 空は読めない
    };
    constexpr Case kCases[] = {
        { "2", "2" },
        { "2px", "2" },
        { "2PX", "2" },
        { " 2px ", "2" },
        { "+2", "2" },
        { "2e0", "2" },
        { "0.5", "0.5" },
        // 文字の大きさは 16px。ex は em の半分とみなす。
        { "1em", "16" },
        { "1ex", "8" },
        { "3pt", "4" },
        { "0.5pc", "8" },
        { "0.5in", "48" },
        { "2.54cm", "96" },
        { "25.4mm", "96" },
        { "2%", "" },
        { "2xx", "" },
        { "2 px", "" },
        { "px", "" },
        { "inf", "" },
        { "nan", "" },
        { "calc(1px + 2px)", "3" },
        { "calc(5px - 2px)", "3" },
        { "calc(1em - 14px)", "2" },
        { "CALC(1px + 2px)", "3" },
        { "calc( 1px  +  2px )", "3" },
        { "calc(1px + 2px + 3px)", "6" },
        { "calc(10px - 2px - 3px)", "5" },
        { "calc(3px)", "3" },
        { "calc(calc(1px + 1px) + 1px)", "3" },
        { "calc(5px - calc(1px + 1px))", "3" },
        { "calc(1px +)", "" },
        { "calc(1px 2px)", "" },
        { "calc(+ 1px)", "" },
        { "calc(1px * 2)", "" },
        { "calc()", "" },
        { "calc(1px + x)", "" },
        { "calc(1px + 2px", "" },
        { "min(5px, 3px)", "3" },
        { "min(3px, 5px)", "3" },
        { "max(2px, 3px)", "3" },
        { "max(3px, 2px)", "3" },
        { "min(3px)", "3" },
        { "MAX(2px, 3px, 2.5px)", "3" },
        { "min(5px, max(2px, 3px))", "3" },
        { "min(calc(1px + 2px), 9px)", "3" },
        { "min()", "" },
        { "min(3px, x)", "" },
        { "min(3px", "" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.value);
        const auto shapes = ShapesOf(std::format(R"svg(<rect x="0" y="0" width="50" height="50" style="stroke:red;stroke-width:{}"/>)svg", c.value));
        const std::string width = c.written.empty() ? std::string{} : std::format(R"svg( stroke-width="{}")svg", c.written);
        EXPECT_EQ(shapes, std::format(R"svg(<rect x="0" y="0" width="50" height="50" fill="#1f1f1f" stroke="#ff0000"{}/>)svg", width));
    }
}

// hsl() の色相の単位と範囲の外の値、color-mix() の百分率の置き方。
TEST_F(PeleSvgTest, ResolvesHslUnitsAndColorMixWeights)
{
    struct Case {
        std::string_view color;
        std::string_view written;
    };
    constexpr Case kCases[] = {
        // 色相は 30 度ごとに成分が入れ替わる。
        { "hsl(30 100% 50%)", R"svg(fill="#ff8000")svg" },
        { "hsl(150 100% 50%)", R"svg(fill="#00ff80")svg" },
        { "hsl(210 100% 50%)", R"svg(fill="#0080ff")svg" },
        { "hsl(270 100% 50%)", R"svg(fill="#8000ff")svg" },
        { "hsl(330 100% 50%)", R"svg(fill="#ff0080")svg" },
        { "hsl(0 100% 25%)", R"svg(fill="#800000")svg" },
        { "hsl(0 100% 75%)", R"svg(fill="#ff8080")svg" },
        { "hsl(0 0% 100%)", R"svg(fill="#ffffff")svg" },
        { "hsl(0 0% 0%)", R"svg(fill="#000000")svg" },
        { "hsl(120deg 100% 50%)", R"svg(fill="#00ff00")svg" },
        { "hsl(0.25turn 100% 50%)", R"svg(fill="#80ff00")svg" },
        { "hsl(3.14159265rad 100% 50%)", R"svg(fill="#00ffff")svg" },
        { "hsl(480 100% 50%)", R"svg(fill="#00ff00")svg" },
        // 彩度と明度は % が無ければ 0〜100 の数。範囲の外は端に寄せる。
        { "hsl(120 100 50)", R"svg(fill="#00ff00")svg" },
        { "hsl(120 200% 50%)", R"svg(fill="#00ff00")svg" },
        { "hsl(120 -10% 50%)", R"svg(fill="#808080")svg" },
        { "hsl(120 100% 150%)", R"svg(fill="#ffffff")svg" },
        { "hsl(120 100% -10%)", R"svg(fill="#000000")svg" },
        { "hsl(120 100% 50% / 50%)", R"svg(fill="#00ff00" fill-opacity="0.5")svg" },
        { "hsl(120, 100%, 50%, 0.5)", R"svg(fill="#00ff00" fill-opacity="0.5")svg" },
        { "hsl(120grad 100% 50%)", R"svg(fill="#1f1f1f")svg" },
        { "hsl(x 100% 50%)", R"svg(fill="#1f1f1f")svg" },
        { "hsl(120 x 50%)", R"svg(fill="#1f1f1f")svg" },
        { "hsl(120 100% x)", R"svg(fill="#1f1f1f")svg" },
        { "hsl(120 100%)", R"svg(fill="#1f1f1f")svg" },
        { "hsl(120 100% 50% / x)", R"svg(fill="#1f1f1f")svg" },
        { "hsl(120 100% 50% / 0.5 0.5)", R"svg(fill="#1f1f1f")svg" },
        // 百分率は色の前にも書ける。合計が 100% を超えたら割合だけを使う。
        { "color-mix(in srgb, 25% red, blue)", R"svg(fill="#4000bf")svg" },
        { "color-mix(in srgb, red, 25% blue)", R"svg(fill="#bf0040")svg" },
        { "color-mix(IN SRGB, red, blue)", R"svg(fill="#800080")svg" },
        { "color-mix(in srgb, red 60%, blue 60%)", R"svg(fill="#800080")svg" },
        { "color-mix(in srgb, red 150%, blue 50%)", R"svg(fill="#bf0040")svg" },
        { "color-mix(in srgb, red 100%, blue 0%)", R"svg(fill="#ff0000")svg" },
        { "color-mix(in srgb, red 0%, blue)", R"svg(fill="#0000ff")svg" },
        { "color-mix(in srgb, red, blue 0%)", R"svg(fill="#ff0000")svg" },
        // 透明と混ぜても色は濁らず、不透明度だけが下がる。
        { "color-mix(in srgb, transparent, red)", R"svg(fill="#ff0000" fill-opacity="0.5")svg" },
        { "color-mix(in srgb, transparent, transparent)", R"svg(fill="#000000" fill-opacity="0")svg" },
        { "color-mix(in srgb, color-mix(in srgb, red, blue), black)", R"svg(fill="#400040")svg" },
        { "color-mix(in srgb, red, blue, green)", R"svg(fill="#1f1f1f")svg" },
        { "color-mix(in srgb, red, bogus)", R"svg(fill="#1f1f1f")svg" },
        { "color-mix(in srgb, bogus, red)", R"svg(fill="#1f1f1f")svg" },
        { "color-mix(red, blue)", R"svg(fill="#1f1f1f")svg" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.color);
        EXPECT_EQ(ShapesOf(std::format(R"svg(<rect x="0" y="0" width="10" height="10" fill="{}"/>)svg", c.color)),
                  std::format(R"svg(<rect x="0" y="0" width="10" height="10" {}/>)svg", c.written));
    }
}

// var() は祖先の宣言から値を引く。参照先が無いか、たどった先が解けなければ代わりの値を使い、それも無ければ宣言ごと読めない。
TEST_F(PeleSvgTest, SubstitutesCustomProperties)
{
    constexpr std::string_view kVars = "--a:red;--b:var(--a);--w:3px;--loop:var(--loop);--broken:var(--missing);--pair:4 2";
    struct Case {
        std::string_view style;
        std::string_view written;
    };
    constexpr Case kCases[] = {
        { "fill:var(--a)", R"svg( fill="#ff0000")svg" },
        { "fill:var( --a )", R"svg( fill="#ff0000")svg" },
        { "fill:VAR(--a)", R"svg( fill="#ff0000")svg" },
        { "fill:var(--b)", R"svg( fill="#ff0000")svg" },
        { "fill:var(--a, blue)", R"svg( fill="#ff0000")svg" },
        { "fill:var(--missing, blue)", R"svg( fill="#0000ff")svg" },
        { "fill:var(--missing, var(--a))", R"svg( fill="#ff0000")svg" },
        { "fill:var(--missing, var(--missing2, blue))", R"svg( fill="#0000ff")svg" },
        // 代わりの値は最初の , から後ろ全部 (値の中の , では切らない)。
        { "fill:var(--missing, rgb(0, 0, 255))", R"svg( fill="#0000ff")svg" },
        { "fill:var(--broken, blue)", R"svg( fill="#0000ff")svg" },
        { "fill:var(--loop, blue)", R"svg( fill="#0000ff")svg" },
        { "fill:var(--missing)", R"svg( fill="#1f1f1f")svg" },
        { "fill:var(--broken)", R"svg( fill="#1f1f1f")svg" },
        { "fill:var(--loop)", R"svg( fill="#1f1f1f")svg" },
        { "fill:var(--a", R"svg( fill="#1f1f1f")svg" },
        // 名前は大文字と小文字を区別する。
        { "fill:var(--A, blue)", R"svg( fill="#0000ff")svg" },
        // 値の一部としても、1 つの宣言に何度でも書ける。
        { "stroke:var(--a);stroke-width:calc(var(--w) + 1px)", R"svg( fill="#1f1f1f" stroke="#ff0000" stroke-width="4")svg" },
        { "stroke:var(--a);stroke-dasharray:var(--w) var(--w)", R"svg( fill="#1f1f1f" stroke="#ff0000" stroke-dasharray="3 3")svg" },
        { "stroke:var(--a);stroke-dasharray:var(--pair)", R"svg( fill="#1f1f1f" stroke="#ff0000" stroke-dasharray="4 2")svg" },
        { "fill:color-mix(in srgb, var(--a), var(--missing, blue))", R"svg( fill="#800080")svg" },
        // 自分の宣言が祖先の宣言に勝つ。
        { "--a:blue;fill:var(--a)", R"svg( fill="#0000ff")svg" },
        { "--a:blue;fill:var(--b)", R"svg( fill="#0000ff")svg" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.style);
        EXPECT_EQ(ShapesOf(std::format(R"svg(<g style="{}"><rect x="0" y="0" width="50" height="50" style="{}"/></g>)svg", kVars, c.style)),
                  std::format(R"svg(<rect x="0" y="0" width="50" height="50"{}/>)svg", c.written));
    }
    // 子で上書きした変数は、兄弟には及ばない。
    EXPECT_EQ(ShapesOf(std::format(R"svg(<g style="{}"><g style="--a:blue"><rect x="0" y="0" width="5" height="5" fill="var(--a)"/></g>)svg"
                                   R"svg(<rect x="0" y="0" width="5" height="5" fill="var(--a)"/></g>)svg",
                                   kVars)),
              R"svg(<rect x="0" y="0" width="5" height="5" fill="#0000ff"/><rect x="0" y="0" width="5" height="5" fill="#ff0000"/>)svg");
}
