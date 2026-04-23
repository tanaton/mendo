#pragma once
#include <windows.h>
#include <climits>

// ホバー時のヒットテスト省略判定用の距離の二乗（ピクセル²）
inline constexpr int HOVER_THROTTLE_DISTANCE_SQ = 16;

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

    // 前回チェック位置からの移動距離²がしきい値を超えていれば位置を更新して true を返す。
    // 返り値 true ならヒットテストを再実行すべきことを意味する。
    [[nodiscard]] bool TryMarkMoved(POINT& last_pos, int px, int py) noexcept
    {
        const int dx = px - last_pos.x;
        const int dy = py - last_pos.y;
        if (dx * dx + dy * dy > HOVER_THROTTLE_DISTANCE_SQ) {
            last_pos = { px, py };
            return true;
        }
        return false;
    }
};
