#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <windows.h>

using Microsoft::WRL::ComPtr;

// Abstract interface for render backend (D2D factories, render target, DPI).
// Brushes and text formats remain in Renderer (abstraction cost too high).
class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    virtual bool Init(HWND hwnd) = 0;
    virtual void Resize(UINT width, UINT height) = 0;
    virtual void SetDpi(float dpi) = 0;
    virtual float GetDpi() const = 0;
    virtual bool RecreateRenderTarget() = 0;

    virtual ID2D1Factory* GetD2DFactory() const = 0;
    virtual ID2D1HwndRenderTarget* GetRenderTarget() const = 0;
    virtual IDWriteFactory* GetDWriteFactory() const = 0;
    virtual HWND GetHwnd() const = 0;
};
