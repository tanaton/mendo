#include "d2d_util.h"
#include "ui_constants.h"
#include <d2d1_3.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <bit>

using Microsoft::WRL::ComPtr;

namespace {

// COLR の層を 1 枚ずつ、層の色の単色で塗る。COLR の層が無ければ false。
bool DrawColorLayers(ID2D1RenderTarget* rt, D2D1_POINT_2F origin, DWRITE_MEASURING_MODE mode, const DWRITE_GLYPH_RUN& run,
                     const DWRITE_GLYPH_RUN_DESCRIPTION* desc, ID2D1Brush* brush, const DWRITE_MATRIX& world) noexcept
{
    static const ComPtr<IDWriteFactory2> factory = [] {
        ComPtr<IDWriteFactory2> created;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2), reinterpret_cast<IUnknown**>(created.GetAddressOf()));
        return created;
    }();
    ComPtr<IDWriteColorGlyphRunEnumerator> layers;
    if (!factory || FAILED(factory->TranslateColorGlyphRun(origin.x, origin.y, &run, desc, mode, &world, 0, &layers))) {
        return false;
    }
    ComPtr<ID2D1SolidColorBrush> color;
    BOOL has_run = FALSE;
    while (SUCCEEDED(layers->MoveNext(&has_run)) && has_run) {
        const DWRITE_COLOR_GLYPH_RUN* layer = nullptr;
        if (FAILED(layers->GetCurrentRun(&layer))) {
            break;
        }
        // 色の部分の無い字形 (paletteIndex 0xFFFF) は brush で描く。
        ID2D1Brush* fill = brush;
        if (layer->paletteIndex != 0xFFFF) {
            if (!color && FAILED(rt->CreateSolidColorBrush(layer->runColor, &color))) {
                break;
            }
            color->SetColor(layer->runColor);
            fill = color.Get();
        }
        rt->DrawGlyphRun(D2D1::Point2F(layer->baselineOriginX, layer->baselineOriginY), &layer->glyphRun, fill, mode);
    }
    return true;
}

// 1 つの brush で描くテキストレイアウトのレンダラ。
class SingleBrushTextRenderer final : public RenderTargetTextRenderer {
public:
    SingleBrushTextRenderer(ID2D1RenderTarget* rt, ID2D1Brush* brush) noexcept : RenderTargetTextRenderer(rt), brush_(brush)
    {}

    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode, const DWRITE_GLYPH_RUN* run,
                                           const DWRITE_GLYPH_RUN_DESCRIPTION* desc, IUnknown*) noexcept override
    {
        if (!DrawColorGlyphRun(x, y, mode, *run, desc, brush_)) {
            rt_->DrawGlyphRun(D2D1::Point2F(x, y), run, brush_, mode);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT x, FLOAT y, const DWRITE_UNDERLINE* u, IUnknown*) noexcept override
    {
        rt_->FillRectangle(D2D1::RectF(x, y + u->offset, x + u->width, y + u->offset + u->thickness), brush_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT x, FLOAT y, const DWRITE_STRIKETHROUGH* s, IUnknown*) noexcept override
    {
        rt_->FillRectangle(D2D1::RectF(x, y + s->offset, x + s->width, y + s->offset + s->thickness), brush_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void* context, FLOAT x, FLOAT y, IDWriteInlineObject* object, BOOL sideways, BOOL rtl,
                                               IUnknown* effect) noexcept override
    {
        return object ? object->Draw(context, this, x, y, sideways, rtl, effect) : S_OK;
    }

private:
    ID2D1Brush* brush_;
};

} // namespace

RenderTargetTextRenderer::RenderTargetTextRenderer(ID2D1RenderTarget* rt) noexcept : rt_(rt)
{
    rt->GetTransform(&transform_);
    float dpi_x = DEFAULT_DPI;
    float dpi_y = DEFAULT_DPI;
    rt->GetDpi(&dpi_x, &dpi_y);
    pixels_per_dip_ = DpiScaleFrom(dpi_x);
}

HRESULT STDMETHODCALLTYPE RenderTargetTextRenderer::QueryInterface(REFIID riid, void** out) noexcept
{
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) || riid == __uuidof(IDWriteTextRenderer)) {
        *out = this;
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE RenderTargetTextRenderer::IsPixelSnappingDisabled(void*, BOOL* disabled) noexcept
{
    *disabled = FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE RenderTargetTextRenderer::GetCurrentTransform(void*, DWRITE_MATRIX* transform) noexcept
{
    *transform = std::bit_cast<DWRITE_MATRIX>(transform_);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE RenderTargetTextRenderer::GetPixelsPerDip(void*, FLOAT* pixels_per_dip) noexcept
{
    *pixels_per_dip = pixels_per_dip_;
    return S_OK;
}

bool RenderTargetTextRenderer::DrawColorGlyphRun(float x, float y, DWRITE_MEASURING_MODE mode, const DWRITE_GLYPH_RUN& run,
                                                 const DWRITE_GLYPH_RUN_DESCRIPTION* desc, ID2D1Brush* brush) const noexcept
{
    ComPtr<IDWriteFontFace4> face;
    if (!run.fontFace || FAILED(run.fontFace->QueryInterface(IID_PPV_ARGS(&face))) || !face->IsColorFont()) {
        return false;
    }
    const D2D1::Matrix3x2F device = *D2D1::Matrix3x2F::ReinterpretBaseType(&transform_) * D2D1::Matrix3x2F::Scale(pixels_per_dip_, pixels_per_dip_);
    const auto world = std::bit_cast<DWRITE_MATRIX>(static_cast<D2D1_MATRIX_3X2_F>(device));
    const D2D1_POINT_2F origin = D2D1::Point2F(x, y);
    if (DrawColorLayers(rt_, origin, mode, run, desc, brush, world)) {
        return true;
    }
    // COLR のフォントで層の無い字形 (国旗の地域指示子など) は、D2D に任せても単色になるうえ 1 回 0.5ms ほどかかる。
    if (face->GetGlyphImageFormats() & DWRITE_GLYPH_IMAGE_FORMATS_COLR) {
        return false;
    }
    if (ComPtr<ID2D1DeviceContext7> dc7; SUCCEEDED(rt_->QueryInterface(IID_PPV_ARGS(&dc7)))) {
        dc7->DrawGlyphRunWithColorSupport(origin, &run, desc, brush, nullptr, 0, mode);
        return true;
    }
    return false;
}

void DrawTextLayoutWithColorLayers(ID2D1RenderTarget* rt, D2D1_POINT_2F origin, IDWriteTextLayout* layout, ID2D1Brush* brush) noexcept
{
    SingleBrushTextRenderer renderer{ rt, brush };
    layout->Draw(nullptr, &renderer, origin.x, origin.y);
}

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
        FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_)) || FAILED(BindSwapChainTarget(dc_.Get(), swap_chain_.Get(), dpi))) {
        Reset();
        return false;
    }
    return true;
}

bool WindowSwapTarget::Resize(UINT width, UINT height, float dpi)
{
    dc_->SetTarget(nullptr);
    if (FAILED(swap_chain_->ResizeBuffers(0, std::max(width, 1u), std::max(height, 1u), DXGI_FORMAT_UNKNOWN, 0)) || FAILED(BindSwapChainTarget(dc_.Get(), swap_chain_.Get(), dpi))) {
        Reset();
        return false;
    }
    return true;
}

HRESULT BindSwapChainTarget(ID2D1DeviceContext* dc, IDXGISwapChain1* swap_chain, float dpi) noexcept
{
    dc->SetDpi(dpi, dpi);
    ComPtr<IDXGISurface> surface;
    ComPtr<ID2D1Bitmap1> target;
    // 不透明な窓なので IGNORE にする。PREMULTIPLIED だと D2D が透過のある描画先とみなし、文字のアンチエイリアスを
    // ClearType からグレースケールに落とす。
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                               D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi);
    HRESULT hr = swap_chain->GetBuffer(0, IID_PPV_ARGS(&surface));
    if (SUCCEEDED(hr)) {
        hr = dc->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target);
    }
    if (SUCCEEDED(hr)) {
        dc->SetTarget(target.Get());
    }
    return hr;
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
