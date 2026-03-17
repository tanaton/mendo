#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <variant>
#include <vector>
#include <string>

// ---- Primitive draw commands ----

struct ClearCmd {
    D2D1_COLOR_F color;
};

struct FillRectCmd {
    D2D1_RECT_F rect;
    D2D1_COLOR_F color;
};

struct FillRoundedRectCmd {
    D2D1_RECT_F rect;
    float rx, ry;
    D2D1_COLOR_F color;
};

struct DrawLineCmd {
    D2D1_POINT_2F p0, p1;
    D2D1_COLOR_F color;
    float stroke_width;
};

struct DrawTextLayoutCmd {
    D2D1_POINT_2F origin;
    IDWriteTextLayout* layout; // Non-owning; lifetime managed by LayoutCache.
    D2D1_COLOR_F color;
};

struct DrawTextCmd {
    std::wstring text;
    D2D1_RECT_F rect;
    IDWriteTextFormat* format; // Non-owning; lifetime managed by Renderer.
    D2D1_COLOR_F color;
};

struct DrawBitmapCmd {
    ID2D1Bitmap* bitmap; // Non-owning; lifetime managed by LayoutCache::DiagramEntry.
    D2D1_RECT_F dest;
    float opacity = 1.0f;
};

struct FillEllipseCmd {
    D2D1_POINT_2F center;
    float rx, ry;
    D2D1_COLOR_F color;
};

struct DrawEllipseCmd {
    D2D1_POINT_2F center;
    float rx, ry;
    D2D1_COLOR_F color;
    float stroke_width;
};

// ---- State commands ----

struct PushClipCmd {
    D2D1_RECT_F rect;
};

struct PopClipCmd {};

struct SetTransformCmd {
    D2D1_MATRIX_3X2_F transform;
};

// ---- Variant + list ----

using DrawCommand = std::variant<
    ClearCmd, FillRectCmd, FillRoundedRectCmd,
    DrawLineCmd, DrawTextLayoutCmd, DrawTextCmd,
    DrawBitmapCmd, FillEllipseCmd, DrawEllipseCmd,
    PushClipCmd, PopClipCmd, SetTransformCmd
>;

using DrawCommandList = std::vector<DrawCommand>;
