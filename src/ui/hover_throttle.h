#pragma once
#include <windows.h>
#include <limits>

// ホバー時のヒットテスト省略判定用の距離の二乗（ピクセル²）
inline constexpr int HOVER_THROTTLE_DISTANCE_SQ = 16;
// MOUSEMOVE バースト時にヒットテストを連発させないための最小経過時間（ミリ秒）。
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

// ヒットテストのスロットリング状態。
// マウス位置が一定距離以上動いた場合のみヒットテストを再実行する。
struct HoverThrottle {
    POINT last_md_hit_pos = kUnsetHoverPos;
    bool last_md_cursor_hand = false;
    POINT last_copy_hit_pos = kUnsetHoverPos;
    POINT last_hover_dispatch_pos = kUnsetHoverPos;

    // 各ターゲット種別のヒットテスト最終実行時刻（GetTickCount）。
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

    // 距離 + 時間の二重ガード版。両方を満たした時のみ位置・時刻を更新して true を返す。
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

    // 距離のみ判定。時間ガードが不要な経路（タイマー駆動など）向け。
    [[nodiscard]] constexpr bool TryMarkMoved(POINT& last_pos, int px, int py) noexcept
    {
        if (IsUnset(last_pos)) {
            last_pos = { px, py };
            return true;
        }
        const int dx = px - last_pos.x;
        const int dy = py - last_pos.y;
        if (dx * dx + dy * dy > HOVER_THROTTLE_DISTANCE_SQ) {
            last_pos = { px, py };
            return true;
        }
        return false;
    }
};
