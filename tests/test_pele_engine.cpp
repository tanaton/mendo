#include <gtest/gtest.h>
#include "diagram_raster.h"
#include "mermaid_util.h"
#include "pele_engine.h"
#include "pele_test_assets.h"
#include "string_convert.h"
#include "test_helpers.h"
#include "wic_util.h"
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using Status = PeleOutcome::Status;

// ランタイムの用意 (バイトコードの読み込みと評価、約 5ms と約 3ms) を 1 回で済ませるため、テストの間で 1 つのエンジンを使う。
// COM は描いた数式を WIC で読む MathDrawTest のため。
class PeleEngineTest : public ComApartmentTest {
protected:
    static void SetUpTestSuite()
    {
        ComApartmentTest::SetUpTestSuite();
        assets_ = PeleTestAssets::Load();
        ASSERT_TRUE(assets_.ok());
        ASSERT_HRESULT_SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw_.GetAddressOf())));
        engine_ = NewEngine();
    }
    static void TearDownTestSuite()
    {
        engine_.reset();
        dw_.Reset();
        ComApartmentTest::TearDownTestSuite();
    }
    static std::unique_ptr<PeleEngine> NewEngine()
    {
        return assets_.NewEngine(dw_.Get());
    }
    static std::shared_ptr<const MathDrawing> Math(std::string_view tex)
    {
        const PeleEngine::MathSource source{ tex, 16.0f };
        return engine_->RenderMaths({ &source, 1 })[0];
    }
    template <class T>
    static size_t CountItems(const MathDrawing& math)
    {
        return std::ranges::count_if(math.items, [](const MathDrawing::Item& item) { return std::holds_alternative<T>(item); });
    }
    template <class T>
    static std::vector<MathDrawing::Color> ColorsOf(const MathDrawing& math)
    {
        std::vector<MathDrawing::Color> out;
        for (const auto& item : math.items) {
            if (const auto* it = std::get_if<T>(&item)) {
                out.push_back(it->color);
            }
        }
        return out;
    }

    static inline PeleTestAssets assets_;
    static inline Microsoft::WRL::ComPtr<IDWriteFactory> dw_;
    static inline std::unique_ptr<PeleEngine> engine_;
};

} // namespace

// 図のコードから描画命令を作る。
TEST_F(PeleEngineTest, RendersDiagramToDrawing)
{
    const auto out = engine_->Render("flowchart LR\n  A[開始] --> B[終了]", false, 800.0f);
    ASSERT_EQ(out.status, Status::Drawn) << string_convert::WideToUtf8(out.error);
    EXPECT_GT(out.drawing.width, 0.0f);
    EXPECT_GT(out.drawing.height, 0.0f);
    std::wstring all;
    for (const auto& layer : out.drawing.layers) {
        for (const auto& t : layer.texts) {
            all += t.text;
        }
    }
    EXPECT_NE(all.find(L"開始"), std::wstring::npos);
    EXPECT_NE(all.find(L"終了"), std::wstring::npos);
}

// コードの誤りは pele の文言で返す。
TEST_F(PeleEngineTest, ReturnsPeleErrorForInvalidCode)
{
    const auto out = engine_->Render("flowchart LR\n  A[ --> ", false, 800.0f);
    EXPECT_EQ(out.status, Status::Error);
    EXPECT_FALSE(out.error.empty());
}

// ラベルの $$...$$ も KaTeX で描画命令にする。描けない TeX は pele がそのまま文字で書く。
TEST_F(PeleEngineTest, RendersMathLabelsWithKatex)
{
    const auto out = engine_->Render("flowchart LR\n  A[\"面積 $$\\frac{a}{b}$$\"] --> B[\"$$\\frac{a}$$\"]", false, 800.0f);
    ASSERT_EQ(out.status, Status::Drawn) << string_convert::WideToUtf8(out.error);
    std::vector<const mermaid_util::DrawingMath*> maths;
    std::wstring texts;
    for (const auto& layer : out.drawing.layers) {
        for (const auto& m : layer.maths) {
            maths.push_back(&m);
        }
        for (const auto& t : layer.texts) {
            texts += t.text;
        }
    }
    ASSERT_EQ(maths.size(), 1u);
    EXPECT_EQ(maths[0]->tex, "\\frac{a}{b}");
    ASSERT_NE(maths[0]->drawing, nullptr);
    EXPECT_GT(maths[0]->drawing->width, 0.0f);
    EXPECT_NE(texts.find(L"面積"), std::wstring::npos);
    EXPECT_NE(texts.find(L"\\frac{a}"), std::wstring::npos);
}

// C++ で作った描画命令はディスクキャッシュに書き、ParseDrawing で同じものに読み戻せる (下付き・太字・絵文字・円のノードも)。
// ディスクキャッシュの描画命令は TeX だけを持つので、読んだ後に数式を組み直す。
TEST_F(PeleEngineTest, SerializedDrawingParsesBackAndResolvesMaths)
{
    auto out = engine_->Render("flowchart TD\n  A[\"$$x^2$$\"] --> B[\"x<sub>2</sub> & <b>y</b> 🦆\"] --> C((円))", true, 800.0f);
    ASSERT_EQ(out.status, Status::Drawn);
    auto back = mermaid_util::ParseDrawing(string_convert::Utf8ToWide(mermaid_util::SerializeDrawing(out.drawing)));
    ASSERT_TRUE(back);
    EXPECT_EQ(*back, out.drawing);
    engine_->ResolveMaths(*back);
    size_t resolved = 0;
    for (const auto& layer : back->layers) {
        for (const auto& m : layer.maths) {
            ASSERT_NE(m.drawing, nullptr);
            ++resolved;
        }
    }
    EXPECT_EQ(resolved, 1u);
}

// インライン数式は同じ順で返し、TeX の誤りは null。分数の線は矩形になる。
TEST_F(PeleEngineTest, RendersInlineMaths)
{
    const PeleEngine::MathSource sources[]{ { "\\frac{a}{b}", 16.0f }, { "\\frac{a}", 16.0f }, { "\\frac{a}{b}", 32.0f } };
    const auto maths = engine_->RenderMaths(sources);
    ASSERT_EQ(maths.size(), 3u);
    ASSERT_NE(maths[0], nullptr);
    EXPECT_EQ(maths[1], nullptr);
    ASSERT_NE(maths[2], nullptr);
    EXPECT_GT(maths[0]->height, 0.0f);
    EXPECT_GT(maths[0]->depth, 0.0f);
    EXPECT_EQ(CountItems<MathDrawing::Rect>(*maths[0]), 1u);
    EXPECT_EQ(CountItems<MathDrawing::Glyphs>(*maths[0]), 2u);
    // KaTeX の CSS と同じく文字の大きさの 1.21 倍で組むので、大きさに比例する。
    EXPECT_NEAR(maths[2]->width, maths[0]->width * 2.0f, 0.01f);
}

// align などの自動の式番号は数式ごとに (1) から振り、式の右に置く。
TEST_F(PeleEngineTest, NumbersEquationsPerFormula)
{
    const auto numbered = Math("\\begin{align} a &= b \\\\ c &= d \\end{align}");
    const auto again = Math("\\begin{align} a &= b \\\\ c &= d \\end{align}");
    const auto plain = Math("\\begin{align*} a &= b \\\\ c &= d \\end{align*}");
    ASSERT_TRUE(numbered && again && plain);
    EXPECT_GT(numbered->width, plain->width + 16.0f);
    EXPECT_EQ(numbered->width, again->width) << "前の数式の番号を引き継がない";
}

// \color で指定した色は字・分数の線・SVG に付き、内側の指定が勝つ。指定の無い部分は色を持たない (描くときの本文の色)。
TEST_F(PeleEngineTest, AppliesTextColors)
{
    constexpr uint32_t kRed = 0xFF0000FF, kBlue = 0x0000FFFF, kGreen = 0x00FF00FF;
    const auto red = Math("\\color{red} x");
    const auto plain = Math("x");
    ASSERT_TRUE(red && plain);
    EXPECT_EQ(red->width, plain->width);
    EXPECT_EQ(ColorsOf<MathDrawing::Glyphs>(*red), (std::vector<MathDrawing::Color>{ kRed }));
    EXPECT_EQ(ColorsOf<MathDrawing::Glyphs>(*plain), (std::vector<MathDrawing::Color>{ std::nullopt }));

    const auto nested = Math("a \\textcolor{red}{b \\textcolor{blue}{c}}");
    ASSERT_NE(nested, nullptr);
    EXPECT_EQ(ColorsOf<MathDrawing::Glyphs>(*nested), (std::vector<MathDrawing::Color>{ std::nullopt, kRed, kBlue }));

    const auto frac = Math("\\textcolor{#0f0}{\\frac{1}{2}}");
    ASSERT_NE(frac, nullptr);
    EXPECT_EQ(ColorsOf<MathDrawing::Rect>(*frac), (std::vector<MathDrawing::Color>{ kGreen }));

    const auto sqrt = Math("\\color{blue}\\sqrt{x}");
    ASSERT_NE(sqrt, nullptr);
    const auto paths = ColorsOf<MathDrawing::Path>(*sqrt);
    ASSERT_FALSE(paths.empty());
    EXPECT_TRUE(std::ranges::all_of(paths, [&](const MathDrawing::Color& c) { return c == kBlue; }));
}

// \colorbox と \fcolorbox の背景は指定の色で、中の文字より先に描く (ダークモードでも指定どおり。#412)。枠は枠の色。
TEST_F(PeleEngineTest, DrawsColorBoxBackgrounds)
{
    constexpr uint32_t kRed = 0xFF0000FF, kYellow = 0xFFFF00FF;
    const auto first_index = [](const MathDrawing& math, auto&& pred) {
        const auto it = std::ranges::find_if(math.items, pred);
        return static_cast<size_t>(it - math.items.begin());
    };
    const auto is_fill = [](const MathDrawing::Item& item) {
        const auto* r = std::get_if<MathDrawing::Rect>(&item);
        return r && r->side == MathDrawing::Rect::Side::Fill;
    };
    const auto is_glyphs = [](const MathDrawing::Item& item) { return std::holds_alternative<MathDrawing::Glyphs>(item); };

    const auto box = Math("\\colorbox{yellow}{x}");
    ASSERT_NE(box, nullptr);
    ASSERT_EQ(CountItems<MathDrawing::Rect>(*box), 1u);
    const auto& fill = std::get<MathDrawing::Rect>(box->items[first_index(*box, is_fill)]);
    EXPECT_EQ(fill.color, kYellow);
    EXPECT_GT(fill.w, 0.0f);
    EXPECT_GT(fill.h, 0.0f);
    EXPECT_LT(first_index(*box, is_fill), first_index(*box, is_glyphs));

    const auto framed = Math("\\fcolorbox{red}{yellow}{x}");
    ASSERT_NE(framed, nullptr);
    size_t borders = 0;
    for (const auto& item : framed->items) {
        if (const auto* r = std::get_if<MathDrawing::Rect>(&item)) {
            EXPECT_EQ(r->color, r->side == MathDrawing::Rect::Side::Fill ? kYellow : kRed);
            borders += r->side != MathDrawing::Rect::Side::Fill;
        }
    }
    EXPECT_EQ(borders, 4u);
}

// KaTeX の書体に無い文字 (\text{} の日本語) は、システムのフォールバックの書体で描く。
TEST_F(PeleEngineTest, FallsBackForCharactersOutsideKatexFonts)
{
    const auto ja = Math("\\text{日本}");
    const auto en = Math("\\text{ab}");
    ASSERT_TRUE(ja && en);
    const auto face_of = [](const MathDrawing& math) {
        const auto it = std::ranges::find_if(math.items, [](const MathDrawing::Item& item) { return std::holds_alternative<MathDrawing::Glyphs>(item); });
        return it == math.items.end() ? nullptr : std::get<MathDrawing::Glyphs>(*it).face.Get();
    };
    ASSERT_NE(face_of(*ja), nullptr);
    EXPECT_NE(face_of(*ja), face_of(*en));
    EXPECT_GT(ja->width, 0.0f);
}

// 同時に呼ぶとランタイムを増やし、どちらも描ける。捨てた後もバイトコードから作り直して描ける。
TEST_F(PeleEngineTest, RendersConcurrentlyAndAfterTrim)
{
    PeleOutcome results[2];
    std::thread other([&] { results[0] = engine_->Render("pie\n  \"a\" : 1\n  \"b\" : 2", true, 400.0f); });
    results[1] = engine_->Render("sequenceDiagram\n  A->>B: hi", false, 800.0f);
    other.join();
    EXPECT_EQ(results[0].status, Status::Drawn);
    EXPECT_EQ(results[1].status, Status::Drawn);
    engine_->Trim();
    EXPECT_EQ(engine_->Render("flowchart LR\n  C --> D", false, 800.0f).status, Status::Drawn);
}

TEST_F(PeleEngineTest, CancelStopsRendering)
{
    const auto engine = NewEngine();
    engine->Cancel();
    EXPECT_EQ(engine->Render("flowchart LR\n  A --> B", false, 800.0f).status, Status::Cancelled);
    const PeleEngine::MathSource source{ "x", 16.0f };
    EXPECT_EQ(engine->RenderMaths({ &source, 1 })[0], nullptr);
}

// 拡大表示とクリップボードは本文の幅に縮めない。
TEST_F(PeleEngineTest, KeepsNaturalSizeWhenNotFitting)
{
    constexpr std::string_view kWide = "flowchart LR\n  A[aaaaaaaaaaaaaaaa] --> B[bbbbbbbbbbbbbbbb] --> C[cccccccccccccccc] --> D[dddddddddddddddd]";
    const auto fitted = engine_->Render(kWide, false, 200.0f);
    const auto natural = engine_->Render(kWide, false, 200.0f, false);
    ASSERT_EQ(fitted.status, Status::Drawn);
    ASSERT_EQ(natural.status, Status::Drawn);
    EXPECT_LE(fitted.drawing.width, 200.0f);
    EXPECT_GT(natural.drawing.width, 200.0f);
}

// 覚えておいた pele の出力をテーマか fit を変えて解き直しても、初めて描いたときと同じになる。
// 最初はライト・fit ありで描くので、テーマだけ・fit だけを変えた 2 通りで足りる (エンジンを作るのは 1 回 25ms ほど)。
TEST_F(PeleEngineTest, ReusedPeleOutputMatchesFreshRender)
{
    constexpr std::string_view kCode = "flowchart LR\n  A[\"面積 $$x^2$$\"] --> B[bbbbbbbbbbbbbbbbbbbbbbbb] --> C[cccccccccccccccc]";
    ASSERT_EQ(engine_->Render(kCode, false, 300.0f).status, Status::Drawn);
    constexpr std::pair<bool, bool> kVariants[]{
        { true,  true  },
        { false, false }
    };
    for (const auto [dark, fit] : kVariants) {
        const auto reused = engine_->Render(kCode, dark, 300.0f, fit);
        const auto fresh = NewEngine()->Render(kCode, dark, 300.0f, fit);
        ASSERT_EQ(reused.status, Status::Drawn);
        ASSERT_EQ(fresh.status, Status::Drawn);
        EXPECT_EQ(reused.drawing, fresh.drawing) << "dark=" << dark << " fit=" << fit;
    }
}

// クリップボードの SVG は 1 枚の svg に、図形と 1 行ずつの <text> を CSS なしで書く。
TEST_F(PeleEngineTest, DrawingToSvgWritesShapesAndTextWithoutCss)
{
    const auto out = engine_->Render("flowchart LR\n  A[\"a &\"] --> B[円]", true, 800.0f, false);
    ASSERT_EQ(out.status, Status::Drawn);
    const std::string svg = DrawingToSvg(out.drawing);
    EXPECT_TRUE(svg.starts_with("<svg xmlns=\"http://www.w3.org/2000/svg\""));
    EXPECT_TRUE(svg.ends_with("</svg>"));
    EXPECT_EQ(svg.find("<svg", 1), std::string::npos);
    EXPECT_NE(svg.find("<rect"), std::string::npos);
    EXPECT_NE(svg.find("font-family=\"&quot;Segoe UI&quot;, &quot;Yu Gothic UI&quot;\""), std::string::npos) << svg;
    EXPECT_NE(svg.find("xml:space=\"preserve\">a &amp;</text>"), std::string::npos) << svg;
    EXPECT_NE(svg.find("円</text>"), std::string::npos);
    EXPECT_EQ(svg.find("var("), std::string::npos);
    EXPECT_EQ(svg.find("style="), std::string::npos);
}

// 数式のラベルは Office や Inkscape で読めるよう、ラベルに書いたとおりの $$TeX$$ の文字にする。
TEST_F(PeleEngineTest, DrawingToSvgWritesMathLabelsAsTex)
{
    const auto out = engine_->Render("flowchart LR\n  A[\"$$a<b$$\"]", false, 800.0f, false);
    ASSERT_EQ(out.status, Status::Drawn);
    const std::string svg = DrawingToSvg(out.drawing);
    EXPECT_NE(svg.find("xml:space=\"preserve\">$$a&lt;b$$</text>"), std::string::npos) << svg;
}

namespace {

// 数式を WIC のビットマップに描いて画素を比べる。
class MathDrawTest : public PeleEngineTest {
protected:
    static void SetUpTestSuite()
    {
        PeleEngineTest::SetUpTestSuite();
        ASSERT_HRESULT_SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()));
        wic_ = wic_util::CreateWicFactory("MathDrawTest");
        ASSERT_NE(wic_, nullptr);
    }
    static void TearDownTestSuite()
    {
        wic_.Reset();
        d2d_.Reset();
        PeleEngineTest::TearDownTestSuite();
    }

    struct Canvas {
        Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
        Microsoft::WRL::ComPtr<ID2D1RenderTarget> rt;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        Microsoft::WRL::ComPtr<ID2D1Device> device;
        UINT width = 0;
        UINT height = 0;
    };
    // math が収まる大きさの、dpr 倍の描画先。
    static Canvas MakeCanvas(const MathDrawing& math, float dpr)
    {
        Canvas c;
        c.width = static_cast<UINT>(std::ceil((math.width + 8.0f) * dpr));
        c.height = static_cast<UINT>(std::ceil((math.height + math.depth + 8.0f) * dpr));
        EXPECT_HRESULT_SUCCEEDED(wic_->CreateBitmap(c.width, c.height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &c.bitmap));
        const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                        96.0f * dpr, 96.0f * dpr);
        EXPECT_HRESULT_SUCCEEDED(d2d_->CreateWicBitmapRenderTarget(c.bitmap.Get(), props, &c.rt));
        EXPECT_HRESULT_SUCCEEDED(c.rt->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.1f, 0.1f), &c.brush));
        Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc;
        EXPECT_HRESULT_SUCCEEDED(c.rt.As(&dc));
        dc->GetDevice(&c.device);
        return c;
    }
    // parts を順に、箱の左上が画素に合わない位置へ描いた画素 (32bpp PBGRA)。
    static std::vector<uint32_t> Draw(const Canvas& c, std::span<const MathDrawing> parts, MathMaskCache* masks = nullptr)
    {
        c.rt->BeginDraw();
        c.rt->Clear(D2D1::ColorF(D2D1::ColorF::White));
        for (const auto& part : parts) {
            DrawMath(c.rt.Get(), part, 2.3f, 2.6f + part.height, c.brush.Get(), masks);
        }
        EXPECT_HRESULT_SUCCEEDED(c.rt->EndDraw());
        std::vector<uint32_t> pixels(static_cast<size_t>(c.width) * c.height);
        EXPECT_HRESULT_SUCCEEDED(c.bitmap->CopyPixels(nullptr, c.width * 4, static_cast<UINT>(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data())));
        return pixels;
    }
    static int MaxChannelDiff(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b)
    {
        int max_diff = 0;
        for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
            for (int shift = 0; shift < 32; shift += 8) {
                max_diff = std::max(max_diff, std::abs(static_cast<int>((a[i] >> shift) & 0xFF) - static_cast<int>((b[i] >> shift) & 0xFF)));
            }
        }
        return max_diff;
    }

    static inline Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
    static inline Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
};

} // namespace

// マスクで塗った SVG の形は、形を毎回塗るのとほぼ同じ画素になる。箱の左上を画素に合わせるので、置く位置の端数に依らない。
TEST_F(MathDrawTest, MaskedShapesMatchFilledShapes)
{
    for (const float dpr : { 1.0f, 1.5f }) {
        for (const char* tex : { "\\sqrt{x+1}", "\\overrightarrow{AB}", "\\widehat{xyz}", "\\xrightarrow{abc}", "\\overbrace{a+b+c}" }) {
            const auto math = Math(tex);
            ASSERT_NE(math, nullptr) << tex;
            ASSERT_GT(CountItems<MathDrawing::Path>(*math), 0u) << tex;
            const auto canvas = MakeCanvas(*math, dpr);
            MathMaskCache masks;
            masks.Reset(canvas.device.Get());
            const auto filled = Draw(canvas, { math.get(), 1 });
            const auto masked = Draw(canvas, { math.get(), 1 }, &masks);
            EXPECT_GT(masks.Bytes(), 0u) << tex;
            EXPECT_LE(MaxChannelDiff(filled, masked), 16) << tex << " dpr=" << dpr;
        }
    }
}

// 同じ書体と大きさのグリフを 1 つの run にまとめても、1 つずつ描くのと同じ画素になる (縦のずれ・横の位置・描く順)。
// 色を指定した部分 (\colorbox の背景を含む) との重なりの順も変わらない (\mathrlap の文字は後の背景に隠れる)。
TEST_F(MathDrawTest, MergedGlyphRunsMatchSeparateRuns)
{
    for (const float dpr : { 1.0f, 1.5f }) {
        for (const char* tex : { "\\frac{a}{b}", "\\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}", "x_1^2 + \\sum_{k=0}^{n} k",
                                 "a_1 + \\textcolor{red}{b_1} + c^2 + \\colorbox{yellow}{x} + y_1", "\\mathrlap{xy}\\colorbox{yellow}{ab}" }) {
            const auto math = Math(tex);
            ASSERT_NE(math, nullptr) << tex;
            ASSERT_GE(CountItems<MathDrawing::Glyphs>(*math), 2u) << tex;
            // 1 項目ずつ別の数式にすると、まとめる相手が無い。
            std::vector<MathDrawing> separate;
            for (const auto& item : math->items) {
                separate.push_back({ .width = math->width, .height = math->height, .depth = math->depth, .items = { item } });
            }
            const auto canvas = MakeCanvas(*math, dpr);
            EXPECT_EQ(MaxChannelDiff(Draw(canvas, { math.get(), 1 }), Draw(canvas, separate)), 0) << tex << " dpr=" << dpr;
        }
    }
}

// 予算を超えても、そのフレームで描いたマスクは捨てない。描かなかったフレームの後で捨てる。
TEST_F(MathDrawTest, MaskCacheEvictsUndrawnMasksOverBudget)
{
    const auto math = Math("\\sqrt{x}");
    ASSERT_NE(math, nullptr);
    const auto canvas = MakeCanvas(*math, 1.0f);
    MathMaskCache masks;
    masks.Reset(canvas.device.Get());
    Draw(canvas, { math.get(), 1 }, &masks);
    const size_t count = masks.Count();
    ASSERT_GT(count, 0u);
    masks.EndFrame(0);
    EXPECT_EQ(masks.Count(), count);
    masks.EndFrame(0);
    EXPECT_EQ(masks.Count(), 0u);
    EXPECT_EQ(masks.Bytes(), 0u);

    // 予算に収まれば描かなくても残す。
    Draw(canvas, { math.get(), 1 }, &masks);
    masks.EndFrame(1u << 20);
    masks.EndFrame(1u << 20);
    EXPECT_EQ(masks.Count(), count);

    // デバイスの無い表では作らず、形をそのまま塗る。
    masks.Reset(nullptr);
    Draw(canvas, { math.get(), 1 }, &masks);
    EXPECT_EQ(masks.Count(), 0u);
}
