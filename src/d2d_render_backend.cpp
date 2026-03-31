#include "d2d_render_backend.h"

using Microsoft::WRL::ComPtr;

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

bool D2DRenderBackend::Init(HWND hwnd)
{
    hwnd_ = hwnd;

    // このウィンドウのモニターの実際のDPIを取得
    dpi_ = static_cast<float>(GetDpiForWindow(hwnd));
    if (dpi_ == 0.0f) {
        dpi_ = 96.0f;
    }

    // D2D 1.1 ファクトリを作成
    const D2D1_FACTORY_OPTIONS opts{};
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1), &opts,
        reinterpret_cast<void**>(d2d_factory_.GetAddressOf()));
    if (FAILED(hr)) {
        return false;
    }

    // DirectWriteファクトリを作成
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwrite_factory_.GetAddressOf()));
    if (FAILED(hr)) {
        return false;
    }

    // WICファクトリを作成（Renderer・ImageLoaderで共有）
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wic_factory_));

    // D3D11デバイスとD2Dデバイスコンテキストを作成
    if (!CreateDeviceResources()) {
        return false;
    }

    return true;
}

bool D2DRenderBackend::CreateDeviceResources()
{
    // D3D11 デバイスを作成
    const D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        flags, feature_levels, ARRAYSIZE(feature_levels),
        D3D11_SDK_VERSION, &d3d_device_, nullptr, nullptr);
    if (FAILED(hr)) {
        // ハードウェアが利用できない場合はWARPフォールバック
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            flags, feature_levels, ARRAYSIZE(feature_levels),
            D3D11_SDK_VERSION, &d3d_device_, nullptr, nullptr);
        if (FAILED(hr)) {
            return false;
        }
    }

    // DXGIデバイスを取得
    ComPtr<IDXGIDevice> dxgi_device;
    hr = d3d_device_.As(&dxgi_device);
    if (FAILED(hr)) {
        return false;
    }

    // D2Dデバイスを作成
    hr = d2d_factory_->CreateDevice(dxgi_device.Get(), &d2d_device_);
    if (FAILED(hr)) {
        return false;
    }

    // D2Dデバイスコンテキストを作成
    hr = d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &device_context_);
    if (FAILED(hr)) {
        return false;
    }

    device_context_->SetDpi(dpi_, dpi_);

    // DXGIスワップチェーンを作成
    ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(&adapter);
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IDXGIFactory2> dxgi_factory;
    hr = adapter->GetParent(IID_PPV_ARGS(&dxgi_factory));
    if (FAILED(hr)) {
        return false;
    }

    RECT rc;
    GetClientRect(hwnd_, &rc);

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = rc.right - rc.left;
    scd.Height = rc.bottom - rc.top;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    hr = dxgi_factory->CreateSwapChainForHwnd(
        d3d_device_.Get(), hwnd_, &scd, nullptr, nullptr, &swap_chain_);
    if (FAILED(hr)) {
        // FLIP_DISCARD非対応の場合はFLIP_SEQUENTIALで再試行
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        hr = dxgi_factory->CreateSwapChainForHwnd(
            d3d_device_.Get(), hwnd_, &scd, nullptr, nullptr, &swap_chain_);
        if (FAILED(hr)) {
            return false;
        }
    }

    // スワップチェーンのバックバッファからD2Dビットマップを作成
    if (!CreateSwapChainBitmap()) {
        return false;
    }

    return true;
}

bool D2DRenderBackend::CreateSwapChainBitmap()
{
    ComPtr<IDXGISurface> surface;
    HRESULT hr = swap_chain_->GetBuffer(0, IID_PPV_ARGS(&surface));
    if (FAILED(hr)) {
        return false;
    }

    const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        dpi_, dpi_);

    ComPtr<ID2D1Bitmap1> target_bitmap;
    hr = device_context_->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &target_bitmap);
    if (FAILED(hr)) {
        return false;
    }

    device_context_->SetTarget(target_bitmap.Get());
    return true;
}

void D2DRenderBackend::Resize(UINT width, UINT height)
{
    if (!swap_chain_ || !device_context_) {
        return;
    }
    if (width == 0 || height == 0) {
        return;
    }

    // ターゲットを解放してからリサイズ
    device_context_->SetTarget(nullptr);

    const HRESULT hr = swap_chain_->ResizeBuffers(0, width, height,
        DXGI_FORMAT_UNKNOWN, 0);
    if (SUCCEEDED(hr)) {
        CreateSwapChainBitmap();
    }
}

void D2DRenderBackend::SetDpi(float dpi)
{
    dpi_ = dpi;
    if (device_context_) {
        device_context_->SetDpi(dpi, dpi);
    }
}

void D2DRenderBackend::Present()
{
    if (swap_chain_) {
        swap_chain_->Present(1, 0);
    }
}

bool D2DRenderBackend::RecreateRenderTarget()
{
    device_context_.Reset();
    d2d_device_.Reset();
    swap_chain_.Reset();
    d3d_device_.Reset();

    return CreateDeviceResources();
}
