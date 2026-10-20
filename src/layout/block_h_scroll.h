#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "layout_computer.h"
#include "theme.h"
#include <algorithm>
#include <memory_resource>
#include <unordered_map>

// 1px 未満のスクロール範囲は操作として意味がない。表は列幅の丸めと、幅差 CELL_WIDTH_EPSILON 未満で
// 列幅の再計算を省く経路により、収まる場合でもこの程度まで可視幅を超えうる。
constexpr float BLOCK_H_SCROLL_SLACK = CELL_WIDTH_EPSILON;

struct BlockHScrollGeometry {
    // 表は描画と同じ配置後の幅、コードは最長行の幅 (左右の余白を含まない)。
    float block_width = 0.0f;
    float visible_width = 0.0f;

    constexpr bool can_scroll() const noexcept
    {
        return visible_width > 0.0f && block_width - visible_width > BLOCK_H_SCROLL_SLACK;
    }
    constexpr float scroll_max() const noexcept
    {
        return can_scroll() ? block_width - visible_width : 0.0f;
    }
    constexpr float ClampScrollX(float x) const noexcept
    {
        return std::clamp(x, 0.0f, scroll_max());
    }
};

// 呼び出し側が可視幅 (ContentWidth − indent) を計算済みの場合のオーバーロード。
inline BlockHScrollGeometry GetBlockHScrollGeometry(
    const Node& node, const NodeLayoutEntry& entry, float visible_width) noexcept
{
    BlockHScrollGeometry g;
    g.visible_width = visible_width;
    // 表は描画と同じ TableWidth を使う。幾何が無効な間は GenTable も描かないのでスクロール不可。
    if (const auto* tl = entry.table_layout(); node.type == NodeType::Table && tl && tl->HasColGeometry()) {
        g.block_width = tl->TableWidth();
    }
    else if (node.type == NodeType::CodeBlock) {
        g.block_width = entry.natural_code_width();
    }
    return g;
}

inline BlockHScrollGeometry GetBlockHScrollGeometry(
    const Node& node, const NodeLayoutEntry& entry, const Theme& theme, float md_pane_width) noexcept
{
    return GetBlockHScrollGeometry(node, entry, layout::NodeContentWidth(node, theme, theme.ContentWidth(md_pane_width)));
}

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
