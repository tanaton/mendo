#pragma once
#include "win_handle.h"
#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>
// IDXGISwapChain2 / SetMaximumFrameLatency / GetFrameLatencyWaitableObject 用。
#include <dxgi1_3.h>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

// D2D ファクトリ・レンダーターゲット・DPI を保持するレンダーバックエンド。
// ブラシとテキストフォーマットは Renderer 側で管理する。
class D2DRenderBackend final {
public:
    bool Init(HWND hwnd);
    void Resize(UINT width, UINT height) noexcept;
    void SetDpi(float dpi) noexcept;
    float GetDpi() const noexcept
    {
        return dpi_;
    }
    bool RecreateRenderTarget();

    // BeginDraw 前に呼び出す。Frame Latency Waitable Object で GPU パイプラインの
    // 1 フレーム遅れに同期し、CPU 側を Present 直前まで詰めずに済ませる。
    void WaitForFrameLatency() noexcept;

    // EndDraw 後に呼び出す。Present の HRESULT を返し、呼び出し側がデバイスロスト等を判定する。
    HRESULT Present() noexcept;

    // Resize / Present が DXGI_ERROR_DEVICE_REMOVED/RESET を検知した、もしくは
    // CreateSwapChainBitmap が失敗した時に true を返す。Renderer が次フレーム頭で
    // RecreateRenderTarget を呼んでフラグをクリアする想定。
    bool IsDeviceLost() const noexcept
    {
        return device_lost_;
    }

    ID2D1Factory* GetD2DFactory() const noexcept
    {
        return d2d_factory_.Get();
    }
    ID2D1DeviceContext* GetRenderTarget() const noexcept
    {
        return device_context_.Get();
    }
    IDWriteFactory* GetDWriteFactory() const noexcept
    {
        return dwrite_factory_.Get();
    }
    IWICImagingFactory* GetWICFactory() const noexcept
    {
        return wic_factory_.Get();
    }
    HWND GetHwnd() const noexcept
    {
        return hwnd_;
    }

private:
    bool CreateDeviceResources();
    bool CreateSwapChainBitmap();
    void ConfigureFrameLatency() noexcept;

    HWND hwnd_ = nullptr;
    float dpi_ = 96.0f;
    bool device_lost_ = false;
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2d_factory_;
    Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2d_device_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> device_context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_;
    UniqueEventHandle frame_latency_waitable_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_;
};
