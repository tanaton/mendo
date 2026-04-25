#include <gtest/gtest.h>
#include "command_executor.h"
#include "test_helpers.h"
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using command_executor_internal::PackColor;

// ---- PackColor の単体テスト ----

TEST(CommandExecutorPackColor, FullyTransparentBlackIsZero)
{
    EXPECT_EQ(PackColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f)), 0u);
}

TEST(CommandExecutorPackColor, FullyOpaqueWhiteIsAllOnes)
{
    EXPECT_EQ(PackColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)), 0xFFFFFFFFu);
}

TEST(CommandExecutorPackColor, ChannelOrderIsRGBA)
{
    // R=255, G=0, B=0, A=255 → 0xFF0000FF
    EXPECT_EQ(PackColor(D2D1::ColorF(1.0f, 0.0f, 0.0f, 1.0f)), 0xFF0000FFu);
    // R=0, G=255, B=0, A=255 → 0x00FF00FF
    EXPECT_EQ(PackColor(D2D1::ColorF(0.0f, 1.0f, 0.0f, 1.0f)), 0x00FF00FFu);
    // R=0, G=0, B=255, A=255 → 0x0000FFFF
    EXPECT_EQ(PackColor(D2D1::ColorF(0.0f, 0.0f, 1.0f, 1.0f)), 0x0000FFFFu);
}

TEST(CommandExecutorPackColor, ClampsBelowZero)
{
    // 範囲外（負値）は 0 にクランプされる
    EXPECT_EQ(PackColor(D2D1::ColorF(-1.0f, -2.0f, -0.5f, -3.0f)), 0u);
}

TEST(CommandExecutorPackColor, ClampsAboveOne)
{
    // 範囲外（1超え）は 255 にクランプされる
    EXPECT_EQ(PackColor(D2D1::ColorF(2.0f, 5.0f, 100.0f, 10.0f)), 0xFFFFFFFFu);
}

TEST(CommandExecutorPackColor, RoundsToNearest)
{
    // 0.5f * 255 + 0.5 = 128.0 → 128
    const auto v = PackColor(D2D1::ColorF(0.5f, 0.5f, 0.5f, 0.5f));
    EXPECT_EQ((v >> 24) & 0xFF, 128u);
    EXPECT_EQ((v >> 16) & 0xFF, 128u);
    EXPECT_EQ((v >> 8) & 0xFF, 128u);
    EXPECT_EQ(v & 0xFF, 128u);
}

TEST(CommandExecutorPackColor, DistinctColorsHaveDistinctKeys)
{
    EXPECT_NE(PackColor(D2D1::ColorF(0.1f, 0.2f, 0.3f, 1.0f)),
        PackColor(D2D1::ColorF(0.1f, 0.2f, 0.4f, 1.0f)));
}

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
    EXPECT_EQ(exec.PoolSizeForTest(), 0u);
}

TEST_F(CommandExecutorIntegrationTest, ExecuteEmptyListSucceeds)
{
    CommandExecutor exec;
    DrawCommandList cmds;
    rt_->BeginDraw();
    exec.Execute(cmds, rt_.Get());
    EXPECT_HRESULT_SUCCEEDED(rt_->EndDraw());
    EXPECT_EQ(exec.PoolSizeForTest(), 0u);
}

TEST_F(CommandExecutorIntegrationTest, FillRectCreatesPooledBrush)
{
    CommandExecutor exec;
    DrawCommandList cmds;
    cmds.emplace_back(FillRectCmd{ D2D1::RectF(0, 0, 32, 32), D2D1::ColorF(D2D1::ColorF::Red) });

    rt_->BeginDraw();
    exec.Execute(cmds, rt_.Get());
    EXPECT_HRESULT_SUCCEEDED(rt_->EndDraw());

    EXPECT_EQ(exec.PoolSizeForTest(), 1u);
    EXPECT_EQ(exec.BoundRtForTest(), rt_.Get());
}

TEST_F(CommandExecutorIntegrationTest, SameColorReusesPooledBrush)
{
    CommandExecutor exec;
    DrawCommandList cmds;
    // 同色を繰り返し描画 → ブラシは1つだけプールされる
    for (int i = 0; i < 5; i++) {
        cmds.emplace_back(FillRectCmd{ D2D1::RectF(0, 0, 4, 4), D2D1::ColorF(D2D1::ColorF::Red) });
    }
    rt_->BeginDraw();
    exec.Execute(cmds, rt_.Get());
    EXPECT_HRESULT_SUCCEEDED(rt_->EndDraw());
    EXPECT_EQ(exec.PoolSizeForTest(), 1u);
}

TEST_F(CommandExecutorIntegrationTest, DistinctColorsCreateDistinctBrushes)
{
    CommandExecutor exec;
    DrawCommandList cmds;
    cmds.emplace_back(FillRectCmd{ D2D1::RectF(0, 0, 4, 4), D2D1::ColorF(D2D1::ColorF::Red) });
    cmds.emplace_back(FillRectCmd{ D2D1::RectF(4, 0, 8, 4), D2D1::ColorF(D2D1::ColorF::Green) });
    cmds.emplace_back(FillRectCmd{ D2D1::RectF(8, 0, 12, 4), D2D1::ColorF(D2D1::ColorF::Blue) });

    rt_->BeginDraw();
    exec.Execute(cmds, rt_.Get());
    EXPECT_HRESULT_SUCCEEDED(rt_->EndDraw());
    EXPECT_EQ(exec.PoolSizeForTest(), 3u);
}

TEST_F(CommandExecutorIntegrationTest, RenderTargetSwitchClearsPool)
{
    CommandExecutor exec;
    DrawCommandList cmds;
    cmds.emplace_back(FillRectCmd{ D2D1::RectF(0, 0, 4, 4), D2D1::ColorF(D2D1::ColorF::Red) });
    cmds.emplace_back(FillRectCmd{ D2D1::RectF(4, 0, 8, 4), D2D1::ColorF(D2D1::ColorF::Green) });

    rt_->BeginDraw();
    exec.Execute(cmds, rt_.Get());
    EXPECT_HRESULT_SUCCEEDED(rt_->EndDraw());
    EXPECT_EQ(exec.PoolSizeForTest(), 2u);

    // 別の RT に切り替えるとプールはクリアされる
    ComPtr<ID2D1RenderTarget> rt2;
    ASSERT_HRESULT_SUCCEEDED(CreateRenderTarget(rt2));

    DrawCommandList cmds2;
    cmds2.emplace_back(FillRectCmd{ D2D1::RectF(0, 0, 4, 4), D2D1::ColorF(D2D1::ColorF::Red) });
    rt2->BeginDraw();
    exec.Execute(cmds2, rt2.Get());
    EXPECT_HRESULT_SUCCEEDED(rt2->EndDraw());
    EXPECT_EQ(exec.PoolSizeForTest(), 1u); // 切替後の Red のみ
    EXPECT_EQ(exec.BoundRtForTest(), rt2.Get());
}

TEST_F(CommandExecutorIntegrationTest, AllShapeCommandsExecuteWithoutError)
{
    CommandExecutor exec;
    DrawCommandList cmds;
    cmds.emplace_back(ClearCmd{ D2D1::ColorF(D2D1::ColorF::Black, 0.0f) });
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
    cmds.emplace_back(DrawBitmapCmd{
        nullptr, D2D1::RectF(0, 0, 8, 8), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR });

    rt_->BeginDraw();
    exec.Execute(cmds, rt_.Get());
    EXPECT_HRESULT_SUCCEEDED(rt_->EndDraw());
    // bitmap 未指定なら何も描画されず、ブラシも不要
    EXPECT_EQ(exec.PoolSizeForTest(), 0u);
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
    EXPECT_EQ(exec.PoolSizeForTest(), 0u);
}
