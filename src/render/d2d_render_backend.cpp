#include "d2d_render_backend.h"
#include "log_hr.h"

using Microsoft::WRL::ComPtr;

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

void D2DRenderBackend::ConfigureFrameLatency() noexcept
{
    frame_latency_waitable_.reset();
    if (!swap_chain_) {
        return;
    }
    ComPtr<IDXGISwapChain2> sc2;
    if (FAILED(swap_chain_.As(&sc2)) || !sc2) {
        return;
    }
    // 既定の MaximumFrameLatency=3 はホイール連打でカクつきの原因になる。1 まで詰める。
    (void)sc2->SetMaximumFrameLatency(1);
    frame_latency_waitable_.reset(sc2->GetFrameLatencyWaitableObject());
}

bool D2DRenderBackend::Init(HWND hwnd)
{
    hwnd_ = hwnd;

    dpi_ = static_cast<float>(GetDpiForWindow(hwnd));
    if (dpi_ == 0.0f) {
        dpi_ = 96.0f;
    }

    const D2D1_FACTORY_OPTIONS opts{};
    HRESULT hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1),
        &opts,
        reinterpret_cast<void**>(d2d_factory_.GetAddressOf()));
    if (FAILED(hr)) {
        mendo::LogHrFailure(L"D2D1CreateFactory", hr);
        return false;
    }

    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwrite_factory_.GetAddressOf()));
    if (FAILED(hr)) {
        mendo::LogHrFailure(L"DWriteCreateFactory", hr);
        return false;
    }

    // Renderer・ImageLoader で共有。アイコン／画像／ダイアグラムは WIC が無いと
    // 機能しないため fail-fast。
    hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wic_factory_));
    if (FAILED(hr)) {
        mendo::LogHrFailure(L"WIC ImagingFactory creation", hr);
        return false;
    }

    if (!CreateDeviceResources()) {
        return false;
    }

    return true;
}

bool D2DRenderBackend::CreateDeviceResources()
{
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
            mendo::LogHrFailure(L"D3D11CreateDevice (WARP)", hr);
            return false;
        }
    }

    ComPtr<IDXGIDevice> dxgi_device;
    hr = d3d_device_.As(&dxgi_device);
    if (FAILED(hr)) {
        mendo::LogHrFailure(L"ID3D11Device::As<IDXGIDevice>", hr);
        return false;
    }

    hr = d2d_factory_->CreateDevice(dxgi_device.Get(), &d2d_device_);
    if (FAILED(hr)) {
        mendo::LogHrFailure(L"ID2D1Factory1::CreateDevice", hr);
        return false;
    }

    hr = d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &device_context_);
    if (FAILED(hr)) {
        mendo::LogHrFailure(L"ID2D1Device::CreateDeviceContext", hr);
        return false;
    }

    device_context_->SetDpi(dpi_, dpi_);

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(&adapter);
    if (FAILED(hr)) {
        mendo::LogHrFailure(L"IDXGIDevice::GetAdapter", hr);
        return false;
    }

    ComPtr<IDXGIFactory2> dxgi_factory;
    hr = adapter->GetParent(IID_PPV_ARGS(&dxgi_factory));
    if (FAILED(hr)) {
        mendo::LogHrFailure(L"IDXGIAdapter::GetParent<IDXGIFactory2>", hr);
        return false;
    }

    RECT rc{};
    if (!GetClientRect(hwnd_, &rc)) {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = rc.right - rc.left;
    scd.Height = rc.bottom - rc.top;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    // Waitable で GPU が次フレームのバッファを準備できるまで CPU を待たせる。
    // これがないと Present の Vsync ブロックで CPU が完全停止する。
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    hr = dxgi_factory->CreateSwapChainForHwnd(
        d3d_device_.Get(), hwnd_, &scd, nullptr, nullptr, &swap_chain_);
    if (FAILED(hr)) {
        // FLIP_DISCARD非対応の場合はFLIP_SEQUENTIALで再試行
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        hr = dxgi_factory->CreateSwapChainForHwnd(
            d3d_device_.Get(), hwnd_, &scd, nullptr, nullptr, &swap_chain_);
        if (FAILED(hr)) {
            mendo::LogHrFailure(L"CreateSwapChainForHwnd (FLIP_SEQUENTIAL)", hr);
            return false;
        }
    }

    ConfigureFrameLatency();

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
        mendo::LogHrFailure(L"IDXGISwapChain1::GetBuffer", hr);
        return false;
    }

    const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        dpi_, dpi_);

    ComPtr<ID2D1Bitmap1> target_bitmap;
    hr = device_context_->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &target_bitmap);
    if (FAILED(hr)) {
        mendo::LogHrFailure(L"CreateBitmapFromDxgiSurface", hr);
        return false;
    }

    device_context_->SetTarget(target_bitmap.Get());
    return true;
}

void D2DRenderBackend::Resize(UINT width, UINT height) noexcept
{
    if (!swap_chain_ || !device_context_) {
        return;
    }
    if (width == 0 || height == 0) {
        return;
    }

    // ResizeBuffers の前にターゲット参照を切る必要がある
    device_context_->SetTarget(nullptr);

    // SwapChain 生成時と同じ Flags を渡す必要がある（Waitable は ResizeBuffers でも維持）。
    const HRESULT hr = swap_chain_->ResizeBuffers(
        0, width, height, DXGI_FORMAT_UNKNOWN,
        DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    if (FAILED(hr)) {
        mendo::LogHrFailure(L"IDXGISwapChain1::ResizeBuffers", hr);
        device_lost_ = true;
        return;
    }
    if (!CreateSwapChainBitmap()) {
        // SetTarget(nullptr) のまま戻ると次回 BeginDraw で D2DERR_WRONG_STATE。
        device_lost_ = true;
    }
}

void D2DRenderBackend::SetDpi(float dpi) noexcept
{
    dpi_ = dpi;
    if (device_context_) {
        device_context_->SetDpi(dpi, dpi);
    }
}

void D2DRenderBackend::WaitForFrameLatency() noexcept
{
    if (!frame_latency_waitable_) {
        return;
    }
    // 1 秒タイムアウト。GPU が完全停止した状況でも UI スレッドを永久ブロックしない。
    (void)WaitForSingleObjectEx(frame_latency_waitable_.get(), 1000, TRUE);
}

HRESULT D2DRenderBackend::Present() noexcept
{
    if (!swap_chain_) {
        return E_FAIL;
    }
    const HRESULT hr = swap_chain_->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        device_lost_ = true;
    }
    return hr;
}

bool D2DRenderBackend::RecreateRenderTarget()
{
    frame_latency_waitable_.reset();
    device_context_.Reset();
    d2d_device_.Reset();
    swap_chain_.Reset();
    d3d_device_.Reset();

    const bool ok = CreateDeviceResources();
    if (ok) {
        device_lost_ = false;
    }
    return ok;
}
