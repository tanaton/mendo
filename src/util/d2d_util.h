#pragma once
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

// ダーク/ライトテーマでの「白オーバーレイ vs 黒オーバーレイ」を 1 行で表現する。
// テーブル縞模様、スクロールバーつまみ、トーストの背景など複数箇所で同じ判定が必要。
inline D2D1_COLOR_F MonochromeOverlay(bool is_dark, float alpha) noexcept
{
    return is_dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha)
                   : D2D1::ColorF(0.0f, 0.0f, 0.0f, alpha);
}

// SetOpacity(alpha) と SetOpacity(1.0f) リセットのペアを RAII で表現する。
// 共有ブラシに対する透明度の復帰漏れを防ぐ。null ブラシは no-op。
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

// DIP 矩形をピクセル境界に丸めて InvalidateRect する。 right/bottom は +1 で
// オーバースキャンし境界線が抜けないようにする (RECT は exclusive 仕様)。
inline void InvalidateDipRect(HWND hwnd, float dip_x, float dip_y, float dip_w, float dip_h, float dpi_scale) noexcept
{
    RECT rc;
    rc.left = static_cast<LONG>(dip_x * dpi_scale);
    rc.top = static_cast<LONG>(dip_y * dpi_scale);
    rc.right = static_cast<LONG>((dip_x + dip_w) * dpi_scale) + 1;
    rc.bottom = static_cast<LONG>((dip_y + dip_h) * dpi_scale) + 1;
    InvalidateRect(hwnd, &rc, FALSE);
}

} // namespace mendo
