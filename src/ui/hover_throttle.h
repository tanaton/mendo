#pragma once
#include <windows.h>
#include <climits>

// ヒットテストのスロットリング状態。
// マウス位置が一定距離以上動いた場合のみヒットテストを再実行する。
struct HoverThrottle {
    POINT last_md_hit_pos = { LONG_MIN, LONG_MIN };
    bool last_md_cursor_hand = false;
    POINT last_copy_hit_pos = { LONG_MIN, LONG_MIN };
    POINT last_save_hit_pos = { LONG_MIN, LONG_MIN };
    POINT last_hover_dispatch_pos = { LONG_MIN, LONG_MIN };

    void Reset() noexcept
    {
        last_md_hit_pos = { LONG_MIN, LONG_MIN };
        last_copy_hit_pos = { LONG_MIN, LONG_MIN };
        last_save_hit_pos = { LONG_MIN, LONG_MIN };
        last_hover_dispatch_pos = { LONG_MIN, LONG_MIN };
    }

    // OS の MOUSEMOVE が同一座標で繰り返し届くことがあるため、
    // 完全同一座標の連続ディスパッチを抑止する。
    // 最低限の前段フィルタであり、通常のスロットリングは
    // last_md_hit_pos 等の距離比較で行う。
    bool ShouldSkipSameDispatch(int px, int py) noexcept
    {
        if (last_hover_dispatch_pos.x == px && last_hover_dispatch_pos.y == py) {
            return true;
        }
        last_hover_dispatch_pos = { px, py };
        return false;
    }
};
