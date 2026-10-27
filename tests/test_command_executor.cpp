#include <gtest/gtest.h>
#include "draw_command.h"
#include "test_helpers.h"
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
        ASSERT_HRESULT_SUCCEEDED(CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&wic_factory_)));

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
    static FixedBrushArray MakeBrushes(ID2D1RenderTarget* rt, std::span<const BrushSpec> specs)
    {
        FixedBrushArray brushes;
        for (const auto& s : specs) {
            EXPECT_HRESULT_SUCCEEDED(rt->CreateSolidColorBrush(s.color, &brushes[std::to_underlying(s.id)]));
        }
        return brushes;
    }
    static FixedBrushArray MakeBrushes(ID2D1RenderTarget* rt, std::initializer_list<BrushSpec> specs)
    {
        return MakeBrushes(rt, std::span{ specs.begin(), specs.size() });
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

    ComPtr<IDWriteTextLayout> CreateTextLayout(std::wstring_view text, const wchar_t* family, const wchar_t* locale, float max_w, float max_h)
    {
        if (!dw_factory_) {
            EXPECT_HRESULT_SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &dw_factory_));
        }
        ComPtr<IDWriteTextFormat> fmt;
        EXPECT_HRESULT_SUCCEEDED(dw_factory_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, locale, &fmt));
        ComPtr<IDWriteTextLayout> layout;
        EXPECT_HRESULT_SUCCEEDED(dw_factory_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), fmt.Get(), max_w, max_h, &layout));
        return layout;
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
            ExecuteDrawCommands(cmds, rt.Get(), brushes);
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
    ExecuteDrawCommands(cmds, rt_.Get(), FixedBrushArray{});
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
    ExecuteDrawCommands(cmds, rt_.Get(), brushes);
    ASSERT_HRESULT_SUCCEEDED(rt_->EndDraw());
    EXPECT_EQ(PixelAt(1, 1), 0xFFFF0000u);
    EXPECT_EQ(PixelAt(5, 1), 0xFF00FF00u);
    EXPECT_EQ(PixelAt(9, 1), 0xFF0000FFu);
    EXPECT_EQ(PixelAt(13, 1), 0xFFFFFFFFu);
}

// opacity は描いた後にブラシへ残らない。
TEST_F(CommandExecutorIntegrationTest, RoundedRectOpacityIsRestored)
{
    const auto brushes = MakeBrushes(rt_.Get(), { { BrushId::OverlayBlack, D2D1::ColorF(D2D1::ColorF::Black) } });
    DrawCommandList cmds;
    cmds.emplace_back(FillRoundedRectCmd{ D2D1::RectF(0, 0, 8, 8), 0.0f, 0.0f, BrushId::OverlayBlack, 0.5f });
    cmds.emplace_back(FillRectCmd{ D2D1::RectF(8, 0, 16, 8), BrushId::OverlayBlack });

    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(D2D1::ColorF::White));
    ExecuteDrawCommands(cmds, rt_.Get(), brushes);
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
    cmds.emplace_back(FillRoundedRectCmd{ D2D1::RectF(8, 0, 16, 8), 2.0f, 2.0f, BrushId::Text });
    cmds.emplace_back(DrawLineCmd{ D2D1::Point2F(0, 16), D2D1::Point2F(32, 16), 1.0f, BrushId::Text });
    cmds.emplace_back(FillEllipseCmd{ D2D1::Point2F(20, 20), 4.0f, 4.0f, BrushId::Text });
    cmds.emplace_back(DrawEllipseCmd{ D2D1::Point2F(28, 28), 2.0f, 2.0f, 1.0f, BrushId::Text });
    cmds.emplace_back(DrawRoundedRectCmd{ D2D1::RectF(16, 0, 24, 8), 2.0f, 2.0f, 1.5f, BrushId::Text });
    cmds.emplace_back(PushClipCmd{ D2D1::RectF(0, 0, 32, 32) });
    cmds.emplace_back(SetTransformCmd{ D2D1::Matrix3x2F::Identity() });
    cmds.emplace_back(PopClipCmd{});

    rt_->BeginDraw();
    ExecuteDrawCommands(cmds, rt_.Get(), brushes);
    EXPECT_HRESULT_SUCCEEDED(rt_->EndDraw());
}

// 半透明の楕円と線のジオメトリも、描いた後にブラシへ不透明度を残さない。
TEST_F(CommandExecutorIntegrationTest, EllipseAndGeometryOpacityIsRestored)
{
    const auto brushes = MakeBrushes(rt_.Get(), { { BrushId::OverlayBlack, D2D1::ColorF(D2D1::ColorF::Black) } });
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
    cmds.emplace_back(FillEllipseCmd{ .center = D2D1::Point2F(4, 4), .rx = 4.0f, .ry = 4.0f, .brush_id = BrushId::OverlayBlack, .opacity = 0.5f });
    cmds.emplace_back(DrawGeometryCmd{ .geometry = path.Get(), .stroke_style = nullptr, .stroke_width = 4.0f, .brush_id = BrushId::OverlayBlack, .opacity = 0.5f });
    cmds.emplace_back(FillRectCmd{ D2D1::RectF(24, 0, 32, 8), BrushId::OverlayBlack });

    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(D2D1::ColorF::White));
    ExecuteDrawCommands(cmds, rt_.Get(), brushes);
    ASSERT_HRESULT_SUCCEEDED(rt_->EndDraw());
    for (const uint32_t half : { PixelAt(4, 4), PixelAt(8, 20) }) {
        EXPECT_NE(half, 0xFFFFFFFFu);
        EXPECT_NE(half, 0xFF000000u);
    }
    EXPECT_EQ(PixelAt(28, 4), 0xFF000000u);
}

TEST_F(CommandExecutorIntegrationTest, NullBitmapDrawIsSkipped)
{
    DrawCommandList cmds;
    cmds.emplace_back(DrawBitmapCmd{ nullptr, D2D1::RectF(0, 0, 8, 8) });

    rt_->BeginDraw();
    ExecuteDrawCommands(cmds, rt_.Get(), FixedBrushArray{});
    EXPECT_HRESULT_SUCCEEDED(rt_->EndDraw());
}

TEST_F(CommandExecutorIntegrationTest, NullTextLayoutDrawIsSkipped)
{
    DrawCommandList cmds;
    cmds.emplace_back(DrawTextLayoutCmd{ D2D1::Point2F(0, 0), nullptr, BrushId::Text });
    const auto brushes = MakeBrushes(rt_.Get(), { { BrushId::Text, D2D1::ColorF(D2D1::ColorF::Black) } });

    rt_->BeginDraw();
    ExecuteDrawCommands(cmds, rt_.Get(), brushes);
    EXPECT_HRESULT_SUCCEEDED(rt_->EndDraw());
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
    const D2D1_POINT_2F origin = D2D1::Point2F(1.0f, -13.5f);

    const auto render = [&](bool via_executor) {
        ComPtr<ID2D1RenderTarget> rt;
        EXPECT_HRESULT_SUCCEEDED(CreateRenderTarget(rt));
        DrawCommandList cmds;
        cmds.emplace_back(DrawTextLayoutCmd{ .origin = origin, .layout = layout.Get(), .brush_id = BrushId::Text, .cull_top = 0.0f, .cull_bottom = 32.0f });
        const auto brushes = MakeBrushes(rt.Get(), { { BrushId::Text, D2D1::ColorF(D2D1::ColorF::Black) } });
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(D2D1::ColorF::White));
        if (via_executor) {
            ExecuteDrawCommands(cmds, rt.Get(), brushes);
        }
        else {
            rt->DrawTextLayout(origin, layout.Get(), brushes[std::to_underlying(BrushId::Text)].Get());
        }
        EXPECT_HRESULT_SUCCEEDED(rt->EndDraw());
        return ReadAllPixels();
    };
    const auto expected = render(false);
    const auto actual = render(true);
    EXPECT_NE(std::ranges::count(expected, 0xFFFFFFFFu), static_cast<std::ptrdiff_t>(expected.size())) << "何も描かれていない";
    EXPECT_EQ(actual, expected);
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
