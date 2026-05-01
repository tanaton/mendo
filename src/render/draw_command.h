#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <variant>
#include <vector>
#include <algorithm>
#include <memory_resource>

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
    // 1〜INLINE_TEXT_CAPACITY 文字は inline_buf に直接格納し、
    // monotonic resource からの小さな allocate を回避する（icon 1 文字や "1." 等の頻出ケース）。
    // pointer と同サイズに収めることで variant 最大サイズに影響しない。
    static constexpr uint8_t INLINE_TEXT_CAPACITY = 4;
    union {
        const wchar_t* text_ptr; // 非所有; ライフタイムはMonotonicResourceが管理。
        wchar_t inline_buf[INLINE_TEXT_CAPACITY];
    };
    IDWriteTextFormat* format = nullptr; // 非所有; ライフタイムはRendererが管理。
    D2D1_RECT_F rect{};
    D2D1_COLOR_F color{};
    uint8_t text_len = 0;
    bool is_inline = false;

    DrawTextCmd() noexcept : text_ptr(nullptr)
    {}

    const wchar_t* text() const noexcept
    {
        return is_inline ? inline_buf : text_ptr;
    }
};

struct DrawBitmapCmd {
    ID2D1Bitmap* bitmap; // 非所有; ライフタイムはLayoutCache::DiagramEntryが管理。
    D2D1_RECT_F dest;
    float opacity = 1.0f;
    D2D1_BITMAP_INTERPOLATION_MODE interpolation_mode = D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
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

struct PushClipCmd {
    D2D1_RECT_F rect;
};

struct PopClipCmd {};

struct SetTransformCmd {
    D2D1_MATRIX_3X2_F transform;
};

using DrawCommand = std::variant<
    ClearCmd,
    FillRectCmd,
    FillRoundedRectCmd,
    DrawLineCmd,
    DrawTextLayoutCmd,
    DrawTextCmd,
    DrawBitmapCmd,
    FillEllipseCmd,
    DrawEllipseCmd,
    PushClipCmd,
    PopClipCmd,
    SetTransformCmd>;

using DrawCommandList = std::pmr::vector<DrawCommand>;
