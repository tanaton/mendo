#pragma once
#include <windows.h>
#include <limits>

inline constexpr int HOVER_THROTTLE_DISTANCE_SQ = 16;
inline constexpr DWORD HOVER_THROTTLE_MIN_INTERVAL_MS = 8;

// last_pos の "未設定" sentinel。
// (px - LONG の最小値) は signed int オーバーフロー (UB) になるため、
// この値の間は距離計算を行わず即座に "移動した" として扱う。
inline constexpr POINT kUnsetHoverPos{
    std::numeric_limits<LONG>::min(), std::numeric_limits<LONG>::min()
};

inline constexpr bool IsUnset(POINT p) noexcept
{
    return p.x == std::numeric_limits<LONG>::min();
}

struct HoverThrottle {
    POINT last_md_hit_pos = kUnsetHoverPos;
    bool last_md_cursor_hand = false;
    POINT last_copy_hit_pos = kUnsetHoverPos;
    POINT last_hover_dispatch_pos = kUnsetHoverPos;

    DWORD last_md_hit_tick = 0;
    DWORD last_copy_hit_tick = 0;

    constexpr void Reset() noexcept
    {
        last_md_hit_pos = kUnsetHoverPos;
        last_copy_hit_pos = kUnsetHoverPos;
        last_hover_dispatch_pos = kUnsetHoverPos;
        last_md_hit_tick = 0;
        last_copy_hit_tick = 0;
    }

    // OS の MOUSEMOVE が同一座標で繰り返し届くことがあるため、
    // 完全同一座標の連続ディスパッチを抑止する。
    // 最低限の前段フィルタであり、通常のスロットリングは
    // last_md_hit_pos 等の距離比較で行う。
    constexpr bool ShouldSkipSameDispatch(int px, int py) noexcept
    {
        if (last_hover_dispatch_pos.x == px && last_hover_dispatch_pos.y == py) {
            return true;
        }
        last_hover_dispatch_pos = { px, py };
        return false;
    }

    [[nodiscard]] bool TryMarkMoved(POINT& last_pos, DWORD& last_tick, int px, int py) noexcept
    {
        if (IsUnset(last_pos)) {
            last_pos = { px, py };
            last_tick = GetTickCount();
            return true;
        }
        const int dx = px - last_pos.x;
        const int dy = py - last_pos.y;
        if (dx * dx + dy * dy <= HOVER_THROTTLE_DISTANCE_SQ) {
            return false;
        }
        const DWORD now = GetTickCount();
        if (last_tick != 0 && (now - last_tick) < HOVER_THROTTLE_MIN_INTERVAL_MS) {
            return false;
        }
        last_pos = { px, py };
        last_tick = now;
        return true;
    }
};
