#pragma once
#include "render_backend.h"
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl/client.h>

class D2DRenderBackend final : public IRenderBackend {
public:
    bool Init(HWND hwnd) override;
    void Resize(UINT width, UINT height) noexcept override;
    void SetDpi(float dpi) noexcept override;
    float GetDpi() const noexcept override { return dpi_; }
    bool RecreateRenderTarget() override;
    void Present() noexcept override;

    ID2D1Factory* GetD2DFactory() const noexcept override { return d2d_factory_.Get(); }
    ID2D1DeviceContext* GetRenderTarget() const noexcept override { return device_context_.Get(); }
    IDWriteFactory* GetDWriteFactory() const noexcept override { return dwrite_factory_.Get(); }
    IWICImagingFactory* GetWICFactory() const noexcept override { return wic_factory_.Get(); }
    HWND GetHwnd() const noexcept override { return hwnd_; }

private:
    bool CreateDeviceResources();
    bool CreateSwapChainBitmap();

    HWND hwnd_ = nullptr;
    float dpi_ = 96.0f;
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2d_factory_;
    Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2d_device_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> device_context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_;
};
