#pragma once
#include <windows.h>
#include <climits>

// ヒットテストのスロットリング状態。
// マウス位置が一定距離以上動いた場合のみヒットテストを再実行する。
struct HoverThrottle {
    POINT last_md_hit_pos = { LONG_MIN, LONG_MIN };
    bool last_md_cursor_hand = false;
    POINT last_copy_hit_pos = { LONG_MIN, LONG_MIN };

    void Reset() noexcept
    {
        last_md_hit_pos = { LONG_MIN, LONG_MIN };
        last_copy_hit_pos = { LONG_MIN, LONG_MIN };
    }
};
