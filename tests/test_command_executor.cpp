#include <gtest/gtest.h>
#include "command_executor.h"
#include "test_helpers.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
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

    HRESULT CreateRenderTarget(ComPtr<ID2D1RenderTarget>& out)
    {
        ComPtr<IWICBitmap> bitmap;
        if (FAILED(wic_factory_->CreateBitmap(32, 32,
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
        std::vector<uint32_t> pixels(32 * 32);
        EXPECT_HRESULT_SUCCEEDED(wic_bitmap_->CopyPixels(nullptr, 32 * 4, static_cast<UINT>(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data())));
        return pixels;
    }

    ComPtr<ID2D1Factory> d2d_factory_;
    ComPtr<IWICImagingFactory> wic_factory_;
    ComPtr<IWICBitmap> wic_bitmap_;
    ComPtr<ID2D1RenderTarget> rt_;
};

TEST_F(CommandExecutorIntegrationTest, ExecuteIgnoresNullRenderTarget)
{
    CommandExecutor exec;
    DrawCommandList cmds;
    cmds.emplace_back(FillRectCmd{ D2D1::RectF(0, 0, 10, 10), D2D1::ColorF(D2D1::ColorF::Red) });
    exec.Execute(cmds, nullptr);
}

TEST_F(CommandExecutorIntegrationTest, ExecuteEmptyListSucceeds)
{
    CommandExecutor exec;
    DrawCommandList cmds;
    rt_->BeginDraw();
    exec.Execute(cmds, rt_.Get());
    EXPECT_HRESULT_SUCCEEDED(rt_->EndDraw());
}

// 動的色は 1 本のブラシを SetColor で使い回すので、後続の色変更が先行の描画に影響しないこと。
TEST_F(CommandExecutorIntegrationTest, DistinctCustomColorsDrawWithOwnColor)
{
    CommandExecutor exec;
    DrawCommandList cmds;
    cmds.emplace_back(FillRectCmd{ D2D1::RectF(0, 0, 4, 4), D2D1::ColorF(D2D1::ColorF::Red) });
    cmds.emplace_back(FillRectCmd{ D2D1::RectF(4, 0, 8, 4), D2D1::ColorF(0x00FF00) });
    cmds.emplace_back(FillRectCmd{ D2D1::RectF(8, 0, 12, 4), D2D1::ColorF(D2D1::ColorF::Blue) });

    rt_->BeginDraw();
    exec.Execute(cmds, rt_.Get());
    ASSERT_HRESULT_SUCCEEDED(rt_->EndDraw());
    EXPECT_EQ(PixelAt(1, 1), 0xFFFF0000u);
    EXPECT_EQ(PixelAt(5, 1), 0xFF00FF00u);
    EXPECT_EQ(PixelAt(9, 1), 0xFF0000FFu);
}

TEST_F(CommandExecutorIntegrationTest, RenderTargetSwitchRecreatesBrush)
{
    CommandExecutor exec;
    DrawCommandList cmds;
    cmds.emplace_back(FillRectCmd{ D2D1::RectF(0, 0, 4, 4), D2D1::ColorF(D2D1::ColorF::Red) });

    rt_->BeginDraw();
    exec.Execute(cmds, rt_.Get());
    ASSERT_HRESULT_SUCCEEDED(rt_->EndDraw());

    ComPtr<ID2D1RenderTarget> rt2;
    ASSERT_HRESULT_SUCCEEDED(CreateRenderTarget(rt2));
    rt2->BeginDraw();
    exec.Execute(cmds, rt2.Get());
    ASSERT_HRESULT_SUCCEEDED(rt2->EndDraw());
    EXPECT_EQ(PixelAt(1, 1), 0xFFFF0000u);
}

TEST_F(CommandExecutorIntegrationTest, AllShapeCommandsExecuteWithoutError)
{
    CommandExecutor exec;
    DrawCommandList cmds;
    cmds.emplace_back(FillRectCmd{ D2D1::RectF(0, 0, 8, 8), D2D1::ColorF(D2D1::ColorF::Red) });
    cmds.emplace_back(FillRoundedRectCmd{
        D2D1::RectF(8, 0, 16, 8), 2.0f, 2.0f, D2D1::ColorF(D2D1::ColorF::Green) });
    cmds.emplace_back(DrawLineCmd{
        D2D1::Point2F(0, 16), D2D1::Point2F(32, 16),
        D2D1::ColorF(D2D1::ColorF::Blue), 1.0f });
    cmds.emplace_back(FillEllipseCmd{ D2D1::Point2F(20, 20), 4.0f, 4.0f, D2D1::ColorF(D2D1::ColorF::Yellow) });
    cmds.emplace_back(DrawEllipseCmd{ D2D1::Point2F(28, 28), 2.0f, 2.0f, D2D1::ColorF(D2D1::ColorF::Magenta), 1.0f });
    cmds.emplace_back(PushClipCmd{ D2D1::RectF(0, 0, 32, 32) });
    cmds.emplace_back(SetTransformCmd{ D2D1::Matrix3x2F::Identity() });
    cmds.emplace_back(PopClipCmd{});

    rt_->BeginDraw();
    exec.Execute(cmds, rt_.Get());
    EXPECT_HRESULT_SUCCEEDED(rt_->EndDraw());
}

TEST_F(CommandExecutorIntegrationTest, NullBitmapDrawIsSkipped)
{
    CommandExecutor exec;
    DrawCommandList cmds;
    cmds.emplace_back(DrawBitmapCmd{ nullptr, D2D1::RectF(0, 0, 8, 8) });

    rt_->BeginDraw();
    exec.Execute(cmds, rt_.Get());
    EXPECT_HRESULT_SUCCEEDED(rt_->EndDraw());
}

TEST_F(CommandExecutorIntegrationTest, NullTextLayoutDrawIsSkipped)
{
    CommandExecutor exec;
    DrawCommandList cmds;
    cmds.emplace_back(DrawTextLayoutCmd{
        D2D1::Point2F(0, 0), nullptr, D2D1::ColorF(D2D1::ColorF::Black) });

    rt_->BeginDraw();
    exec.Execute(cmds, rt_.Get());
    EXPECT_HRESULT_SUCCEEDED(rt_->EndDraw());
}

// cull_runs のレイアウトは可視ランだけ描く経路になる。D2D の DrawTextLayout と同じ画素になること。
TEST_F(CommandExecutorIntegrationTest, TallTextLayoutMatchesDrawTextLayout)
{
    ComPtr<IDWriteFactory> dw;
    ASSERT_HRESULT_SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &dw));
    ComPtr<IDWriteTextFormat> fmt;
    ASSERT_HRESULT_SUCCEEDED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &fmt));
    std::wstring text;
    for (int i = 0; i < 40; i++) {
        text += L"Wg_|\n";
    }
    ComPtr<IDWriteTextLayout> layout;
    ASSERT_HRESULT_SUCCEEDED(dw->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), fmt.Get(), 100.0f, 1000.0f, &layout));
    // 2 行目 (先頭行は描画面外) に下線と取り消し線を付ける。
    layout->SetUnderline(TRUE, DWRITE_TEXT_RANGE{ 5, 2 });
    layout->SetStrikethrough(TRUE, DWRITE_TEXT_RANGE{ 12, 2 });
    const D2D1_POINT_2F origin = D2D1::Point2F(1.0f, -13.5f);

    const auto render = [&](bool via_executor) {
        ComPtr<ID2D1RenderTarget> rt;
        EXPECT_HRESULT_SUCCEEDED(CreateRenderTarget(rt));
        CommandExecutor exec;
        DrawCommandList cmds;
        cmds.emplace_back(DrawTextLayoutCmd{ .origin = origin, .layout = layout.Get(), .color = D2D1::ColorF(D2D1::ColorF::Black), .cull_runs = true, .cull_top = 0.0f, .cull_bottom = 32.0f });
        ComPtr<ID2D1SolidColorBrush> black;
        rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &black);
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(D2D1::ColorF::White));
        if (via_executor) {
            exec.Execute(cmds, rt.Get());
        }
        else {
            rt->DrawTextLayout(origin, layout.Get(), black.Get());
        }
        EXPECT_HRESULT_SUCCEEDED(rt->EndDraw());
        return ReadAllPixels();
    };
    const auto expected = render(false);
    const auto actual = render(true);
    EXPECT_NE(std::ranges::count(expected, 0xFFFFFFFFu), static_cast<std::ptrdiff_t>(expected.size())) << "何も描かれていない";
    EXPECT_EQ(actual, expected);
}
