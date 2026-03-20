#pragma once
#include "render_backend.h"

class D2DRenderBackend final : public IRenderBackend {
public:
    bool Init(HWND hwnd) override;
    void Resize(UINT width, UINT height) override;
    void SetDpi(float dpi) override;
    float GetDpi() const noexcept override { return dpi_; }
    bool RecreateRenderTarget() override;

    ID2D1Factory* GetD2DFactory() const noexcept override { return d2d_factory_.Get(); }
    ID2D1HwndRenderTarget* GetRenderTarget() const noexcept override { return render_target_.Get(); }
    IDWriteFactory* GetDWriteFactory() const noexcept override { return dwrite_factory_.Get(); }
    HWND GetHwnd() const noexcept override { return hwnd_; }

private:
    HWND hwnd_ = nullptr;
    float dpi_ = 96.0f;
    ComPtr<ID2D1Factory> d2d_factory_;
    ComPtr<ID2D1HwndRenderTarget> render_target_;
    ComPtr<IDWriteFactory> dwrite_factory_;
};
