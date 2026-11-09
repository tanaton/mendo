#include <gtest/gtest.h>
#include "renderer.h"
#include <windows.h>

namespace {

RenderDeviceState ReadyState()
{
    RenderDeviceState s;
    s.MarkCreated();
    return s;
}

} // namespace

// 作る前は描かず、フレームも待たない。
TEST(RenderDeviceStateTest, StartsWithoutDevice)
{
    RenderDeviceState s;
    EXPECT_FALSE(s.IsReady());
    EXPECT_FALSE(s.IsLost());
    EXPECT_FALSE(s.NeedsFrame());
    s.MarkFrameAcquired();
    s.MarkCreated();
    EXPECT_TRUE(s.NeedsFrame()) << "作る前に受け取った印を、作ったデバイスに持ち込まない";
}

// フレームを受け取ったら、表示するまで待ち直さない。
TEST(RenderDeviceStateTest, KeepsFrameUntilPresented)
{
    auto s = ReadyState();
    EXPECT_TRUE(s.NeedsFrame());
    s.MarkFrameAcquired();
    EXPECT_FALSE(s.NeedsFrame());
    s.OnPresent(S_OK);
    EXPECT_TRUE(s.NeedsFrame());
    EXPECT_TRUE(s.IsReady());
}

// 表示されなかったフレームはシグナルを消費しないので、受け取った扱いのまま次に回す。誤爆するエラーではロストにしない。
TEST(RenderDeviceStateTest, KeepsFrameWhenNotShown)
{
    for (const HRESULT hr : { DXGI_STATUS_OCCLUDED, DXGI_ERROR_WAS_STILL_DRAWING, DXGI_ERROR_INVALID_CALL }) {
        SCOPED_TRACE(hr);
        auto s = ReadyState();
        s.MarkFrameAcquired();
        s.OnPresent(hr);
        EXPECT_TRUE(s.IsReady());
        EXPECT_FALSE(s.NeedsFrame());
    }
}

// デバイスを失ったら、作り直すまで描かず、フレームも待たない。作り直したら受け取り直す。
TEST(RenderDeviceStateTest, PresentDeviceErrorsLoseDevice)
{
    for (const HRESULT hr : { DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET, DXGI_ERROR_DEVICE_HUNG, DXGI_ERROR_DRIVER_INTERNAL_ERROR }) {
        SCOPED_TRACE(hr);
        auto s = ReadyState();
        s.MarkFrameAcquired();
        s.OnPresent(hr);
        EXPECT_TRUE(s.IsLost());
        EXPECT_FALSE(s.NeedsFrame());
        s.MarkFrameAcquired();
        s.MarkCreated();
        EXPECT_TRUE(s.NeedsFrame());
    }
}

// EndDraw は描画先を作り直せと言われたらロストにする。ほかの失敗は表示しないだけで、受け取ったフレームを持ち越す。
TEST(RenderDeviceStateTest, EndDrawDecidesPresent)
{
    auto ok = ReadyState();
    EXPECT_TRUE(ok.OnEndDraw(S_OK));
    EXPECT_TRUE(ok.IsReady());

    auto lost = ReadyState();
    EXPECT_FALSE(lost.OnEndDraw(D2DERR_RECREATE_TARGET));
    EXPECT_TRUE(lost.IsLost());

    auto failed = ReadyState();
    failed.MarkFrameAcquired();
    EXPECT_FALSE(failed.OnEndDraw(E_FAIL));
    EXPECT_TRUE(failed.IsReady());
    EXPECT_FALSE(failed.NeedsFrame());
}

// 作り直しは失敗しても次の描画で再試行するよう、作り始めからロストにする (Renderer::CreateDevice)。
TEST(RenderDeviceStateTest, MarkLostDropsAcquiredFrame)
{
    auto s = ReadyState();
    s.MarkFrameAcquired();
    s.MarkLost();
    EXPECT_TRUE(s.IsLost());
    EXPECT_FALSE(s.IsReady());
    s.MarkCreated();
    EXPECT_TRUE(s.NeedsFrame());
}
