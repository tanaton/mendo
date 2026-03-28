#include "d2d_render_backend.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

bool D2DRenderBackend::Init(HWND hwnd)
{
    hwnd_ = hwnd;

    // このウィンドウのモニターの実際のDPIを取得
    dpi_ = static_cast<float>(GetDpiForWindow(hwnd));
    if (dpi_ == 0.0f) {
        dpi_ = 96.0f;
    }

    // D2Dファクトリを作成
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_factory_.GetAddressOf());
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
    // 正しい初期DPIでレンダーターゲットを作成
    RECT rc;
    GetClientRect(hwnd, &rc);

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties();
    rtProps.dpiX = dpi_;
    rtProps.dpiY = dpi_;

    D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(
        hwnd, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top));
    hwndProps.presentOptions = D2D1_PRESENT_OPTIONS_NONE;

    hr = d2d_factory_->CreateHwndRenderTarget(rtProps, hwndProps, &render_target_);
    if (FAILED(hr)) {
        return false;
    }
    return true;
}

void D2DRenderBackend::Resize(UINT width, UINT height)
{
    if (render_target_) {
        render_target_->Resize(D2D1::SizeU(width, height));
    }
}

void D2DRenderBackend::SetDpi(float dpi)
{
    dpi_ = dpi;
    if (render_target_) {
        render_target_->SetDpi(dpi, dpi);
    }
}

bool D2DRenderBackend::RecreateRenderTarget()
{
    render_target_.Reset();

    RECT rc;
    GetClientRect(hwnd_, &rc);

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties();
    rtProps.dpiX = dpi_;
    rtProps.dpiY = dpi_;

    D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(
        hwnd_, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top));
    hwndProps.presentOptions = D2D1_PRESENT_OPTIONS_NONE;

    HRESULT hr = d2d_factory_->CreateHwndRenderTarget(rtProps, hwndProps, &render_target_);
    return SUCCEEDED(hr);
}
