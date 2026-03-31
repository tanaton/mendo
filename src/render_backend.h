#pragma once
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <windows.h>

// レンダーバックエンドの抽象インターフェース（D2Dファクトリ、レンダーターゲット、DPI）。
// ブラシとテキストフォーマットは抽象化コストが高すぎるためRendererに残す。
class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    virtual bool Init(HWND hwnd) = 0;
    virtual void Resize(UINT width, UINT height) = 0;
    virtual void SetDpi(float dpi) = 0;
    virtual float GetDpi() const noexcept = 0;
    virtual bool RecreateRenderTarget() = 0;

    virtual ID2D1Factory* GetD2DFactory() const noexcept = 0;
    virtual ID2D1DeviceContext* GetRenderTarget() const noexcept = 0;
    virtual IDWriteFactory* GetDWriteFactory() const noexcept = 0;
    virtual IWICImagingFactory* GetWICFactory() const noexcept = 0;
    virtual HWND GetHwnd() const noexcept = 0;

    // スワップチェーンのPresent。EndDraw後に呼び出す。
    virtual void Present() = 0;
};
