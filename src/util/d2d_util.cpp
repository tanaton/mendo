#include "d2d_util.h"
#include "ui_constants.h"
#include <d2d1_3.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <array>
#include <bit>
#include <functional>
#include <memory>
#include <span>

using Microsoft::WRL::ComPtr;

namespace {

// COLR の層を順に受け取って塗る。同じ色と原点で続く 1 字形の層は、送り幅 0 の 1 つのランにためてまとめて描く。
// 絵文字の一覧 (example/emoji.md) では隣り合う層の 31% が直前と同じ色で、DrawGlyphRun が 31% 減り、BM_EmojiFrameDraw が
// 928〜937 → 863〜867µs になる (新旧を交互に 3 回)。手間の正体は D2D が色の変わるたびに描画のまとまりを切ることで
// (全層を 1 色で描くと −290µs)、ブラシの使い回しや (書体, 字形) ごとの層の並びの記憶は効かなかった。
// 画素は、同じ色の層の縁が重なるところのアンチエイリアスが少し薄くなるだけ (250% で最大 62 階調・824 画素、100% で最大
// 11 階調・16 画素、500% は同じ)。右から左のランは送り幅 0 だと字形がずれ、縦書きは使わないので、どちらもまとめない。
class LayerPainter {
public:
    LayerPainter(ID2D1RenderTarget* rt, ID2D1Brush* brush, DWRITE_MEASURING_MODE mode) noexcept : rt_(rt), brush_(brush), mode_(mode)
    {}

    void Add(const DWRITE_COLOR_GLYPH_RUN& layer) noexcept
    {
        const DWRITE_GLYPH_RUN& run = layer.glyphRun;
        const D2D1_POINT_2F origin = D2D1::Point2F(layer.baselineOriginX, layer.baselineOriginY);
        const bool foreground = layer.paletteIndex == 0xFFFF;
        const bool single = run.glyphCount == 1 && !run.isSideways && (run.bidiLevel & 1) == 0;
        if (count_ > 0 && !(single && Continues(origin, run, foreground, layer.runColor))) {
            Flush();
        }
        if (!single) {
            Paint(origin, run, foreground, layer.runColor);
            return;
        }
        if (count_ == 0) {
            origin_ = origin;
            face_ = run.fontFace;
            size_ = run.fontEmSize;
            foreground_ = foreground;
            color_ = layer.runColor;
        }
        glyphs_[count_] = run.glyphIndices[0];
        offsets_[count_] = run.glyphOffsets ? run.glyphOffsets[0] : DWRITE_GLYPH_OFFSET{};
        ++count_;
    }

    void Flush() noexcept
    {
        if (count_ == 0) {
            return;
        }
        const DWRITE_GLYPH_RUN run{
            .fontFace = face_,
            .fontEmSize = size_,
            .glyphCount = count_,
            .glyphIndices = glyphs_.data(),
            .glyphAdvances = kZeroAdvances.data(),
            .glyphOffsets = offsets_.data(),
        };
        Paint(origin_, run, foreground_, color_);
        count_ = 0;
    }

private:
    static constexpr UINT32 kCapacity = 64;
    static constexpr std::array<FLOAT, kCapacity> kZeroAdvances{};

    // ためた層に続けて同じランで描けるか。
    bool Continues(D2D1_POINT_2F origin, const DWRITE_GLYPH_RUN& run, bool foreground, const D2D1_COLOR_F& c) const noexcept
    {
        return count_ < kCapacity && origin.x == origin_.x && origin.y == origin_.y && run.fontFace == face_ && run.fontEmSize == size_ &&
               foreground == foreground_ && (foreground || (c.r == color_.r && c.g == color_.g && c.b == color_.b && c.a == color_.a));
    }

    // 色の部分の無い字形 (paletteIndex 0xFFFF) と、色のブラシを作れないときは brush_ で描く。
    // 色のブラシは brush_ の不透明度 (非アクティブの窓のタイトルなど) に合わせる (DrawMath の色の字形と同じ)。
    void Paint(D2D1_POINT_2F origin, const DWRITE_GLYPH_RUN& run, bool foreground, const D2D1_COLOR_F& color) noexcept
    {
        ID2D1Brush* fill = brush_;
        if (!foreground &&
            (color_brush_ || SUCCEEDED(rt_->CreateSolidColorBrush(color, D2D1::BrushProperties(brush_->GetOpacity()), &color_brush_)))) {
            color_brush_->SetColor(color);
            fill = color_brush_.Get();
        }
        rt_->DrawGlyphRun(origin, &run, fill, mode_);
    }

    ID2D1RenderTarget* rt_;
    ID2D1Brush* brush_;
    DWRITE_MEASURING_MODE mode_;
    ComPtr<ID2D1SolidColorBrush> color_brush_;
    D2D1_POINT_2F origin_{};
    IDWriteFontFace* face_ = nullptr;
    float size_ = 0.0f;
    bool foreground_ = false;
    D2D1_COLOR_F color_{};
    UINT32 count_ = 0;
    std::array<UINT16, kCapacity> glyphs_{};
    std::array<DWRITE_GLYPH_OFFSET, kCapacity> offsets_{};
};

// COLR の層を塗る。層の無い字形なら何も描かずに false。
bool DrawColorLayers(ID2D1RenderTarget* rt, D2D1_POINT_2F origin, DWRITE_MEASURING_MODE mode, const DWRITE_GLYPH_RUN& run,
                     const DWRITE_GLYPH_RUN_DESCRIPTION* desc, ID2D1Brush* brush) noexcept
{
    static const ComPtr<IDWriteFactory2> factory = [] {
        ComPtr<IDWriteFactory2> created;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2), reinterpret_cast<IUnknown**>(created.GetAddressOf()));
        return created;
    }();
    // 字形の座標から装置の画素への変換。
    D2D1::Matrix3x2F transform;
    rt->GetTransform(&transform);
    float dpi_x = DEFAULT_DPI;
    float dpi_y = DEFAULT_DPI;
    rt->GetDpi(&dpi_x, &dpi_y);
    const auto world =
        std::bit_cast<DWRITE_MATRIX>(static_cast<D2D1_MATRIX_3X2_F>(transform * D2D1::Matrix3x2F::Scale(DpiScaleFrom(dpi_x), DpiScaleFrom(dpi_y))));
    ComPtr<IDWriteColorGlyphRunEnumerator> layers;
    if (!factory || FAILED(factory->TranslateColorGlyphRun(origin.x, origin.y, &run, desc, mode, &world, 0, &layers))) {
        return false;
    }
    LayerPainter painter(rt, brush, mode);
    BOOL has_run = FALSE;
    const DWRITE_COLOR_GLYPH_RUN* layer = nullptr;
    while (SUCCEEDED(layers->MoveNext(&has_run)) && has_run && SUCCEEDED(layers->GetCurrentRun(&layer))) {
        painter.Add(*layer);
    }
    painter.Flush();
    return true;
}

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

bool IsColorFont(IDWriteFontFace* face) noexcept
{
    ComPtr<IDWriteFontFace2> face2;
    return face && SUCCEEDED(face->QueryInterface(IID_PPV_ARGS(&face2))) && face2->IsColorFont();
}

bool DrawColorGlyphRun(ID2D1RenderTarget* rt, D2D1_POINT_2F origin, DWRITE_MEASURING_MODE mode, const DWRITE_GLYPH_RUN& run,
                       const DWRITE_GLYPH_RUN_DESCRIPTION* desc, ID2D1Brush* brush) noexcept
{
    if (DrawColorLayers(rt, origin, mode, run, desc, brush)) {
        return true;
    }
    // COLR のフォントで層の無い字形 (国旗の地域指示子など) は、D2D に任せても単色になるうえ遅い。
    if (ComPtr<IDWriteFontFace4> face; FAILED(run.fontFace->QueryInterface(IID_PPV_ARGS(&face))) ||
                                       (face->GetGlyphImageFormats() & DWRITE_GLYPH_IMAGE_FORMATS_COLR)) {
        return false;
    }
    if (ComPtr<ID2D1DeviceContext7> dc7; SUCCEEDED(rt->QueryInterface(IID_PPV_ARGS(&dc7)))) {
        dc7->DrawGlyphRunWithColorSupport(origin, &run, desc, brush, nullptr, 0, mode);
        return true;
    }
    return false;
}

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
    return IsColorFont(run.fontFace) && ::DrawColorGlyphRun(rt_, D2D1::Point2F(x, y), mode, run, desc, brush);
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

void GlyphRunMerger::Add(float x, float y, const DWRITE_GLYPH_RUN& run, ID2D1Brush* brush, DWRITE_MEASURING_MODE mode)
{
    if (run.glyphCount == 0) {
        return;
    }
    const auto used = std::span{ groups_ }.first(used_);
    const auto it = std::ranges::find_if(used, [&](const Group& g) {
        return g.face == run.fontFace && g.size == run.fontEmSize && g.brush == brush && g.mode == mode;
    });
    Group* g = it != used.end() ? std::to_address(it) : nullptr;
    if (!g) {
        if (used_ == groups_.size()) {
            groups_.emplace_back();
        }
        g = &groups_[used_++];
        g->face = run.fontFace;
        g->size = run.fontEmSize;
        g->brush = brush;
        g->mode = mode;
        g->origin = D2D1::Point2F(x, y);
        g->glyphs.clear();
        g->advances.clear();
        g->offsets.clear();
    }
    // 前のランの最後の字形の送りで、ペンをこのランの頭まで進める (行の頭へ戻るときは負になる)。
    if (!g->advances.empty()) {
        g->advances.back() += x - g->pen;
    }
    const std::span advances{ run.glyphAdvances, run.glyphCount };
    g->glyphs.append_range(std::span{ run.glyphIndices, run.glyphCount });
    g->advances.append_range(advances);
    const float rise = g->origin.y - y;
    if (run.glyphOffsets) {
        for (const DWRITE_GLYPH_OFFSET& o : std::span{ run.glyphOffsets, run.glyphCount }) {
            g->offsets.push_back({ .advanceOffset = o.advanceOffset, .ascenderOffset = o.ascenderOffset + rise });
        }
    }
    else {
        g->offsets.insert(g->offsets.end(), run.glyphCount, { .advanceOffset = 0.0f, .ascenderOffset = rise });
    }
    g->pen = std::ranges::fold_left(advances, x, std::plus{});
}

void GlyphRunMerger::Flush(ID2D1RenderTarget* rt)
{
    for (const Group& g : std::span{ groups_ }.first(used_)) {
        const DWRITE_GLYPH_RUN run{
            .fontFace = g.face,
            .fontEmSize = g.size,
            .glyphCount = static_cast<UINT32>(g.glyphs.size()),
            .glyphIndices = g.glyphs.data(),
            .glyphAdvances = g.advances.data(),
            .glyphOffsets = g.offsets.data(),
        };
        rt->DrawGlyphRun(g.origin, &run, g.brush, g.mode);
    }
    used_ = 0;
}
