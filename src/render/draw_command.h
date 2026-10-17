#pragma once
#include "brush_id.h"
#include <d2d1.h>
#include <dwrite.h>
#include <variant>
#include <vector>
#include <memory_resource>
#include <cstdint>

struct FillRectCmd {
    D2D1_RECT_F rect;
    D2D1_COLOR_F color;
    BrushId brush_id = BrushId::Custom;
};

struct FillRoundedRectCmd {
    D2D1_RECT_F rect;
    float rx, ry;
    D2D1_COLOR_F color;
    BrushId brush_id = BrushId::Custom;
};

struct DrawLineCmd {
    D2D1_POINT_2F p0, p1;
    D2D1_COLOR_F color;
    float stroke_width;
    BrushId brush_id = BrushId::Custom;
};

struct DrawTextLayoutCmd {
    D2D1_POINT_2F origin;
    IDWriteTextLayout* layout; // 非所有; ライフタイムは LayoutCache か TextLayoutCache が管理。
    D2D1_COLOR_F color;
    BrushId brush_id = BrushId::Custom;
    // 可視範囲より高いレイアウトは [cull_top, cull_bottom] (origin と同じ座標系) 外のランを飛ばす。
    bool cull_runs = false;
    float cull_top = 0.0f;
    float cull_bottom = 0.0f;
    float opacity = 1.0f;
};

struct DrawBitmapCmd {
    ID2D1Bitmap* bitmap; // 非所有; ライフタイムは LayoutCache::DiagramEntry か Renderer が管理。
    D2D1_RECT_F dest;
    float opacity = 1.0f;
};

struct FillEllipseCmd {
    D2D1_POINT_2F center;
    float rx, ry;
    D2D1_COLOR_F color;
    BrushId brush_id = BrushId::Custom;
};

struct DrawEllipseCmd {
    D2D1_POINT_2F center;
    float rx, ry;
    D2D1_COLOR_F color;
    float stroke_width;
    BrushId brush_id = BrushId::Custom;
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
