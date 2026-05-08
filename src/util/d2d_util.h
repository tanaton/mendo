#pragma once
#include <d2d1.h>
#include <wrl/client.h>
#include <windows.h>

namespace mendo {

// CreateSolidColorBrush のフェイルセーフラッパ。失敗時は Magenta で再試行し、
// nullptr ブラシが DrawXXX に渡るのを防ぐ。重い失敗 (D2DERR_RECREATE_TARGET 等) は
// EndDraw 経路で検知されて次フレームの HandleDeviceLost で復旧される想定。
inline void CreateSolidColorBrushOrFallback(
    ID2D1RenderTarget* rt,
    D2D1_COLOR_F color,
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>& out_brush) noexcept
{
    if (SUCCEEDED(rt->CreateSolidColorBrush(color, &out_brush))) {
        return;
    }
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Magenta), &out_brush))) {
        OutputDebugStringW(L"[mendo] CreateSolidColorBrush failed even on magenta fallback\n");
    }
}

} // namespace mendo
