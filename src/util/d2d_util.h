#pragma once
#include "ui_constants.h"
#include "ui_types.h"
#include "profiler.h"
#include "win32_util.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <windows.h>
#include <bit>
#include <cstddef>

// DipRect は D2D1_RECT_F とメンバ順・サイズ・アライメントが同一であることを
// 静的に保証し、std::bit_cast で安全にビット等価変換する。
static_assert(sizeof(DipRect) == sizeof(D2D1_RECT_F));
static_assert(alignof(DipRect) == alignof(D2D1_RECT_F));
static_assert(offsetof(DipRect, left) == offsetof(D2D1_RECT_F, left));
static_assert(offsetof(DipRect, top) == offsetof(D2D1_RECT_F, top));
static_assert(offsetof(DipRect, right) == offsetof(D2D1_RECT_F, right));
static_assert(offsetof(DipRect, bottom) == offsetof(D2D1_RECT_F, bottom));

inline D2D1_RECT_F ToD2DRect(const DipRect& r) noexcept
{
    return std::bit_cast<D2D1_RECT_F>(r);
}

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
};

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
    if (s.para_align != DWRITE_PARAGRAPH_ALIGNMENT_NEAR) {
        fmt->SetParagraphAlignment(s.para_align);
    }
    return fmt;
}

// PaneRect は {x,y,width,height} で D2D1_RECT_F とビット等価ではないため計算で変換する。
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

// PBGRA 前提の GPU メモリ見積もり。キャッシュのバイト予算に使う。
inline size_t BitmapBytes(ID2D1Bitmap* bitmap) noexcept
{
    if (!bitmap) {
        return 0;
    }
    const auto size = bitmap->GetPixelSize();
    return static_cast<size_t>(size.width) * size.height * 4;
}

inline D2D1_COLOR_F MonochromeOverlay(bool is_dark, float alpha) noexcept
{
    return is_dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha)
                   : D2D1::ColorF(0.0f, 0.0f, 0.0f, alpha);
}

// Renderer の brushes_[TableStripe] と CommandGenerator のストライプ色で共有する。
inline D2D1_COLOR_F TableStripeColor(bool is_dark) noexcept
{
    return MonochromeOverlay(is_dark, is_dark ? TABLE_STRIPE_ALPHA_DARK : TABLE_STRIPE_ALPHA_LIGHT);
}

// null ブラシは no-op。
class OpacityScope {
public:
    OpacityScope(ID2D1SolidColorBrush* brush, float alpha) noexcept : brush_(brush)
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
