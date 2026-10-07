#pragma once
// command_generator の分割ファイル間でのみ使用する内部共有宣言。
#include "command_generator.h"
#include <span>

inline D2D1_RECT_F OffsetRectF(const D2D1_RECT_F& r, float origin_x, float origin_y) noexcept
{
    return D2D1::RectF(origin_x + r.left, origin_y + r.top, origin_x + r.right, origin_y + r.bottom);
}

// 可視 Y 範囲 [cull_top, cull_bottom] (ペインローカル) と重なるか。
constexpr bool OverlapsY(const D2D1_RECT_F& r, float cull_top, float cull_bottom) noexcept
{
    return !IsOffscreen(r.top, r.bottom - r.top, cull_top, cull_bottom);
}

// bgs はパディング適用済みのレイアウト原点相対矩形。
inline void GenInlineCodeBgs(DrawCommandList& cmds, std::span<const InlineCodeBg> bgs, float origin_x, float origin_y, D2D1_COLOR_F color, float cull_top, float cull_bottom)
{
    for (const auto& bg : bgs) {
        const auto r = OffsetRectF(bg, origin_x, origin_y);
        if (OverlapsY(r, cull_top, cull_bottom)) {
            cmds.emplace_back(FillRoundedRectCmd{ r, INLINE_CODE_CORNER, INLINE_CODE_CORNER, color });
        }
    }
}

// テーブルセルのインラインコード背景を描画する。
// bgs は cell_index 昇順を維持しているため、cursor を進めるだけで O(N) 全体で済む。
// 戻り値は次回呼び出し向けに進めた cursor。
inline size_t GenCellInlineCodeBgs(DrawCommandList& cmds, std::span<const CellInlineCodeBg> bgs, size_t cursor, uint32_t cell_index, float origin_x, float origin_y, D2D1_COLOR_F color)
{
    while (cursor < bgs.size() && bgs[cursor].cell_index < cell_index) {
        ++cursor;
    }
    while (cursor < bgs.size() && bgs[cursor].cell_index == cell_index) {
        cmds.emplace_back(FillRoundedRectCmd{ OffsetRectF(bgs[cursor].rect, origin_x, origin_y), INLINE_CODE_CORNER, INLINE_CODE_CORNER, color });
        ++cursor;
    }
    return cursor;
}

#ifdef MENDO_USE_TRACY
#include <cstdint>

// command_generator.cpp で定義される Tracy プロット用統計カウンタ。
// 分割 cpp から MENDO_COUNT_INC/SET で参照するために外部リンケージが必要。
struct CmdGenStats {
    int64_t hittest_range = 0;
    int64_t sel_hl_cache_hit = 0;
    int64_t sel_hl_cache_miss = 0;
    int64_t search_hl_rebuild = 0;
    int64_t search_hl_provisional = 0;
    int64_t last_visible_node_count = 0;
};
extern CmdGenStats g_cmd_gen_stats;

#endif
