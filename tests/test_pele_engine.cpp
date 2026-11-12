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
#include <process.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <ranges>
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
    static std::vector<T> ItemsOf(const MathDrawing& math)
    {
        std::vector<T> out;
        for (const auto& item : math.items) {
            if (const auto* it = std::get_if<T>(&item)) {
                out.push_back(*it);
            }
        }
        return out;
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

// 取り消した後は、HTML を覚えている数式も描かない。
TEST_F(PeleEngineTest, CancelStopsRendering)
{
    const auto engine = NewEngine();
    const PeleEngine::MathSource source{ "x", 16.0f };
    ASSERT_NE(engine->RenderMaths({ &source, 1 })[0], nullptr);
    engine->Cancel();
    EXPECT_EQ(engine->Render("flowchart LR\n  A --> B", false, 800.0f).status, Status::Cancelled);
    EXPECT_EQ(engine->RenderMaths({ &source, 1 })[0], nullptr);
}

// KaTeX の HTML は文字の大きさに依らないので覚えておき、別の大きさ (ズーム) では配置だけをやり直す。
// 覚えた HTML から組んでも初めて描いたときと同じになり、描けない TeX は覚えた後も null。
TEST_F(PeleEngineTest, ReusedKatexHtmlMatchesFreshRender)
{
    constexpr std::string_view kTex = "\\sqrt{x+1} + \\frac{a}{b} + \\text{日本}";
    const PeleEngine::MathSource first[]{ { kTex, 16.0f }, { "\\frac{a}", 16.0f } };
    const PeleEngine::MathSource resized[]{ { kTex, 24.0f }, { "\\frac{a}", 24.0f } };
    const auto engine = NewEngine();
    ASSERT_NE(engine->RenderMaths(first)[0], nullptr);
    const auto reused = engine->RenderMaths(resized);
    const auto fresh = NewEngine()->RenderMaths(resized);
    ASSERT_TRUE(reused[0] && fresh[0]);
    EXPECT_EQ(reused[0]->width, fresh[0]->width);
    EXPECT_EQ(reused[0]->height, fresh[0]->height);
    EXPECT_EQ(reused[0]->depth, fresh[0]->depth);
    const auto kinds = [](const MathDrawing& math) {
        return math.items | std::views::transform([](const MathDrawing::Item& item) { return item.index(); }) | std::ranges::to<std::vector>();
    };
    EXPECT_EQ(kinds(*reused[0]), kinds(*fresh[0]));
    EXPECT_EQ(reused[1], nullptr);
    EXPECT_EQ(fresh[1], nullptr);
}

namespace {

// "a" を open と close で depth 段くるんだ数式。
std::string NestedTex(std::string_view open, std::string_view close, int depth)
{
    std::string tex = "a";
    for (int i = 0; i < depth; ++i) {
        tex = std::string{ open } + tex + std::string{ close };
    }
    return tex;
}

// スタックの予約が reserve バイトのスレッドで f を走らせる (std::thread は exe の既定の大きさになる)。
template <class F>
void RunOnThreadWithStack(size_t reserve, F f)
{
    const auto run = [](void* p) -> unsigned {
        (*static_cast<F*>(p))();
        return 0;
    };
    const auto thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, static_cast<unsigned>(reserve), run, &f, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr));
    ASSERT_NE(thread, nullptr);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
}

} // namespace

// 分数・根号・行列を深く入れ子にした数式も描く。JS のスタックが 512KB だった頃は、分数と上付き 3 段・根号 4 段・行列 2 段を
// 超えると "Maximum call stack size exceeded" で描けず、原文のまま出ていた。
TEST_F(PeleEngineTest, RendersDeeplyNestedMaths)
{
    EXPECT_NE(Math("\\sqrt{\\frac{\\sqrt{\\frac{a}{b}}}{\\frac{c}{d}}}"), nullptr);
    EXPECT_NE(Math(NestedTex("\\frac{", "}{b}", 16)), nullptr);
    EXPECT_NE(Math(NestedTex("\\sqrt{", "}", 16)), nullptr);
    EXPECT_NE(Math(NestedTex("\\cfrac{1}{1+", "}", 16)), nullptr);
    EXPECT_NE(Math(NestedTex("x^{", "}", 16)), nullptr);
    EXPECT_NE(Math(NestedTex("\\begin{pmatrix}", " & b \\\\ c & d\\end{pmatrix}", 12)), nullptr);
}

// 上限を超える深さの数式は、スタックを溢れさせずに描けない式として返す。その後も描ける。
TEST_F(PeleEngineTest, TooDeepMathFailsWithoutOverflowingStack)
{
    EXPECT_EQ(Math(NestedTex("\\frac{", "}{b}", 300)), nullptr);
    EXPECT_EQ(Math(NestedTex("{", "}", 3000)), nullptr);
    EXPECT_NE(Math("\\frac{x}{y}"), nullptr);
}

// スタックの小さいスレッドでは JS に使わせる量も減らし、深い数式は溢れさせずに描けない式として返す。
// 別のスレッドで作ったランタイムを使い回しても、走らせるスレッドのスタックで決める。
TEST_F(PeleEngineTest, SmallStackThreadLimitsNestingInsteadOfOverflowing)
{
    const auto engine = NewEngine();
    const auto math = [&](const std::string& tex) {
        const PeleEngine::MathSource source{ tex, 16.0f };
        return engine->RenderMaths({ &source, 1 })[0];
    };
    ASSERT_NE(math("x"), nullptr);
    RunOnThreadWithStack(1u << 20, [&] {
        EXPECT_EQ(math(NestedTex("\\frac{", "}{b}", 12)), nullptr);
        EXPECT_NE(math("y^2"), nullptr);
    });
    EXPECT_NE(math(NestedTex("\\frac{", "}{c}", 12)), nullptr);
}

// 幅の上限に収まらない図も、縮めない大きさの描画命令にする (縮めるのは描くとき)。図形の変換は小数 3 桁に丸めて書き、
// 文字の変換は丸めないので、拡大率を描画命令に書くと図形と文字がずれる。どちらにも拡大率を書かず、描くときに同じ拡大率を掛ける。
TEST_F(PeleEngineTest, WideDiagramKeepsNaturalSizeWithoutBakedScale)
{
    constexpr std::string_view kWide = "flowchart LR\n  A[aaaaaaaaaaaaaaaa] --> B[bbbbbbbbbbbbbbbb] --> C[cccccccccccccccc] --> D[dddddddddddddddd]";
    const auto out = engine_->Render(kWide, false, 200.0f);
    ASSERT_EQ(out.status, Status::Drawn);
    EXPECT_GT(out.drawing.width, 200.0f);
    size_t texts = 0;
    for (const auto& layer : out.drawing.layers) {
        EXPECT_EQ(layer.shapes.find("matrix("), std::string::npos) << layer.shapes;
        for (const auto& t : layer.texts) {
            const auto& m = t.at.transform;
            EXPECT_EQ((std::array{ m[0], m[1], m[2], m[3] }), (std::array{ 1.0f, 0.0f, 0.0f, 1.0f })) << string_convert::WideToUtf8(t.text);
            ++texts;
        }
    }
    EXPECT_EQ(texts, 4u);
}

// 幅の上限に収まる図は、上限が変わっても同じ描画命令になる (幅に縛られない結果のキャッシュの前提)。
TEST_F(PeleEngineTest, FittingDiagramDoesNotDependOnMaxWidth)
{
    constexpr std::string_view kCode = "flowchart LR\n  A[開始] --> B[終了]";
    const auto narrow = engine_->Render(kCode, false, 600.0f);
    const auto wide = engine_->Render(kCode, false, 1600.0f);
    ASSERT_EQ(narrow.status, Status::Drawn);
    ASSERT_EQ(wide.status, Status::Drawn);
    ASSERT_LT(narrow.drawing.width, 600.0f);
    EXPECT_EQ(narrow.drawing, wide.drawing);
}

// 覚えておいた pele の出力をテーマを変えて解き直しても、初めて描いたときと同じになる。
TEST_F(PeleEngineTest, ReusedPeleOutputMatchesFreshRender)
{
    constexpr std::string_view kCode = "flowchart LR\n  A[\"面積 $$x^2$$\"] --> B[bbbbbbbbbbbbbbbbbbbbbbbb] --> C[cccccccccccccccc]";
    ASSERT_EQ(engine_->Render(kCode, false, 300.0f).status, Status::Drawn);
    const auto reused = engine_->Render(kCode, true, 300.0f);
    const auto fresh = NewEngine()->Render(kCode, true, 300.0f);
    ASSERT_EQ(reused.status, Status::Drawn);
    ASSERT_EQ(fresh.status, Status::Drawn);
    EXPECT_EQ(reused.drawing, fresh.drawing);
}

// クリップボードの SVG は 1 枚の svg に、図形と 1 行ずつの <text> を CSS なしで書く。
TEST_F(PeleEngineTest, DrawingToSvgWritesShapesAndTextWithoutCss)
{
    const auto out = engine_->Render("flowchart LR\n  A[\"a &\"] --> B[円]", true, 800.0f);
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
    const auto out = engine_->Render("flowchart LR\n  A[\"$$a<b$$\"]", false, 800.0f);
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

// ---- KaTeX の HTML の配置 (箱の計算) ----
// KaTeX を動かさず、手書きの HTML の断片を配置する。期待値は katex.css とインラインの style から CSS の箱の計算で求めた値。

class KatexLayoutTest : public PeleEngineTest {
protected:
    // 文字の大きさ 10px で組む。KaTeX は 1.21 倍するので 1em = 12.1px。
    static constexpr float kEm = 12.1f;
    static constexpr uint32_t kRed = 0xFF0000FF, kBlue = 0x0000FFFF;
    using Side = MathDrawing::Rect::Side;

    static std::shared_ptr<const MathDrawing> Layout(std::string_view body)
    {
        return engine_->LayoutHtml(std::format(R"(<span class="katex"><span class="katex-html">{}</span></span>)", body), 10.0f);
    }
    // 中身の無い inline-block の箱 (katex-strut は inline-block になる class)。
    static std::string Box(std::string_view style, std::string_view extra_class = {}, std::string_view inner = {})
    {
        return std::format(R"(<span class="katex-strut {}" style="{}">{}</span>)", extra_class, style, inner);
    }
    static std::string Wrap(std::string_view classes, std::string_view inner)
    {
        return std::format(R"(<span class="{}">{}</span>)", classes, inner);
    }
    static std::vector<MathDrawing::Rect> Rects(const MathDrawing& math)
    {
        return ItemsOf<MathDrawing::Rect>(math);
    }
    // 最初の字形の並び。配置できないか、字形が無ければ nullopt。
    static std::optional<MathDrawing::Glyphs> FirstGlyphs(std::string_view body)
    {
        const auto math = Layout(body);
        auto runs = math ? ItemsOf<MathDrawing::Glyphs>(*math) : std::vector<MathDrawing::Glyphs>{};
        return runs.empty() ? std::nullopt : std::optional{ std::move(runs.front()) };
    }
    // 位置と大きさは em で比べる。
    static void ExpectRect(const MathDrawing::Rect& r, float x, float y, float w, float h)
    {
        EXPECT_NEAR(r.x / kEm, x, 1e-3f);
        EXPECT_NEAR(r.y / kEm, y, 1e-3f);
        EXPECT_NEAR(r.w / kEm, w, 1e-3f);
        EXPECT_NEAR(r.h / kEm, h, 1e-3f);
    }
    // 書体は KatexAssets が持ち続けるので、ポインタを比べられる。
    static IDWriteFontFace* FaceOf(std::string_view body)
    {
        const auto glyphs = FirstGlyphs(body);
        return glyphs ? glyphs->face.Get() : nullptr;
    }
};

// 幅・高さ・余白・詰め物・vertical-align から箱の大きさとベースラインを決める。中身の無い inline-block は下端がベースライン。
// 背景の矩形は、余白の内側 (枠と詰め物を含む) を塗る。
TEST_F(KatexLayoutTest, BoxSizeFromInlineStyle)
{
    struct Case {
        std::string_view style;
        float width, height, depth; // 数式全体 (em)
        float x, y, w, h;           // 背景の矩形 (em)
    };
    constexpr Case kCases[] = {
        { "height:2em;width:3em", 3.0f, 2.0f, 0.0f, 0.0f, -2.0f, 3.0f, 2.0f },
        // vertical-align は上向きが正。下げた分が深さになる。
        { "height:2em;width:3em;vertical-align:-0.5em", 3.0f, 1.5f, 0.5f, 0.0f, -1.5f, 3.0f, 2.0f },
        { "height:2em;width:3em;vertical-align:0.5em", 3.0f, 2.5f, 0.0f, 0.0f, -2.5f, 3.0f, 2.0f },
        { "height:2em;width:3em;margin-left:1em;margin-right:0.5em", 4.5f, 2.0f, 0.0f, 1.0f, -2.0f, 3.0f, 2.0f },
        // margin の一括指定は上・右・下・左の順。値が 1〜3 個なら左は右と同じ。
        { "height:2em;width:3em;margin:0 0.5em 0 1em", 4.5f, 2.0f, 0.0f, 1.0f, -2.0f, 3.0f, 2.0f },
        { "height:2em;width:3em;margin:0 1em 0", 5.0f, 2.0f, 0.0f, 1.0f, -2.0f, 3.0f, 2.0f },
        { "height:2em;width:3em;margin:0 0.5em", 4.0f, 2.0f, 0.0f, 0.5f, -2.0f, 3.0f, 2.0f },
        { "height:2em;width:3em;margin:0.25em", 3.5f, 2.0f, 0.0f, 0.25f, -2.0f, 3.0f, 2.0f },
        { "height:2em;width:3em;padding-left:1em", 4.0f, 2.0f, 0.0f, 0.0f, -2.0f, 4.0f, 2.0f },
        { "height:2em;width:1em;min-width:3em", 3.0f, 2.0f, 0.0f, 0.0f, -2.0f, 3.0f, 2.0f },
        { "height:2em;width:3em;min-width:1em", 3.0f, 2.0f, 0.0f, 0.0f, -2.0f, 3.0f, 2.0f },
        // 長さは px でも書ける (12.1px = 1em)。
        { "height:24.2px;width:36.3px", 3.0f, 2.0f, 0.0f, 0.0f, -2.0f, 3.0f, 2.0f },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.style);
        const auto math = Layout(Box(std::format("{};background-color:red", c.style)));
        ASSERT_NE(math, nullptr);
        EXPECT_NEAR(math->width / kEm, c.width, 1e-3f);
        EXPECT_NEAR(math->height / kEm, c.height, 1e-3f);
        EXPECT_NEAR(math->depth / kEm, c.depth, 1e-3f);
        const auto rects = Rects(*math);
        ASSERT_EQ(rects.size(), 1u);
        EXPECT_EQ(rects[0].side, Side::Fill);
        EXPECT_EQ(rects[0].color, kRed);
        ExpectRect(rects[0], c.x, c.y, c.w, c.h);
    }
}

// 枠は内容の外側に付き、箱を広げる。背景は枠の下まで塗る。枠の色は border-color、無ければ文字の色 (指定なし)。
TEST_F(KatexLayoutTest, BordersSurroundContent)
{
    const auto all = Layout(Box("height:2em;width:3em;border-width:0.1em;border-color:blue;background-color:red"));
    ASSERT_NE(all, nullptr);
    EXPECT_NEAR(all->width / kEm, 3.2f, 1e-3f);
    EXPECT_NEAR(all->height / kEm, 2.2f, 1e-3f);
    EXPECT_NEAR(all->depth / kEm, 0.0f, 1e-3f);
    const auto rects = Rects(*all);
    ASSERT_EQ(rects.size(), 5u);
    EXPECT_EQ(rects[0].side, Side::Fill);
    EXPECT_EQ(rects[0].color, kRed);
    ExpectRect(rects[0], 0.0f, -2.2f, 3.2f, 2.2f);
    EXPECT_EQ(rects[1].side, Side::Top);
    ExpectRect(rects[1], 0.0f, -2.2f, 3.2f, 0.1f);
    EXPECT_EQ(rects[2].side, Side::Bottom);
    ExpectRect(rects[2], 0.0f, -0.1f, 3.2f, 0.1f);
    EXPECT_EQ(rects[3].side, Side::Left);
    ExpectRect(rects[3], 0.0f, -2.2f, 0.1f, 2.2f);
    EXPECT_EQ(rects[4].side, Side::Right);
    ExpectRect(rects[4], 3.1f, -2.2f, 0.1f, 2.2f);
    for (size_t i = 1; i < rects.size(); ++i) {
        EXPECT_EQ(rects[i].color, kBlue);
        EXPECT_FALSE(rects[i].dashed);
    }

    // 辺ごとの太さ。指定した辺だけに枠が付く。
    struct OneSide {
        std::string_view style;
        Side side;
        float width, height;
        float x, y, w, h;
    };
    constexpr OneSide kSides[] = {
        { "border-top-width:0.1em", Side::Top, 3.0f, 2.1f, 0.0f, -2.1f, 3.0f, 0.1f },
        { "border-right-width:0.1em", Side::Right, 3.1f, 2.0f, 3.0f, -2.0f, 0.1f, 2.0f },
        { "border-bottom-width:0.1em", Side::Bottom, 3.0f, 2.1f, 0.0f, -0.1f, 3.0f, 0.1f },
    };
    for (const auto& c : kSides) {
        SCOPED_TRACE(c.style);
        const auto math = Layout(Box(std::format("height:2em;width:3em;{}", c.style)));
        ASSERT_NE(math, nullptr);
        EXPECT_NEAR(math->width / kEm, c.width, 1e-3f);
        EXPECT_NEAR(math->height / kEm, c.height, 1e-3f);
        const auto one = Rects(*math);
        ASSERT_EQ(one.size(), 1u);
        EXPECT_EQ(one[0].side, c.side);
        EXPECT_EQ(one[0].color, std::nullopt);
        ExpectRect(one[0], c.x, c.y, c.w, c.h);
    }

    // 破線は border-style で全部の辺、border-right-style で右の辺だけ。
    const auto dashed_sides = [](std::string_view style) {
        const auto math = Layout(Box(std::format("height:2em;width:3em;border-width:0.1em;{}", style)));
        std::vector<Side> out;
        if (math) {
            for (const auto& r : Rects(*math)) {
                if (r.dashed) {
                    out.push_back(r.side);
                }
            }
        }
        return out;
    };
    EXPECT_EQ(dashed_sides("border-style:dashed"), (std::vector<Side>{ Side::Top, Side::Bottom, Side::Left, Side::Right }));
    EXPECT_EQ(dashed_sides("border-right-style:dashed"), (std::vector<Side>{ Side::Right }));
    EXPECT_EQ(dashed_sides("border-style:solid;border-right-style:solid"), (std::vector<Side>{}));
}

// katex.css の class が決める幅・余白・詰め物・文字の大きさ。1em 四方の箱を入れて、全体の幅で確かめる。
TEST_F(KatexLayoutTest, ClassRulesSetWidthsAndSpacing)
{
    const std::string unit = Box("height:1em;width:1em");
    struct Case {
        std::string body;
        float width_em;
    };
    const Case kCases[] = {
        { Wrap("nulldelimiter", ""), 0.12f },
        { Wrap("katex-thinbox", unit), 0.0f },
        // 左右の詰め物。
        { Wrap("x-arrow-pad", unit), 2.0f },
        { Wrap("boxpad", unit), 1.6f },
        { Wrap("cancel-pad", unit), 1.4f },
        { Wrap("anglpad", unit), 1.07778f },
        { Wrap("cd-arrow-pad", unit), 1.83334f },
        // 負の余白で左右に 0.2em ずつはみ出す。
        { Box("height:1em;width:3em", "cancel-lap"), 2.6f },
        // 根号の指数は左に 5/18em 空け、右は 10/18em 重ねる。sqrt の子でなければ空けない。
        { Wrap("sqrt", Box("height:1em;width:1em", "katex-root")), 0.72222f },
        { Wrap("mord", Box("height:1em;width:1em", "katex-root")), 1.0f },
        // 文字の大きさ (reset-sizeA sizeB)。em の長さが 大きさ B / 大きさ A 倍になる。
        { Wrap("katex-sizing reset-size6 size3", unit), 0.7f },
        { Wrap("fontsize-ensurer reset-size6 size3", unit), 0.7f },
        { Wrap("katex-sizing reset-size3 size6", unit), 1.0f / 0.7f },
        { Wrap("katex-sizing reset-size11 size1", unit), 0.5f / 2.488f },
        { Wrap("katex-sizing reset-size1 size11", unit), 2.488f / 0.5f },
        { Wrap("katex-sizing reset-size6 size7", unit), 1.2f },
        // どちらかが欠けるか、大きさを変える class が無ければ変えない。
        { Wrap("katex-sizing size3", unit), 1.0f },
        { Wrap("katex-sizing reset-size3", unit), 1.0f },
        { Wrap("mord reset-size6 size3", unit), 1.0f },
        // 入れ子は掛け合わせる。
        { Wrap("katex-sizing reset-size6 size3", Wrap("katex-sizing reset-size3 size6", unit)), 1.0f },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.body);
        const auto math = Layout(c.body);
        ASSERT_NE(math, nullptr);
        EXPECT_NEAR(math->width / kEm, c.width_em, 1e-3f);
    }
    // 縦の区切り線は細くても 1px の幅を持つ。
    const auto separator = Layout(Wrap("vertical-separator", ""));
    ASSERT_NE(separator, nullptr);
    EXPECT_EQ(separator->width, 1.0f);
}

// 枠を持つ class。fbox と angl は border-box なので、指定の幅と高さに枠を含める。
TEST_F(KatexLayoutTest, ClassRulesSetBorders)
{
    const auto fbox = Layout(Box("height:2em;width:3em", "fbox"));
    ASSERT_NE(fbox, nullptr);
    EXPECT_NEAR(fbox->width / kEm, 3.0f, 1e-3f);
    EXPECT_NEAR(fbox->height / kEm, 2.0f, 1e-3f);
    const auto fbox_rects = Rects(*fbox);
    ASSERT_EQ(fbox_rects.size(), 4u);
    EXPECT_EQ(fbox_rects[0].side, Side::Top);
    ExpectRect(fbox_rects[0], 0.0f, -2.0f, 3.0f, 0.04f);
    EXPECT_EQ(fbox_rects[1].side, Side::Bottom);
    ExpectRect(fbox_rects[1], 0.0f, -0.04f, 3.0f, 0.04f);
    EXPECT_EQ(fbox_rects[2].side, Side::Left);
    ExpectRect(fbox_rects[2], 0.0f, -2.0f, 0.04f, 2.0f);
    EXPECT_EQ(fbox_rects[3].side, Side::Right);
    ExpectRect(fbox_rects[3], 2.96f, -2.0f, 0.04f, 2.0f);
    const auto fcolorbox = Layout(Box("height:2em;width:3em", "fcolorbox"));
    ASSERT_NE(fcolorbox, nullptr);
    EXPECT_EQ(Rects(*fcolorbox).size(), 4u);

    // \angl は上と右に線を引き、右に余白を空ける。
    const auto angl = Layout(Box("height:2em;width:3em", "angl"));
    ASSERT_NE(angl, nullptr);
    EXPECT_NEAR(angl->width / kEm, 3.03889f, 1e-3f);
    EXPECT_NEAR(angl->height / kEm, 2.0f, 1e-3f);
    const auto angl_rects = Rects(*angl);
    ASSERT_EQ(angl_rects.size(), 2u);
    EXPECT_EQ(angl_rects[0].side, Side::Top);
    ExpectRect(angl_rects[0], 0.0f, -2.0f, 3.0f, 0.049f);
    EXPECT_EQ(angl_rects[1].side, Side::Right);
    ExpectRect(angl_rects[1], 2.951f, -2.0f, 0.049f, 2.0f);

    // \sout は下に線を引く (箱はその分だけ高くなる)。
    const auto sout = Layout(Box("height:1em;width:3em", "katex-sout"));
    ASSERT_NE(sout, nullptr);
    EXPECT_NEAR(sout->height / kEm, 1.08f, 1e-3f);
    const auto sout_rects = Rects(*sout);
    ASSERT_EQ(sout_rects.size(), 1u);
    EXPECT_EQ(sout_rects[0].side, Side::Bottom);
    ExpectRect(sout_rects[0], 0.0f, -0.08f, 3.0f, 0.08f);
}

// 分数の線などは含む箱の幅いっぱいに引き、細くても 1px の高さを持つ。hdashline は破線。
TEST_F(KatexLayoutTest, RuleLinesFillContainerWidth)
{
    for (const std::string_view cls : { "frac-line", "overline-line", "underline-line", "katex-hline", "katex-hdashline" }) {
        SCOPED_TRACE(cls);
        const auto math = Layout(Box("width:4em", "", std::format(R"(<span class="{}" style="border-bottom-width:0.04em"></span>)", cls)));
        ASSERT_NE(math, nullptr);
        EXPECT_NEAR(math->width / kEm, 4.0f, 1e-3f);
        EXPECT_NEAR(math->height, 1.0f + 0.04f * kEm, 1e-3f);
        const auto rects = Rects(*math);
        ASSERT_EQ(rects.size(), 1u);
        EXPECT_EQ(rects[0].side, Side::Bottom);
        EXPECT_EQ(rects[0].dashed, cls == "katex-hdashline");
        ExpectRect(rects[0], 0.0f, -0.04f, 4.0f, 0.04f);
    }
}

// 相対位置の要素は top・left・right・bottom の分だけ描く位置をずらし、箱の大きさは変えない。位置指定していない要素はずらさない。
TEST_F(KatexLayoutTest, RelativeOffsetsMoveDrawingOnly)
{
    struct Case {
        std::string_view classes;
        std::string_view style;
        float x, y;
    };
    constexpr Case kCases[] = {
        { "katex-rule", "", 0.0f, -1.0f },
        { "katex-rule", "top:0.5em", 0.0f, -0.5f },
        { "katex-rule", "bottom:0.5em", 0.0f, -1.5f },
        { "katex-rule", "left:0.25em", 0.25f, -1.0f },
        { "katex-rule", "right:0.25em", -0.25f, -1.0f },
        { "katex-rule", "top:0.5em;left:0.25em", 0.25f, -0.5f },
        // top と bottom、left と right の両方があれば top と left が勝つ。
        { "katex-rule", "top:0.5em;bottom:2em;left:0.25em;right:2em", 0.25f, -0.5f },
        { "katex-strut", "top:0.5em;left:0.25em", 0.0f, -1.0f },
        { "katex-strut", "position:relative;top:0.5em;left:0.25em", 0.25f, -0.5f },
        { "katex-rule", "position:static;top:0.5em;left:0.25em", 0.0f, -1.0f },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(testing::Message() << c.classes << " " << c.style);
        const auto math = Layout(std::format(R"(<span class="{}" style="height:1em;width:1em;background-color:red;{}"></span>)", c.classes, c.style));
        ASSERT_NE(math, nullptr);
        EXPECT_NEAR(math->width / kEm, 1.0f, 1e-3f);
        EXPECT_NEAR(math->height / kEm, 1.0f, 1e-3f);
        const auto rects = Rects(*math);
        ASSERT_EQ(rects.size(), 1u);
        ExpectRect(rects[0], c.x, c.y, 1.0f, 1.0f);
    }
}

// \rlap・\llap・\clap は幅 0 の箱で、中身を右・左・中央にはみ出して置く。後ろの箱は詰めて置く。
TEST_F(KatexLayoutTest, LapBoxesOverflowWithoutWidth)
{
    struct Case {
        std::string_view cls;
        float inner_x;
    };
    constexpr Case kCases[] = { { "rlap", 0.0f }, { "llap", -2.0f }, { "clap", -1.0f } };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.cls);
        const std::string lap = Wrap(c.cls, Wrap("katex-inner", Box("height:1em;width:2em;background-color:red")) + Wrap("katex-fix", ""));
        const auto math = Layout(Box("height:1em;width:3em") + lap + Box("height:1em;width:1em;background-color:blue"));
        ASSERT_NE(math, nullptr);
        EXPECT_NEAR(math->width / kEm, 4.0f, 1e-3f);
        const auto rects = Rects(*math);
        ASSERT_EQ(rects.size(), 2u);
        EXPECT_EQ(rects[0].color, kRed);
        ExpectRect(rects[0], 3.0f + c.inner_x, -1.0f, 2.0f, 1.0f);
        EXPECT_EQ(rects[1].color, kBlue);
        ExpectRect(rects[1], 3.0f, -1.0f, 1.0f, 1.0f);
    }
}

// 幅を決めた箱の中身は中央に置く (KaTeX の箱の既定)。msupsub と svg-align は左に寄せ、その中の mover などはまた中央に戻す。
TEST_F(KatexLayoutTest, ContentAlignmentInsideFixedWidth)
{
    const std::string inner = Box("height:1em;width:1em;background-color:red");
    struct Case {
        std::string body;
        float x;
    };
    const Case kCases[] = {
        { Box("width:4em", "", inner), 1.5f },
        { Box("width:4em", "msupsub", inner), 0.0f },
        { Box("width:4em", "svg-align", inner), 0.0f },
        { Wrap("msupsub", Box("width:4em", "", inner)), 0.0f },
        { Wrap("msupsub", Box("width:4em", "mover", inner)), 1.5f },
        { Wrap("msupsub", Box("width:4em", "munder", inner)), 1.5f },
        { Wrap("msupsub", Box("width:4em", "x-arrow", inner)), 1.5f },
        // 中身のほうが広ければ左端から置く。
        { Box("width:0.5em", "", inner), 0.0f },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.body);
        const auto math = Layout(c.body);
        ASSERT_NE(math, nullptr);
        const auto rects = Rects(*math);
        ASSERT_EQ(rects.size(), 1u);
        ExpectRect(rects[0], c.x, -1.0f, 1.0f, 1.0f);
    }
}

// 式番号 (katex-tag) は HTML のどこにあっても、本体の右に 1em 空けて置く。
TEST_F(KatexLayoutTest, TagGoesRightOfBodyWithGap)
{
    const auto math = Layout(Wrap("katex-tag", Box("height:1em;width:1em;background-color:red")) + Box("height:1em;width:2em;background-color:blue"));
    ASSERT_NE(math, nullptr);
    EXPECT_NEAR(math->width / kEm, 4.0f, 1e-3f);
    const auto rects = Rects(*math);
    ASSERT_EQ(rects.size(), 2u);
    EXPECT_EQ(rects[0].color, kBlue);
    ExpectRect(rects[0], 0.0f, -1.0f, 2.0f, 1.0f);
    EXPECT_EQ(rects[1].color, kRed);
    ExpectRect(rects[1], 3.0f, -1.0f, 1.0f, 1.0f);
}

// 配置できない HTML は、崩れたまま描かずに数式ごと諦める (本文は TeX のまま出る)。
TEST_F(KatexLayoutTest, UnsupportedHtmlFailsWholeFormula)
{
    ASSERT_NE(Layout(Box("height:1em;width:1em")), nullptr);
    const std::string_view kBodies[] = {
        R"(<span class="katex-strut" style="height:1em;float:left"></span>)",
        R"(<span class="katex-strut" style="height:abc"></span>)",
        R"(<span class="katex-strut" style="width:abc"></span>)",
        R"(<span class="katex-strut" style="min-width:abc"></span>)",
        R"(<span class="katex-strut" style="vertical-align:abc"></span>)",
        R"(<span class="katex-strut" style="top:abc"></span>)",
        R"(<span class="katex-strut" style="margin-left:abc"></span>)",
        R"(<span class="katex-strut" style="margin-right:abc"></span>)",
        R"(<span class="katex-strut" style="margin:1em abc"></span>)",
        R"(<span class="katex-strut" style="padding-left:abc"></span>)",
        R"(<span class="katex-strut" style="border-width:abc"></span>)",
        R"(<span class="katex-strut" style="border-bottom-width:abc"></span>)",
        R"(<div style="height:1em"></div>)",
        R"(<span class="katex-newline"></span>)",
    };
    for (const auto body : kBodies) {
        SCOPED_TRACE(body);
        EXPECT_EQ(Layout(body), nullptr);
    }
    // 読めない色はブラウザと同じく無視して描く。
    EXPECT_NE(Layout(R"(<span class="katex-strut" style="height:1em;color:nosuchcolor;background-color:nosuchcolor"></span>)"), nullptr);
    // katex-html が無ければ配置するものが無い。
    EXPECT_EQ(engine_->LayoutHtml(R"(<span class="katex"><span class="katex-mathml">x</span></span>)", 10.0f), nullptr);
}

// 書体を決める class ごとに、KaTeX の別の書体 (ファイルか、太字・斜体の合成) を使う。
TEST_F(KatexLayoutTest, FontClassesSelectDifferentFaces)
{
    const auto face = [](std::string_view classes, std::string_view text = "A") { return FaceOf(Wrap(classes, text)); };
    IDWriteFontFace* const plain = face("mord");
    ASSERT_NE(plain, nullptr);
    EXPECT_EQ(face("mord other"), plain);
    const std::string_view kClasses[] = { "textbf", "textit", "textsf", "texttt", "mathnormal", "boldsymbol", "mathbb", "mathcal", "mathfrak", "mathboldfrak" };
    std::vector<IDWriteFontFace*> faces{ plain };
    for (const auto cls : kClasses) {
        SCOPED_TRACE(cls);
        IDWriteFontFace* const f = face(cls);
        ASSERT_NE(f, nullptr);
        EXPECT_FALSE(std::ranges::contains(faces, f)) << "ほかの class と同じ書体";
        faces.push_back(f);
    }
    // mathrm は斜体だけを外す (textit の斜体を打ち消して元の書体に戻る)。
    EXPECT_EQ(face("textit mathrm"), plain);
    // 後の規則が勝つ: textsf の後の texttt は等幅。
    EXPECT_EQ(face("textsf texttt"), face("texttt"));

    // 区切り記号の大きさごとの書体 (Size1〜Size4)。delimsizing が無ければ size は書体を変えない。
    std::vector<IDWriteFontFace*> sizes{ face("mord", "(") };
    for (const std::string_view cls : { "delimsizing size1", "delimsizing size2", "delimsizing size3", "delimsizing size4" }) {
        SCOPED_TRACE(cls);
        IDWriteFontFace* const f = face(cls, "(");
        ASSERT_NE(f, nullptr);
        EXPECT_FALSE(std::ranges::contains(sizes, f));
        sizes.push_back(f);
    }
    EXPECT_EQ(face("mord size2", "("), sizes[0]);
    EXPECT_EQ(face("delimsizing size5", "("), sizes[0]);
    EXPECT_EQ(face("delimsizing size0", "("), sizes[0]);
    // 大きな演算子は small-op が Size1、large-op が Size2。
    EXPECT_EQ(face("op-symbol small-op", "("), sizes[1]);
    EXPECT_EQ(face("op-symbol large-op", "("), sizes[2]);
    EXPECT_EQ(face("small-op", "("), sizes[0]);
    // delim-size1・delim-size4 の中の span。
    EXPECT_EQ(FaceOf(Wrap("delim-size1", Wrap("x", "("))), sizes[1]);
    EXPECT_EQ(FaceOf(Wrap("delim-size4", Wrap("x", "("))), sizes[4]);
    EXPECT_EQ(FaceOf(Wrap("delim-size2", Wrap("x", "("))), sizes[0]);
}

// 縦積み (vlist) は、行ごとに top + pstrut の高さだけベースラインをずらして重ねる。高さは 1 行目のセル、深さは 2 行目のセルの
// height。幅はいちばん広い行 (余白を含む) で、狭い行は列の中でそろえる。
TEST_F(KatexLayoutTest, VListStacksRowsByTopAndPstrut)
{
    const auto vlist = [](std::string_view extra_row = {}) {
        return std::format(
            R"(<span class="vlist-t vlist-t2"><span class="vlist-r"><span class="vlist" style="height:2em">)"
            R"(<span style="top:-3em"><span class="pstrut" style="height:3em"></span>{}</span>)"
            R"(<span style="top:-4em;margin-left:0.5em"><span class="pstrut" style="height:3em"></span>{}</span>{})"
            R"(</span><span class="vlist-s">x</span></span>)"
            R"(<span class="vlist-r"><span class="vlist" style="height:0.5em"><span></span></span></span></span>)",
            Box("height:1em;width:2em;background-color:red"), Box("height:1em;width:1em;background-color:blue"), extra_row);
    };
    const auto math = Layout(vlist());
    ASSERT_NE(math, nullptr);
    EXPECT_NEAR(math->width / kEm, 2.0f, 1e-3f);
    EXPECT_NEAR(math->height / kEm, 2.0f, 1e-3f);
    EXPECT_NEAR(math->depth / kEm, 0.5f, 1e-3f);
    auto rects = Rects(*math);
    ASSERT_EQ(rects.size(), 2u);
    // 1 行目はベースラインの上、2 行目は 1em 上。2 行目は左の余白 0.5em の残り 1.5em の中央。
    ExpectRect(rects[0], 0.0f, -1.0f, 2.0f, 1.0f);
    ExpectRect(rects[1], 0.75f, -2.0f, 1.0f, 1.0f);

    // 列の中のそろえ方は、vlist-t の親の class で決まる。
    struct Align {
        std::string_view outer, parent;
        float x;
    };
    constexpr Align kAligns[] = {
        { "mord", "col-align-l", 0.5f },   { "mord", "col-align-r", 1.0f },    { "msupsub", "col-align-c", 0.75f }, { "msupsub", "op-limits", 0.75f },
        { "msupsub", "katex-accent", 0.75f }, { "msupsub", "mord", 0.5f }, { "msupsub", "mfrac", 0.75f },
    };
    for (const auto& a : kAligns) {
        SCOPED_TRACE(testing::Message() << a.outer << " > " << a.parent);
        const auto aligned = Layout(Wrap(a.outer, Wrap(a.parent, vlist())));
        ASSERT_NE(aligned, nullptr);
        rects = Rects(*aligned);
        ASSERT_EQ(rects.size(), 2u);
        ExpectRect(rects[1], a.x, -2.0f, 1.0f, 1.0f);
    }

    // 幅が百分率の行 (分数の線) は、列の幅が決まってからその幅で置く。
    const auto with_line = Layout(vlist(R"(<span style="top:-3.2em"><span class="pstrut" style="height:3em"></span>)"
                                        R"(<span class="frac-line" style="border-bottom-width:0.04em"></span></span>)"));
    ASSERT_NE(with_line, nullptr);
    rects = Rects(*with_line);
    ASSERT_EQ(rects.size(), 3u);
    EXPECT_EQ(rects[2].side, Side::Bottom);
    ExpectRect(rects[2], 0.0f, -0.24f, 2.0f, 0.04f);

    // 行の中身は pstrut のほかに 1 つだけ。2 つあれば配置できない。
    EXPECT_EQ(Layout(vlist(R"(<span style="top:-3em"><span class="pstrut" style="height:3em"></span><span>a</span><span>b</span></span>)")), nullptr);
}

// inline の要素の背景と下線は、中身の高さと深さに合わせて、余白の内側に描く。箱の高さと深さは中身のまま。
TEST_F(KatexLayoutTest, InlineSpanPaintsAroundContent)
{
    const auto math = Layout(std::format(R"(<span class="mord" style="background-color:red;border-bottom-width:0.1em;border-color:blue;)"
                                         R"(margin-left:0.5em;margin-right:1em;padding-left:0.25em">{}</span>)",
                                         Box("height:1em;width:2em;vertical-align:-0.25em")));
    ASSERT_NE(math, nullptr);
    EXPECT_NEAR(math->width / kEm, 3.75f, 1e-3f);
    EXPECT_NEAR(math->height / kEm, 0.75f, 1e-3f);
    EXPECT_NEAR(math->depth / kEm, 0.25f, 1e-3f);
    const auto rects = Rects(*math);
    ASSERT_EQ(rects.size(), 2u);
    EXPECT_EQ(rects[0].side, Side::Fill);
    EXPECT_EQ(rects[0].color, kRed);
    ExpectRect(rects[0], 0.5f, -0.75f, 2.25f, 1.1f);
    EXPECT_EQ(rects[1].side, Side::Bottom);
    EXPECT_EQ(rects[1].color, kBlue);
    ExpectRect(rects[1], 0.5f, 0.25f, 2.25f, 0.1f);

    // inline の要素は width を持たない (中身の幅になる)。
    const auto inline_width = Layout(std::format(R"(<span class="mord" style="width:5em">{}</span>)", Box("height:1em;width:2em")));
    ASSERT_NE(inline_width, nullptr);
    EXPECT_NEAR(inline_width->width / kEm, 2.0f, 1e-3f);
}

// 絶対位置の子 (伸びる括弧や矢印の部品) は、位置指定した親の箱の幅を基準に幅と位置を決め、親の内容の上端から置く。
TEST_F(KatexLayoutTest, AbsoluteChildrenUsePositionedParentBox)
{
    struct Case {
        std::string_view classes;
        std::string_view style;
        float x, y, w;
    };
    constexpr Case kCases[] = {
        { "brace-left", "", 0.0f, -2.0f, 1.004f },
        { "brace-center", "", 1.0f, -2.0f, 2.0f },
        { "brace-right", "", 2.996f, -2.0f, 1.004f },
        { "halfarrow-left", "", 0.0f, -2.0f, 2.008f },
        { "halfarrow-right", "", 1.992f, -2.0f, 2.008f },
        // left と right は親の箱の幅に対する百分率でも書ける。bottom は親の下端から。
        { "brace-left", "left:1em", 1.0f, -2.0f, 1.004f },
        { "brace-left", "left:50%", 2.0f, -2.0f, 1.004f },
        { "brace-right", "right:0.5em", 2.496f, -2.0f, 1.004f },
        { "brace-right", "right:25%", 1.996f, -2.0f, 1.004f },
        { "brace-left", "bottom:0.25em", 0.0f, -1.25f, 1.004f },
        { "brace-left", "bottom:50%", 0.0f, -3.0f, 1.004f },
        { "brace-left", "width:1em", 0.0f, -2.0f, 1.0f },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(testing::Message() << c.classes << " " << c.style);
        const std::string child = std::format(R"(<span class="{}" style="height:1em;background-color:red;{}"></span>)", c.classes, c.style);
        const auto math = Layout(std::format(R"(<span class="katex-rule" style="height:2em;width:4em;margin-left:1em">{}</span>)", child));
        ASSERT_NE(math, nullptr);
        EXPECT_NEAR(math->width / kEm, 5.0f, 1e-3f);
        const auto rects = Rects(*math);
        ASSERT_EQ(rects.size(), 1u);
        ExpectRect(rects[0], 1.0f + c.x, c.y, c.w, 1.0f);
    }
}

// 文字の並び: 続く空白は 1 つにまとめ、ゼロ幅スペースは置かない。高さと深さは書体の値で、文字の大きさに比例する。
TEST_F(KatexLayoutTest, TextRunsCollapseSpacesAndScaleWithSize)
{
    const auto width = [](std::string_view body) {
        const auto math = Layout(body);
        EXPECT_NE(math, nullptr) << body;
        return math ? math->width : -1.0f;
    };
    const float ab = width("ab");
    const float a_b = width("a b");
    EXPECT_GT(ab, 0.0f);
    EXPECT_GT(a_b, ab + 1.0f);
    EXPECT_EQ(width("a  b"), a_b);
    EXPECT_EQ(width("a \t\n b"), a_b);
    EXPECT_EQ(width("a\tb"), a_b);
    EXPECT_EQ(width("a\nb"), a_b);
    EXPECT_EQ(width("a&#x200B;b"), ab);
    EXPECT_EQ(width("a&#xA0;b"), a_b);
    // 空白をまとめるのは続いているときだけ。
    EXPECT_NEAR(width("a b b") - a_b, a_b - width("a"), 1e-3f);

    const auto normal = Layout("A");
    const auto large = Layout(Wrap("katex-sizing reset-size6 size7", "A"));
    ASSERT_TRUE(normal && large);
    EXPECT_GT(normal->height, 0.5f * kEm);
    EXPECT_GT(normal->depth, 0.0f);
    EXPECT_NEAR(large->width, normal->width * 1.2f, 1e-3f);
    EXPECT_NEAR(large->height, normal->height * 1.2f, 1e-3f);
    EXPECT_NEAR(large->depth, normal->depth * 1.2f, 1e-3f);

    // 文字の後ろの箱は、文字の幅の分だけ右に置く。
    const auto after = Layout("A" + Box("height:1em;width:1em;background-color:red"));
    ASSERT_NE(after, nullptr);
    const auto rects = Rects(*after);
    ASSERT_EQ(rects.size(), 1u);
    EXPECT_NEAR(rects[0].x, normal->width, 1e-3f);
    EXPECT_NEAR(after->width, normal->width + kEm, 1e-3f);
}

// \reflectbox は箱の中で字形の並びを左右に折り返す。
TEST_F(KatexLayoutTest, ReflectBoxMirrorsGlyphRuns)
{
    const std::string inner = "A" + Box("height:1em;width:2em");
    const auto plain = Layout(Wrap("mord", inner));
    const auto reflected = Layout(Wrap("reflectbox", inner));
    ASSERT_TRUE(plain && reflected);
    const auto p = FirstGlyphs(Wrap("mord", inner));
    const auto r = FirstGlyphs(Wrap("reflectbox", inner));
    const auto t = FirstGlyphs(Wrap("reflectbox", Wrap("reflectbox", inner)));
    ASSERT_TRUE(p && r && t);
    EXPECT_FALSE(p->mirror);
    EXPECT_EQ(p->x, 0.0f);
    EXPECT_TRUE(r->mirror);
    EXPECT_NEAR(r->x, 2.0f * kEm, 1e-3f);
    EXPECT_EQ(reflected->width, plain->width);
    // 2 回折り返すと元に戻る。
    EXPECT_FALSE(t->mirror);
    EXPECT_NEAR(t->x, 0.0f, 1e-3f);
}

// 自動の式番号 (eqn-num) は文書の順に (1) から振る。
TEST_F(KatexLayoutTest, EquationNumbersCountInDocumentOrder)
{
    const auto math = Layout(Wrap("eqn-num", "") + Wrap("mord", "") + Wrap("eqn-num", ""));
    const auto one = Layout(Wrap("eqn-num", ""));
    const auto text = Layout("(1)(2)");
    ASSERT_TRUE(math && one && text);
    const auto all_glyphs = [](const MathDrawing& m) {
        std::vector<uint16_t> out;
        for (const auto& run : ItemsOf<MathDrawing::Glyphs>(m)) {
            out.insert(out.end(), run.glyphs.begin(), run.glyphs.end());
        }
        return out;
    };
    EXPECT_EQ(all_glyphs(*math), all_glyphs(*text));
    EXPECT_EQ(all_glyphs(*one).size(), 3u);
    EXPECT_NEAR(math->width, text->width, 1e-3f);
}
