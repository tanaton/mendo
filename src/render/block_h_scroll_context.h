#pragma once
#include "block_h_scroll.h"
#include <memory_resource>
#include <unordered_map>

// node のブロック横スクロール量。マップ未設定・未登録は 0。
// 保存値は幅が縮んでも残るので、描画・ヒットテスト・入力が同じ値を使うよう読み出し時に必ず clamp する。
inline float LookupBlockScrollX(const std::pmr::unordered_map<int, float>* scroll_x, int node_index, const BlockHScrollGeometry& geom) noexcept
{
    if (!scroll_x) {
        return 0.0f;
    }
    const auto it = scroll_x->find(node_index);
    return (it != scroll_x->end()) ? geom.ClampScrollX(it->second) : 0.0f;
}

struct BlockHScrollContext {
    const std::pmr::unordered_map<int, float>* scroll_x = nullptr;
    int hovered_block = -1;
    int drag_block = -1;

    float GetScrollX(int node_index, const BlockHScrollGeometry& geom) const noexcept
    {
        return LookupBlockScrollX(scroll_x, node_index, geom);
    }
};
