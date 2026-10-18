#pragma once
#include "brush_id.h"
#include "syntax.h"
#include <d2d1.h>
#include <dwrite.h>
#include <limits>
#include <variant>
#include <vector>
#include <memory_resource>
#include <span>
#include <cstdint>

// 色はすべて固定ブラシ (BrushId) で指定する。半透明は opacity で掛ける。
struct FillRectCmd {
    D2D1_RECT_F rect;
    BrushId brush_id;
};

struct FillRoundedRectCmd {
    D2D1_RECT_F rect;
    float rx, ry;
    BrushId brush_id;
    float opacity = 1.0f;
};

struct DrawLineCmd {
    D2D1_POINT_2F p0, p1;
    float stroke_width;
    BrushId brush_id;
};

struct DrawTextLayoutCmd {
    D2D1_POINT_2F origin;
    IDWriteTextLayout* layout; // 非所有; ライフタイムは LayoutCache か TextLayoutCache が管理。
    BrushId brush_id;
    // 可視範囲より高いレイアウトは [cull_top, cull_bottom] (origin と同じ座標系) 外のランを飛ばす。
    // 低いレイアウトでは範囲は既定の無限のままにする。
    float cull_top = -std::numeric_limits<float>::infinity();
    float cull_bottom = std::numeric_limits<float>::infinity();
    float opacity = 1.0f;
    // 非所有; layout と同じ NodeLayoutEntry が持つ。UTF-16 位置のトークンで、描画時にグリフランを色分けする。
    std::span<const SyntaxToken> syntax_tokens;
};

struct DrawBitmapCmd {
    ID2D1Bitmap* bitmap; // 非所有; ライフタイムは LayoutCache::DiagramEntry か Renderer が管理。
    D2D1_RECT_F dest;
    float opacity = 1.0f;
};

struct FillEllipseCmd {
    D2D1_POINT_2F center;
    float rx, ry;
    BrushId brush_id;
};

struct DrawEllipseCmd {
    D2D1_POINT_2F center;
    float rx, ry;
    float stroke_width;
    BrushId brush_id;
};

struct PushClipCmd {
    D2D1_RECT_F rect;
};

struct PopClipCmd {};

struct SetTransformCmd {
    D2D1_MATRIX_3X2_F transform;
};

using DrawCommand = std::variant<
    FillRectCmd,
    FillRoundedRectCmd,
    DrawLineCmd,
    DrawTextLayoutCmd,
    DrawBitmapCmd,
    FillEllipseCmd,
    DrawEllipseCmd,
    PushClipCmd,
    PopClipCmd,
    SetTransformCmd>;

using DrawCommandList = std::pmr::vector<DrawCommand>;
