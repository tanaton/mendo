#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <variant>
#include <vector>
#include <algorithm>
#include <memory_resource>

// ---- 基本描画コマンド ----

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
    IDWriteTextLayout* layout; // 非所有; ライフタイムはLayoutCacheが管理。
    D2D1_COLOR_F color;
};

struct DrawTextCmd {
    const wchar_t* text = nullptr; // 非所有; ライフタイムはMonotonicResourceが管理。
    IDWriteTextFormat* format = nullptr; // 非所有; ライフタイムはRendererが管理。
    D2D1_RECT_F rect{};
    D2D1_COLOR_F color{};
    uint8_t text_len = 0;
};

struct DrawBitmapCmd {
    ID2D1Bitmap* bitmap; // 非所有; ライフタイムはLayoutCache::DiagramEntryが管理。
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

// ---- 状態コマンド ----

struct PushClipCmd {
    D2D1_RECT_F rect;
};

struct PopClipCmd {};

struct SetTransformCmd {
    D2D1_MATRIX_3X2_F transform;
};

// ---- バリアント + リスト ----

using DrawCommand = std::variant<
    ClearCmd, FillRectCmd, FillRoundedRectCmd,
    DrawLineCmd, DrawTextLayoutCmd, DrawTextCmd,
    DrawBitmapCmd, FillEllipseCmd, DrawEllipseCmd,
    PushClipCmd, PopClipCmd, SetTransformCmd
>;

using DrawCommandList = std::pmr::vector<DrawCommand>;
