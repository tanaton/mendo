#include "d2d_util.h"
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <algorithm>

using Microsoft::WRL::ComPtr;

WindowSwapTarget::WindowSwapTarget() noexcept = default;

WindowSwapTarget::~WindowSwapTarget() = default;

bool WindowSwapTarget::Create(ID3D11Device* d3d, ID2D1Device* device, HWND hwnd, UINT width, UINT height, float dpi, Mode mode)
{
    Reset();
    const bool flip = mode == Mode::Flip;
    const DXGI_SWAP_CHAIN_DESC1 desc{
        .Width = std::max(width, 1u),
        .Height = std::max(height, 1u),
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .SampleDesc = { 1, 0 },
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = flip ? 2u : 1u,
        .Scaling = flip ? DXGI_SCALING_NONE : DXGI_SCALING_STRETCH,
        .SwapEffect = flip ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD,
    };
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(d3d->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateSwapChainForHwnd(d3d, hwnd, &desc, nullptr, nullptr, &swap_chain_)) ||
        FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_)) || !BindBackBuffer(dpi)) {
        Reset();
        return false;
    }
    return true;
}

bool WindowSwapTarget::Resize(UINT width, UINT height, float dpi)
{
    dc_->SetTarget(nullptr);
    if (FAILED(swap_chain_->ResizeBuffers(0, std::max(width, 1u), std::max(height, 1u), DXGI_FORMAT_UNKNOWN, 0)) || !BindBackBuffer(dpi)) {
        Reset();
        return false;
    }
    return true;
}

bool WindowSwapTarget::BindBackBuffer(float dpi)
{
    dc_->SetDpi(dpi, dpi);
    ComPtr<IDXGISurface> surface;
    ComPtr<ID2D1Bitmap1> target;
    // 不透明な窓なので IGNORE にする (PREMULTIPLIED だと文字が ClearType からグレースケールに落ちる)。
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                               D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi);
    if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&surface))) || FAILED(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target))) {
        return false;
    }
    dc_->SetTarget(target.Get());
    return true;
}

bool WindowSwapTarget::EndDrawAndPresent()
{
    HRESULT hr = dc_->EndDraw();
    if (SUCCEEDED(hr)) {
        hr = swap_chain_->Present(1, 0);
    }
    return hr != D2DERR_RECREATE_TARGET && !IsDeviceLostOnPresent(hr);
}

void WindowSwapTarget::Reset() noexcept
{
    if (dc_) {
        dc_->SetTarget(nullptr);
    }
    dc_.Reset();
    swap_chain_.Reset();
}
