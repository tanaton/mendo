#pragma once

// DIP 単位の矩形。`<d2d1.h>` を巻き込まずに公開 API で矩形を扱うための型。
// 描画レイヤでは ToD2DRect() で D2D1_RECT_F に変換する
// （render_params.h で layout 互換を static_assert で担保）。
struct DipRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

// 点が矩形内にあるか判定する（D2D 規約に合わせ右辺・下辺は排他的）。
// `left/top/right/bottom` を持つ任意の矩形型 (DipRect / D2D1_RECT_F 等) に対応する。
template <class Rect>
inline constexpr bool PointInRect(float x, float y, const Rect& r) noexcept
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}
