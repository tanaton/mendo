#include <gtest/gtest.h>
#include "d2d_util.h"
#include "draw_command.h"
#include "diagram_raster.h"
#include "inline_math.h"
#include "test_helpers.h"
#include "wic_util.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <span>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// ---- WIC bitmap render target を使った Execute の統合テスト ----

class CommandExecutorIntegrationTest : public ComApartmentTest {
protected:
    void SetUp() override
    {
        ASSERT_HRESULT_SUCCEEDED(D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            IID_PPV_ARGS(&d2d_factory_)));
        wic_factory_ = wic_util::CreateWicFactory("CommandExecutorIntegrationTest");
        ASSERT_NE(wic_factory_, nullptr);

        ASSERT_HRESULT_SUCCEEDED(CreateRenderTarget(rt_));
    }

    HRESULT CreateRenderTarget(ComPtr<ID2D1RenderTarget>& out, UINT size = 32)
    {
        size_ = size;
        ComPtr<IWICBitmap> bitmap;
        if (FAILED(wic_factory_->CreateBitmap(size, size,
            GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap))) {
            return E_FAIL;
        }
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1RenderTarget> rt;
        const HRESULT hr = d2d_factory_->CreateWicBitmapRenderTarget(bitmap.Get(), props, &rt);
        if (SUCCEEDED(hr)) {
            wic_bitmap_ = bitmap;
            out = rt;
        }
        return hr;
    }

    struct BrushSpec {
        BrushId id;
        D2D1_COLOR_F color;
    };
    static FixedBrushArray MakeBrushes(ID2D1RenderTarget* rt, const std::vector<BrushSpec>& specs)
    {
        FixedBrushArray brushes;
        for (const auto& s : specs) {
            EXPECT_HRESULT_SUCCEEDED(rt->CreateSolidColorBrush(s.color, &brushes[std::to_underlying(s.id)]));
        }
        return brushes;
    }

    // 32bppPBGRA の (x, y) を 0xAARRGGBB で返す。
    uint32_t PixelAt(UINT x, UINT y) const
    {
        WICRect rc{ static_cast<INT>(x), static_cast<INT>(y), 1, 1 };
        uint32_t px = 0;
        EXPECT_HRESULT_SUCCEEDED(wic_bitmap_->CopyPixels(&rc, 4, 4, reinterpret_cast<BYTE*>(&px)));
        return px;
    }

    std::vector<uint32_t> ReadAllPixels() const
    {
        std::vector<uint32_t> pixels(size_ * size_);
        EXPECT_HRESULT_SUCCEEDED(wic_bitmap_->CopyPixels(nullptr, size_ * 4, static_cast<UINT>(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data())));
        return pixels;
    }

    ComPtr<IDWriteTextLayout> CreateTextLayout(std::wstring_view text, const wchar_t* family, const wchar_t* locale, float max_w, float max_h, bool ellipsis = false)
    {
        if (!dw_factory_) {
            EXPECT_HRESULT_SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &dw_factory_));
        }
        const auto fmt = CreateTextFormat(dw_factory_.Get(), { .family = family, .size = 12.0f, .locale = locale, .ellipsis = ellipsis });
        EXPECT_NE(fmt, nullptr);
        ComPtr<IDWriteTextLayout> layout;
        EXPECT_HRESULT_SUCCEEDED(dw_factory_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), fmt.Get(), max_w, max_h, &layout));
        return layout;
    }

    // cmd を executor か D2D の DrawTextLayout で、黒の BrushId::Text で描いた画素。
    std::vector<uint32_t> RenderTextLayout(const DrawTextLayoutCmd& cmd, bool via_executor)
    {
        ComPtr<ID2D1RenderTarget> rt;
        EXPECT_HRESULT_SUCCEEDED(CreateRenderTarget(rt));
        const auto brushes = MakeBrushes(rt.Get(), { { BrushId::Text, D2D1::ColorF(D2D1::ColorF::Black) } });
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(D2D1::ColorF::White));
        if (via_executor) {
            DrawCommandList cmds;
            cmds.emplace_back(cmd);
            ExecuteDrawCommands(cmds, rt.Get(), brushes, no_math_);
        }
        else {
            rt->DrawTextLayout(cmd.origin, cmd.layout, brushes[std::to_underlying(BrushId::Text)].Get());
        }
        EXPECT_HRESULT_SUCCEEDED(rt->EndDraw());
        return ReadAllPixels();
    }

    // tokens で色付けした text を、executor 経由か SetDrawingEffect + DrawTextLayout で描いた画素。
    std::vector<uint32_t> RenderSyntaxColored(std::wstring_view text, std::span<const SyntaxToken> tokens, bool via_executor)
    {
        struct Style {
            SyntaxTokenType type;
            BrushId id;
            D2D1_COLOR_F color;
        };
        const Style styles[]{
            { SyntaxTokenType::Type, BrushId::SyntaxType, D2D1::ColorF(D2D1::ColorF::Red) },
            { SyntaxTokenType::Number, BrushId::SyntaxNumber, D2D1::ColorF(D2D1::ColorF::Green) },
            { SyntaxTokenType::Comment, BrushId::SyntaxComment, D2D1::ColorF(D2D1::ColorF::Blue) },
            { SyntaxTokenType::Keyword, BrushId::SyntaxKeyword, D2D1::ColorF(D2D1::ColorF::Magenta) },
            { SyntaxTokenType::String, BrushId::SyntaxString, D2D1::ColorF(D2D1::ColorF::Orange) },
            { SyntaxTokenType::Function, BrushId::SyntaxFunction, D2D1::ColorF(D2D1::ColorF::Teal) },
        };
        const D2D1_POINT_2F origin = D2D1::Point2F(1.5f, 2.0f);
        ComPtr<ID2D1RenderTarget> rt;
        EXPECT_HRESULT_SUCCEEDED(CreateRenderTarget(rt, 96));
        std::vector<BrushSpec> specs{ { BrushId::CodeText, D2D1::ColorF(D2D1::ColorF::Black) } };
        for (const auto& st : styles) {
            specs.push_back({ st.id, st.color });
        }
        const auto brushes = MakeBrushes(rt.Get(), specs);
        const auto layout = CreateTextLayout(text, L"Consolas", L"ja-jp", 1000.0f, 1000.0f);
        ComPtr<ID2D1SolidColorBrush> black;
        rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &black);
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(D2D1::ColorF::White));
        if (via_executor) {
            DrawCommandList cmds;
            cmds.emplace_back(DrawTextLayoutCmd{ .origin = origin, .layout = layout.Get(), .brush_id = BrushId::CodeText, .syntax_tokens = tokens });
            ExecuteDrawCommands(cmds, rt.Get(), brushes, no_math_);
        }
        else {
            for (const auto& t : tokens) {
                const auto it = std::ranges::find(styles, t.type, &Style::type);
                layout->SetDrawingEffect(brushes[std::to_underlying(it->id)].Get(), DWRITE_TEXT_RANGE{ t.start, t.length });
            }
            rt->DrawTextLayout(origin, layout.Get(), black.Get());
        }
        EXPECT_HRESULT_SUCCEEDED(rt->EndDraw());
        return ReadAllPixels();
    }

    static bool HasColoredPixel(std::span<const uint32_t> pixels)
    {
        return std::ranges::any_of(pixels, [](uint32_t px) static { return (px & 0xFF) != ((px >> 8) & 0xFF); });
    }

    UINT size_ = 32;
    InlineMathDrawings no_math_;
    ComPtr<IDWriteFactory> dw_factory_;
    ComPtr<ID2D1Factory> d2d_factory_;
    ComPtr<IWICImagingFactory> wic_factory_;
    ComPtr<IWICBitmap> wic_bitmap_;
    ComPtr<ID2D1RenderTarget> rt_;
};

TEST_F(CommandExecutorIntegrationTest, ExecuteEmptyListSucceeds)
{
    DrawCommandList cmds;
    rt_->BeginDraw();
    ExecuteDrawCommands(cmds, rt_.Get(), FixedBrushArray{}, no_math_);
    EXPECT_HRESULT_SUCCEEDED(rt_->EndDraw());
}

// 各コマンドは brush_id の固定ブラシで描かれる。
TEST_F(CommandExecutorIntegrationTest, FixedBrushesDrawWithOwnColor)
{
    const auto brushes = MakeBrushes(rt_.Get(), {
        { BrushId::Text, D2D1::ColorF(D2D1::ColorF::Red) },
        { BrushId::Heading, D2D1::ColorF(0x00FF00) },
        { BrushId::Link, D2D1::ColorF(D2D1::ColorF::Blue) },
    });
    DrawCommandList cmds;
    cmds.emplace_back(FillRectCmd{ D2D1::RectF(0, 0, 4, 4), BrushId::Text });
    cmds.emplace_back(FillRectCmd{ D2D1::RectF(4, 0, 8, 4), BrushId::Heading });
    cmds.emplace_back(FillRectCmd{ D2D1::RectF(8, 0, 12, 4), BrushId::Link });
    // 未作成のブラシは描かない。
    cmds.emplace_back(FillRectCmd{ D2D1::RectF(12, 0, 16, 4), BrushId::Hr });

    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(D2D1::ColorF::White));
    ExecuteDrawCommands(cmds, rt_.Get(), brushes, no_math_);
    ASSERT_HRESULT_SUCCEEDED(rt_->EndDraw());
    EXPECT_EQ(PixelAt(1, 1), 0xFFFF0000u);
    EXPECT_EQ(PixelAt(5, 1), 0xFF00FF00u);
    EXPECT_EQ(PixelAt(9, 1), 0xFF0000FFu);
    EXPECT_EQ(PixelAt(13, 1), 0xFFFFFFFFu);
}

// 図のコマンドリストは dest の幅に合わせて縮めて描き、後のコマンドの変換を元に戻す。
TEST_F(CommandExecutorIntegrationTest, DiagramCommandListIsScaledToDest)
{
    ComPtr<ID2D1DeviceContext> dc;
    ASSERT_HRESULT_SUCCEEDED(rt_.As(&dc));
    ComPtr<ID2D1CommandList> list;
    ComPtr<ID2D1SolidColorBrush> red;
    ASSERT_HRESULT_SUCCEEDED(dc->CreateCommandList(&list));
    ASSERT_HRESULT_SUCCEEDED(dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Red), &red));
    ComPtr<ID2D1Image> target;
    dc->GetTarget(&target);
    dc->SetTarget(list.Get());
    dc->BeginDraw();
    dc->FillRectangle(D2D1::RectF(0, 0, 20, 20), red.Get());
    ASSERT_HRESULT_SUCCEEDED(dc->EndDraw());
    ASSERT_HRESULT_SUCCEEDED(list->Close());
    dc->SetTarget(target.Get());

    const auto brushes = MakeBrushes(rt_.Get(), { { BrushId::Link, D2D1::ColorF(D2D1::ColorF::Blue) } });
    DrawCommandList cmds;
    // 幅 20 で記録した図を 10 の枠に描く
    cmds.emplace_back(DrawDiagramCmd{ list.Get(), D2D1::RectF(4, 4, 14, 14), 20.0f });
    cmds.emplace_back(FillRectCmd{ D2D1::RectF(20, 20, 24, 24), BrushId::Link });
    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(D2D1::ColorF::White));
    ExecuteDrawCommands(cmds, rt_.Get(), brushes, no_math_);
    ASSERT_HRESULT_SUCCEEDED(rt_->EndDraw());
    EXPECT_EQ(PixelAt(2, 2), 0xFFFFFFFFu);
    EXPECT_EQ(PixelAt(5, 5), 0xFFFF0000u);
    EXPECT_EQ(PixelAt(13, 13), 0xFFFF0000u);
    EXPECT_EQ(PixelAt(16, 16), 0xFFFFFFFFu);
    EXPECT_EQ(PixelAt(21, 21), 0xFF0000FFu);
}

// opacity は描いた後にブラシへ残らない。
TEST_F(CommandExecutorIntegrationTest, RoundedRectOpacityIsRestored)
{
    const auto brushes = MakeBrushes(rt_.Get(), { { BrushId::Overlay, D2D1::ColorF(D2D1::ColorF::Black) } });
    DrawCommandList cmds;
    cmds.emplace_back(FillRoundedRectCmd{ D2D1::RectF(0, 0, 8, 8), 0.0f, BrushId::Overlay, 0.5f });
    cmds.emplace_back(FillRectCmd{ D2D1::RectF(8, 0, 16, 8), BrushId::Overlay });

    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(D2D1::ColorF::White));
    ExecuteDrawCommands(cmds, rt_.Get(), brushes, no_math_);
    ASSERT_HRESULT_SUCCEEDED(rt_->EndDraw());
    const uint32_t half = PixelAt(4, 4);
    EXPECT_NE(half, 0xFFFFFFFFu);
    EXPECT_NE(half, 0xFF000000u);
    EXPECT_EQ(PixelAt(12, 4), 0xFF000000u);
}

TEST_F(CommandExecutorIntegrationTest, AllShapeCommandsExecuteWithoutError)
{
    DrawCommandList cmds;
    const auto brushes = MakeBrushes(rt_.Get(), { { BrushId::Text, D2D1::ColorF(D2D1::ColorF::Red) } });
    cmds.emplace_back(FillRectCmd{ D2D1::RectF(0, 0, 8, 8), BrushId::Text });
    cmds.emplace_back(FillRoundedRectCmd{ D2D1::RectF(8, 0, 16, 8), 2.0f, BrushId::Text });
    cmds.emplace_back(DrawLineCmd{ D2D1::Point2F(0, 16), D2D1::Point2F(32, 16), 1.0f, BrushId::Text });
    cmds.emplace_back(FillEllipseCmd{ D2D1::Point2F(20, 20), 4.0f, BrushId::Text });
    cmds.emplace_back(DrawEllipseCmd{ D2D1::Point2F(28, 28), 2.0f, 1.0f, BrushId::Text });
    cmds.emplace_back(DrawRoundedRectCmd{ D2D1::RectF(16, 0, 24, 8), 2.0f, 1.5f, BrushId::Text });
    cmds.emplace_back(PushClipCmd{ D2D1::RectF(0, 0, 32, 32) });
    cmds.emplace_back(SetTransformCmd{ D2D1::Matrix3x2F::Identity() });
    cmds.emplace_back(PopClipCmd{});

    rt_->BeginDraw();
    ExecuteDrawCommands(cmds, rt_.Get(), brushes, no_math_);
    EXPECT_HRESULT_SUCCEEDED(rt_->EndDraw());
}

// 半透明の楕円と線のジオメトリも、描いた後にブラシへ不透明度を残さない。
TEST_F(CommandExecutorIntegrationTest, EllipseAndGeometryOpacityIsRestored)
{
    const auto brushes = MakeBrushes(rt_.Get(), { { BrushId::Overlay, D2D1::ColorF(D2D1::ColorF::Black) } });
    ComPtr<ID2D1PathGeometry> path;
    ASSERT_HRESULT_SUCCEEDED(d2d_factory_->CreatePathGeometry(&path));
    {
        ComPtr<ID2D1GeometrySink> sink;
        ASSERT_HRESULT_SUCCEEDED(path->Open(&sink));
        sink->BeginFigure(D2D1::Point2F(0, 20), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine(D2D1::Point2F(16, 20));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        ASSERT_HRESULT_SUCCEEDED(sink->Close());
    }
    DrawCommandList cmds;
    cmds.emplace_back(FillEllipseCmd{ .center = D2D1::Point2F(4, 4), .radius = 4.0f, .brush_id = BrushId::Overlay, .opacity = 0.5f });
    cmds.emplace_back(DrawGeometryCmd{ .geometry = path.Get(), .stroke_style = nullptr, .stroke_width = 4.0f, .brush_id = BrushId::Overlay, .opacity = 0.5f });
    cmds.emplace_back(FillRectCmd{ D2D1::RectF(24, 0, 32, 8), BrushId::Overlay });

    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(D2D1::ColorF::White));
    ExecuteDrawCommands(cmds, rt_.Get(), brushes, no_math_);
    ASSERT_HRESULT_SUCCEEDED(rt_->EndDraw());
    for (const uint32_t half : { PixelAt(4, 4), PixelAt(8, 20) }) {
        EXPECT_NE(half, 0xFFFFFFFFu);
        EXPECT_NE(half, 0xFF000000u);
    }
    EXPECT_EQ(PixelAt(28, 4), 0xFF000000u);
}

// 間引き範囲を持つレイアウトは可視ランだけ描く経路になる。D2D の DrawTextLayout と同じ画素になること。
TEST_F(CommandExecutorIntegrationTest, TallTextLayoutMatchesDrawTextLayout)
{
    std::wstring text;
    for (int i = 0; i < 40; i++) {
        text += L"Wg_|\n";
    }
    const auto layout = CreateTextLayout(text, L"Segoe UI", L"en-us", 100.0f, 1000.0f);
    ASSERT_TRUE(layout);
    // 2 行目 (先頭行は描画面外) に下線と取り消し線を付ける。
    layout->SetUnderline(TRUE, DWRITE_TEXT_RANGE{ 5, 2 });
    layout->SetStrikethrough(TRUE, DWRITE_TEXT_RANGE{ 12, 2 });
    const DrawTextLayoutCmd cmd{ .origin = D2D1::Point2F(1.0f, -13.5f), .layout = layout.Get(), .brush_id = BrushId::Text, .cull_top = 0.0f, .cull_bottom = 32.0f };
    const auto expected = RenderTextLayout(cmd, false);
    EXPECT_NE(std::ranges::count(expected, 0xFFFFFFFFu), static_cast<std::ptrdiff_t>(expected.size())) << "何も描かれていない";
    EXPECT_EQ(RenderTextLayout(cmd, true), expected);
}

// 書式の省略記号はインラインオブジェクトとして DrawInlineObject に届く。数式と取り違えずに描き、
// D2D の DrawTextLayout と同じ画素になること。
TEST_F(CommandExecutorIntegrationTest, TrimmingSignMatchesDrawTextLayout)
{
    const auto layout = CreateTextLayout(L"Wg Wg Wg Wg Wg Wg Wg Wg", L"Segoe UI", L"en-us", 30.0f, 20.0f, true);
    ASSERT_TRUE(layout);
    const DrawTextLayoutCmd cmd{ .origin = D2D1::Point2F(1.0f, 2.0f), .layout = layout.Get(), .brush_id = BrushId::Text };
    const auto expected = RenderTextLayout(cmd, false);
    EXPECT_NE(std::ranges::count(expected, 0xFFFFFFFFu), static_cast<std::ptrdiff_t>(expected.size())) << "何も描かれていない";
    EXPECT_EQ(RenderTextLayout(cmd, true), expected);
}

// インライン数式は描画命令を周りの文字の色で、箱の左上をベースラインから height だけ上にして描く。
// 描画命令の無い数式 (描けなかった・まだ届いていない) は何も描かない。
TEST_F(CommandExecutorIntegrationTest, InlineMathDrawsDrawingInTextColor)
{
    const auto layout = CreateTextLayout(L"\uFFFC \uFFFC", L"Segoe UI", L"en-us", 100.0f, 32.0f);
    ASSERT_TRUE(layout);
    ASSERT_HRESULT_SUCCEEDED(layout->SetInlineObject(MakeInlineMathObject(1, { 10.0f, 10.0f, 8.0f }, 1.0f).Get(), DWRITE_TEXT_RANGE{ 0, 1 }));
    ASSERT_HRESULT_SUCCEEDED(layout->SetInlineObject(MakeInlineMathObject(2, { 10.0f, 10.0f, 8.0f }, 1.0f).Get(), DWRITE_TEXT_RANGE{ 2, 1 }));
    auto drawing = std::make_shared<MathDrawing>(MathDrawing{ .width = 10.0f, .height = 8.0f, .depth = 2.0f });
    drawing->items.emplace_back(MathDrawing::Rect{ .x = 0.0f, .y = -8.0f, .w = 10.0f, .h = 10.0f, .side = MathDrawing::Rect::Side::Top });
    InlineMathDrawings math;
    *math.drawings.TryEmplace(1).first = std::move(drawing);
    DrawCommandList cmds;
    cmds.emplace_back(DrawTextLayoutCmd{ .origin = D2D1::Point2F(0.0f, 0.0f), .layout = layout.Get(), .brush_id = BrushId::Text, .cull_top = 0.0f, .cull_bottom = 32.0f });
    const auto brushes = MakeBrushes(rt_.Get(), { { BrushId::Text, D2D1::ColorF(D2D1::ColorF::Red) } });
    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(D2D1::ColorF::White));
    ExecuteDrawCommands(cmds, rt_.Get(), brushes, math);
    EXPECT_HRESULT_SUCCEEDED(rt_->EndDraw());
    const auto pixels = ReadAllPixels();
    EXPECT_EQ(std::ranges::count(pixels, 0xFFFF0000u), 100) << "1 つ目の数式の 10x10 だけを文字の色で塗る";
}

// 構文トークン付きのレイアウトはグリフランを分けて色を付ける。SetDrawingEffect で色を付けた
// DrawTextLayout と同じ画素になること (フォールバックで別ランになる和文・ランの途中の境界・行またぎを含む)。
TEST_F(CommandExecutorIntegrationTest, SyntaxTokensMatchSetDrawingEffect)
{
    const std::wstring_view text = L"int x=0;//あい\nreturn \"s\";\n/* a\nb */ f(1)";
    const SyntaxToken tokens[]{
        { 0, 3, SyntaxTokenType::Type },
        { 6, 1, SyntaxTokenType::Number },
        { 8, 4, SyntaxTokenType::Comment },
        { 13, 6, SyntaxTokenType::Keyword },
        { 20, 3, SyntaxTokenType::String },
        { 25, 9, SyntaxTokenType::Comment },
        { 35, 1, SyntaxTokenType::Function },
        { 37, 1, SyntaxTokenType::Number },
    };
    const auto expected = RenderSyntaxColored(text, tokens, false);
    const auto actual = RenderSyntaxColored(text, tokens, true);
    EXPECT_TRUE(HasColoredPixel(expected)) << "色付きの画素が無い";
    EXPECT_EQ(actual, expected);
}

// 右から左のラン (ヘブライ語) の途中にある境界でも同じ画素になること。
TEST_F(CommandExecutorIntegrationTest, SyntaxTokensMatchSetDrawingEffectRtl)
{
    const std::wstring_view text = L"x=\"\u05E9\u05DC\u05D5\u05DD \u05E2\u05D5\u05DC\u05DD\";";
    const SyntaxToken tokens[]{
        { 2, 6, SyntaxTokenType::String },
        { 11, 2, SyntaxTokenType::Number },
    };
    const auto expected = RenderSyntaxColored(text, tokens, false);
    const auto actual = RenderSyntaxColored(text, tokens, true);
    EXPECT_TRUE(HasColoredPixel(expected)) << "色付きの画素が無い";
    EXPECT_EQ(actual, expected);
}

// 横スクロールの clip の外のグリフを飛ばしても、clip に映る画素は全部描いたときと同じ。
TEST_F(CommandExecutorIntegrationTest, HorizontalCullKeepsPixelsInsideClip)
{
    std::wstring ltr;
    std::vector<SyntaxToken> tokens;
    for (uint32_t i = 0; i < 80; ++i) {
        tokens.push_back({ i * 9, 3, SyntaxTokenType::Keyword });
        ltr += L"int x=0; ";
    }
    std::wstring rtl;
    for (int i = 0; i < 40; ++i) {
        rtl += L"\u05E9\u05DC\u05D5\u05DD ";
    }
    struct Case {
        const char* name;
        std::wstring_view text;
        std::span<const SyntaxToken> tokens;
    };
    const Case cases[]{
        { "Tokens", ltr, tokens },
        { "NoTokens", ltr, {} },
        { "RightToLeft", rtl, {} },
    };
    constexpr UINT kSize = 96;
    constexpr float kScrollX = 300.0f;
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        const auto layout = CreateTextLayout(c.text, L"Consolas", L"ja-jp", 4000.0f, 40.0f);
        ASSERT_TRUE(layout);
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        const auto render = [&](bool cull) {
            ComPtr<ID2D1RenderTarget> rt;
            EXPECT_HRESULT_SUCCEEDED(CreateRenderTarget(rt, kSize));
            const auto brushes = MakeBrushes(rt.Get(), { { BrushId::Text, D2D1::ColorF(D2D1::ColorF::Black) }, { BrushId::SyntaxKeyword, D2D1::ColorF(D2D1::ColorF::Red) } });
            DrawTextLayoutCmd text{ .origin = D2D1::Point2F(0.0f, 2.0f), .layout = layout.Get(), .brush_id = BrushId::Text, .syntax_tokens = c.tokens };
            if (cull) {
                text.cull_left = kScrollX;
                text.cull_right = kScrollX + static_cast<float>(kSize);
            }
            DrawCommandList cmds;
            cmds.emplace_back(SetTransformCmd{ D2D1::Matrix3x2F::Translation(-kScrollX, 0.0f) });
            cmds.emplace_back(text);
            rt->BeginDraw();
            rt->Clear(D2D1::ColorF(D2D1::ColorF::White));
            ExecuteDrawCommands(cmds, rt.Get(), brushes, no_math_);
            EXPECT_HRESULT_SUCCEEDED(rt->EndDraw());
            return ReadAllPixels();
        };
        const auto expected = render(false);
        EXPECT_NE(std::ranges::count(expected, 0xFFFFFFFFu), static_cast<std::ptrdiff_t>(expected.size())) << "何も描かれていない";
        EXPECT_EQ(render(true), expected);
    }
}
