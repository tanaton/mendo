#include <gtest/gtest.h>
#include "diagram_raster.h"
#include "example_files.h"
#include "mermaid_util.h"
#include "pele_engine.h"
#include "string_convert.h"
#include <dwrite.h>
#include <wrl/client.h>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {

using Status = PeleOutcome::Status;

// ランタイムの用意 (バイトコードの読み込みと評価、約 5ms と約 3ms) を 1 回で済ませるため、テストの間で 1 つのエンジンを使う。
class PeleEngineTest : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        const std::filesystem::path third_party(MENDO_THIRD_PARTY_DIR);
        const std::filesystem::path bytecode(MENDO_JS_BYTECODE_DIR);
        pele_ = ReadFileBytes(bytecode / "pele.qbc");
        katex_ = ReadFileBytes(bytecode / "katex.qbc");
        ASSERT_FALSE(pele_.empty());
        ASSERT_FALSE(katex_.empty());
        fonts_.clear();
        for (const auto name : kKatexFontFiles) {
            fonts_.push_back(ReadFileBytes(third_party / "katex" / "fonts" / (std::wstring{ name } + L".woff2")));
            ASSERT_FALSE(fonts_.back().empty());
        }
        ASSERT_HRESULT_SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw_.GetAddressOf())));
        engine_ = NewEngine();
    }
    static void TearDownTestSuite()
    {
        engine_.reset();
        dw_.Reset();
    }
    static std::unique_ptr<PeleEngine> NewEngine()
    {
        std::vector<std::span<const std::byte>> fonts;
        for (const auto& f : fonts_) {
            fonts.push_back(std::as_bytes(std::span{ f }));
        }
        return std::make_unique<PeleEngine>(std::as_bytes(std::span{ pele_ }), std::as_bytes(std::span{ katex_ }), std::move(fonts), dw_.Get(), L"ja-jp");
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

    static inline std::string pele_;
    static inline std::string katex_;
    static inline std::vector<std::string> fonts_;
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

// 覚えておいた pele の出力をテーマと fit を変えて解き直しても、初めて描いたときと同じになる。
TEST_F(PeleEngineTest, ReusedPeleOutputMatchesFreshRender)
{
    constexpr std::string_view kCode = "flowchart LR\n  A[\"面積 $$x^2$$\"] --> B[bbbbbbbbbbbbbbbbbbbbbbbb] --> C[cccccccccccccccc]";
    ASSERT_EQ(engine_->Render(kCode, false, 300.0f).status, Status::Drawn);
    for (const bool dark : { false, true }) {
        for (const bool fit : { false, true }) {
            const auto reused = engine_->Render(kCode, dark, 300.0f, fit);
            const auto fresh = NewEngine()->Render(kCode, dark, 300.0f, fit);
            ASSERT_EQ(reused.status, Status::Drawn);
            ASSERT_EQ(fresh.status, Status::Drawn);
            EXPECT_EQ(reused.drawing, fresh.drawing) << "dark=" << dark << " fit=" << fit;
        }
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
