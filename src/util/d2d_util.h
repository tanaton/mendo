#pragma once
#include "ui_constants.h"
#include <d2d1.h>
#include <wrl/client.h>
#include <windows.h>

namespace mendo {

// CreateSolidColorBrush のフェイルセーフラッパ。失敗時は Magenta で再試行し、
// nullptr ブラシが DrawXXX に渡るのを防ぐ。重い失敗 (D2DERR_RECREATE_TARGET 等) は
// EndDraw 経路で検知されて次フレームの HandleDeviceLost で復旧される想定。
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
        OutputDebugStringW(L"[mendo] CreateSolidColorBrush failed even on magenta fallback\n");
    }
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

} // namespace mendo
