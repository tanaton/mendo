#pragma once
#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

// レンダーバックエンドの抽象インターフェース（D2Dファクトリ、レンダーターゲット、DPI）。
// ブラシとテキストフォーマットは抽象化コストが高すぎるためRendererに残す。
class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    virtual bool Init(HWND hwnd) = 0;
    virtual void Resize(UINT width, UINT height) noexcept = 0;
    virtual void SetDpi(float dpi) noexcept = 0;
    virtual float GetDpi() const noexcept = 0;
    virtual bool RecreateRenderTarget() = 0;

    virtual ID2D1Factory* GetD2DFactory() const noexcept = 0;
    virtual ID2D1DeviceContext* GetRenderTarget() const noexcept = 0;
    virtual IDWriteFactory* GetDWriteFactory() const noexcept = 0;
    virtual IWICImagingFactory* GetWICFactory() const noexcept = 0;
    virtual HWND GetHwnd() const noexcept = 0;

    // EndDraw 後に呼び出す。Present の HRESULT を返し、呼び出し側がデバイスロスト等を判定する。
    virtual HRESULT Present() noexcept = 0;

    // Resize / Present が DXGI_ERROR_DEVICE_REMOVED/RESET を検知した、もしくは
    // CreateSwapChainBitmap が失敗した時に true を返す。Renderer が次フレーム頭で
    // RecreateRenderTarget を呼んでフラグをクリアする想定。
    virtual bool IsDeviceLost() const noexcept = 0;
};

class D2DRenderBackend final : public IRenderBackend {
public:
    bool Init(HWND hwnd) override;
    void Resize(UINT width, UINT height) noexcept override;
    void SetDpi(float dpi) noexcept override;
    float GetDpi() const noexcept override
    {
        return dpi_;
    }
    bool RecreateRenderTarget() override;
    HRESULT Present() noexcept override;
    bool IsDeviceLost() const noexcept override
    {
        return device_lost_;
    }

    ID2D1Factory* GetD2DFactory() const noexcept override
    {
        return d2d_factory_.Get();
    }
    ID2D1DeviceContext* GetRenderTarget() const noexcept override
    {
        return device_context_.Get();
    }
    IDWriteFactory* GetDWriteFactory() const noexcept override
    {
        return dwrite_factory_.Get();
    }
    IWICImagingFactory* GetWICFactory() const noexcept override
    {
        return wic_factory_.Get();
    }
    HWND GetHwnd() const noexcept override
    {
        return hwnd_;
    }

private:
    bool CreateDeviceResources();
    bool CreateSwapChainBitmap();

    HWND hwnd_ = nullptr;
    float dpi_ = 96.0f;
    bool device_lost_ = false;
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2d_factory_;
    Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2d_device_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> device_context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_;
};
