#pragma once

// include 連鎖を浅く保つため 1 か所に集約。

// <d2d1.h> を巻き込まずに矩形を扱う。
struct DipRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

// D2D 規約に合わせ右辺・下辺は排他的。
template <class Rect>
inline constexpr bool PointInRect(float x, float y, const Rect& r) noexcept
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// 右辺・下辺を含む inclusive 版。
template <class Rect>
inline constexpr bool PointInRectInclusive(float x, float y, const Rect& r) noexcept
{
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

struct PaneRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct ScrollState {
    float scroll_y = 0.0f;
    float max_scroll = 0.0f;
};

inline constexpr float PANE_SCROLLBAR_WIDTH = 8.0f;
inline constexpr float PANE_SCROLLBAR_THUMB_MIN = 24.0f;
inline constexpr float PANE_SCROLLBAR_MARGIN = 2.0f;
inline constexpr float PANE_SCROLLBAR_HIT_PADDING = 4.0f;

// 各フィールドは App の複数箇所から直接読み書きされるため public にしている。
struct ScrollRestoration {
    int pending_restore_node = -1;
    int pending_restore_offset = 0;

    constexpr bool HasNodeRestore() const noexcept
    {
        return pending_restore_node >= 0;
    }

    constexpr void SetNodeRestore(int node, int offset) noexcept
    {
        pending_restore_node = node;
        pending_restore_offset = offset;
    }

    constexpr void ClearNodeRestore() noexcept
    {
        pending_restore_node = -1;
        pending_restore_offset = 0;
    }
};

// -1 は「該当なし」。
struct HoveredButtons {
    int copy = -1;
    int save = -1;
    int svg_copy = -1;

    bool operator==(const HoveredButtons&) const = default;
};

enum class NavButtonHover : uint8_t {
    None,
    Back,
    Forward
};
