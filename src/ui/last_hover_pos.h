#pragma once
#include <windows.h>
#include <limits>

struct LastHoverPos {
    static constexpr POINT kUnsetPos{
        std::numeric_limits<LONG>::min(), std::numeric_limits<LONG>::min()
    };

    POINT pos = kUnsetPos;

    constexpr void Reset() noexcept
    {
        pos = kUnsetPos;
    }

    // OS の MOUSEMOVE が同一座標で繰り返し届くことがあるため、
    // 完全同一座標の連続ディスパッチを抑止する。
    // ヒットテスト自体は 1 万行のコードブロックでも数 µs なので、これ以上の間引きはしない。
    constexpr bool IsRepeat(int px, int py) noexcept
    {
        if (pos.x == px && pos.y == py) {
            return true;
        }
        pos = { px, py };
        return false;
    }
};
