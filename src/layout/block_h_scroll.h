#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "layout_computer.h"
#include "theme.h"
#include <algorithm>
#include <memory_resource>
#include <unordered_map>

// これ以下のはみ出しはスクロール対象にしない。表は列幅の丸めで収まる場合も可視幅をわずかに超え、
// 幅差が 0.5 DIP 未満なら列幅の再計算を省くため、最大 0.5 DIP 古い幅のまま残る。
constexpr float BLOCK_H_SCROLL_SLACK = 0.5f;

struct BlockHScrollGeometry {
    // 描画されるブロックの幅 (表は配置後の列幅合計、コードは折り返さない最長行)。
    float block_width = 0.0f;
    float visible_width = 0.0f;

    constexpr bool overflows() const noexcept
    {
        return block_width - visible_width > BLOCK_H_SCROLL_SLACK;
    }
    constexpr bool can_scroll() const noexcept
    {
        return overflows() && visible_width > 0.0f;
    }
    constexpr float scroll_max() const noexcept
    {
        return overflows() ? block_width - visible_width : 0.0f;
    }
    constexpr float ClampScrollX(float x) const noexcept
    {
        return can_scroll() ? std::clamp(x, 0.0f, scroll_max()) : 0.0f;
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
