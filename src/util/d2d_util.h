#pragma once
#include "ui_constants.h"
#include "ui_types.h"
#include "profiler.h"
#include "win32_util.h"
#include <d2d1.h>
#include <dwrite_3.h>
#include <wrl/client.h>
#include <windows.h>
#include <cstddef>

// テキスト書式の作り方。描画とコンテキストメニューで共有する。
struct TextFormatSpec {
    const wchar_t* family = nullptr;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    float size = 0.0f;
    // 漢字の字形とフォールバック先が決まる。文書から来る文字は Theme::text_locale、UI の決まった文言は i18n::S().lang_locale を渡す。
    const wchar_t* locale = L"en-us";
    DWRITE_TEXT_ALIGNMENT text_align = DWRITE_TEXT_ALIGNMENT_LEADING;
    DWRITE_PARAGRAPH_ALIGNMENT para_align = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
    bool no_wrap = false;
    // 0 より大きければ、主フォントの行の高さを size のこの倍にする。増減分は行の上下に半分ずつ配る。
    float line_height_factor = 0.0f;
    // 箱に収まらない行を省き、見えている最後の行の末尾を省略記号にする。箱の外には描かない。
    bool ellipsis = false;
};

// 比率指定の行間にして、フォールバックで高くなる行は既定と同じく高いまま、主フォントの行を line_height にする。
inline void SetLineHeight(IDWriteFactory* dw, IDWriteTextFormat* fmt, float line_height)
{
    Microsoft::WRL::ComPtr<IDWriteTextFormat3> fmt3;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> probe;
    if (FAILED(fmt->QueryInterface(IID_PPV_ARGS(&fmt3))) || FAILED(dw->CreateTextLayout(L"a", 1, fmt, 0.0f, 0.0f, &probe))) {
        return;
    }
    DWRITE_LINE_METRICS lm{};
    UINT32 count = 0;
    if (FAILED(probe->GetLineMetrics(&lm, 1, &count)) || count == 0 || lm.height <= 0.0f) {
        return;
    }
    const DWRITE_LINE_SPACING spacing{ DWRITE_LINE_SPACING_METHOD_PROPORTIONAL, line_height / lm.height, 1.0f, 0.5f, DWRITE_FONT_LINE_GAP_USAGE_DEFAULT };
    fmt3->SetLineSpacing(&spacing);
}

// 無効なフォントファミリ名は Segoe UI で作り直す (「フォントが効かない」状態を出さないための最終防壁)。
inline Microsoft::WRL::ComPtr<IDWriteTextFormat> CreateTextFormat(IDWriteFactory* dw, const TextFormatSpec& s)
{
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
    const auto create = [&](const wchar_t* family) {
        return dw->CreateTextFormat(family, nullptr, s.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, s.size, s.locale, fmt.ReleaseAndGetAddressOf());
    };
    if (const HRESULT hr = create(s.family); FAILED(hr)) {
        LogHrFailure("CreateTextFormat (falling back to Segoe UI)", hr);
        if (const HRESULT fallback = create(L"Segoe UI"); FAILED(fallback)) {
            LogHrFailure("CreateTextFormat (Segoe UI fallback)", fallback);
            return nullptr;
        }
    }
    if (s.no_wrap) {
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (s.text_align != DWRITE_TEXT_ALIGNMENT_LEADING) {
        fmt->SetTextAlignment(s.text_align);
    }
    if (s.line_height_factor > 0.0f) {
        SetLineHeight(dw, fmt.Get(), s.size * s.line_height_factor);
    }
    if (s.para_align != DWRITE_PARAGRAPH_ALIGNMENT_NEAR) {
        fmt->SetParagraphAlignment(s.para_align);
    }
    if (s.ellipsis) {
        Microsoft::WRL::ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(fmt.Get(), &sign))) {
            const DWRITE_TRIMMING trimming{ .granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER };
            fmt->SetTrimming(&trimming, sign.Get());
        }
    }
    return fmt;
}

inline D2D1_RECT_F ToD2DRect(const PaneRect& r) noexcept
{
    return D2D1::RectF(r.x, r.y, r.x + r.width, r.y + r.height);
}

inline D2D1_RECT_F OffsetRectF(const D2D1_RECT_F& r, float origin_x, float origin_y) noexcept
{
    return D2D1::RectF(origin_x + r.left, origin_y + r.top, origin_x + r.right, origin_y + r.bottom);
}

// CreateSolidColorBrush のフェイルセーフラッパ。失敗時は Magenta で再試行し、
// nullptr ブラシが DrawXXX に渡るのを防ぐ。重い失敗 (D2DERR_RECREATE_TARGET 等) は
// EndDraw 経路で検知されて次フレームの BeginFrame で作り直される想定。
// ReleaseAndGetAddressOf を使い、既存 brush を持つ ComPtr が渡されても assert/leak
// しないように防御する。
inline void CreateSolidColorBrushOrFallback(
    ID2D1RenderTarget* rt,
    D2D1_COLOR_F color,
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>& out_brush) noexcept
{
    if (SUCCEEDED(rt->CreateSolidColorBrush(color, out_brush.ReleaseAndGetAddressOf()))) {
        return;
    }
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Magenta), out_brush.ReleaseAndGetAddressOf()))) {
        MENDO_TRACE("CreateSolidColorBrush failed even on magenta fallback");
    }
}

// ビットマップの最大辺。描画ターゲットを作る前は D3D11 (FL11) のテクスチャ最大辺を使う。
inline UINT MaxBitmapDim(ID2D1RenderTarget* rt) noexcept
{
    constexpr UINT kWithoutTarget = 16384;
    return rt ? rt->GetMaximumBitmapSize() : kWithoutTarget;
}

// GPU メモリ見積もり。キャッシュのバイト予算に使う。A8 (インライン数式の型) のほかは PBGRA。
inline size_t BitmapBytes(ID2D1Bitmap* bitmap) noexcept
{
    if (!bitmap) {
        return 0;
    }
    const auto size = bitmap->GetPixelSize();
    const size_t bytes_per_pixel = bitmap->GetPixelFormat().format == DXGI_FORMAT_A8_UNORM ? 1 : 4;
    return static_cast<size_t>(size.width) * size.height * bytes_per_pixel;
}

// null ブラシと不透明は no-op。
class OpacityScope {
public:
    OpacityScope(ID2D1SolidColorBrush* brush, float alpha) noexcept : brush_(alpha != 1.0f ? brush : nullptr)
    {
        if (brush_) {
            brush_->SetOpacity(alpha);
        }
    }
    ~OpacityScope() noexcept
    {
        if (brush_) {
            brush_->SetOpacity(1.0f);
        }
    }
    OpacityScope(const OpacityScope&) = delete;
    OpacityScope& operator=(const OpacityScope&) = delete;
    OpacityScope(OpacityScope&&) = delete;
    OpacityScope& operator=(OpacityScope&&) = delete;

private:
    ID2D1SolidColorBrush* brush_;
};
