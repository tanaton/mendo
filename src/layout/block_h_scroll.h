#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "layout_computer.h"
#include "theme.h"
#include <algorithm>
#include <memory_resource>
#include <span>
#include <unordered_map>

// 1px 未満のスクロール範囲は操作として意味がない。表は列幅の丸めと、幅差 CELL_WIDTH_EPSILON 未満で
// 列幅の再計算を省く経路により、収まる場合でもこの程度まで可視幅を超えうる。
constexpr float BLOCK_H_SCROLL_SLACK = CELL_WIDTH_EPSILON;

struct BlockHScrollGeometry {
    // 描画されるブロックの幅。表は配置後の列幅合計、コードは最長行に左右の余白を足した幅。
    float block_width = 0.0f;
    float visible_width = 0.0f;
    // 横スクロール量を持つ鍵 (ノード番号)。区切ったコードブロックは並びの先頭。
    int scroll_key = -1;

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
// 区切ったコードブロックは並び全体を 1 つのブロックとして横スクロールする。スクロール量は並びの先頭を鍵に持ち、
// 幅は並びのうち計測済みの部分の最大にする (部分ごとに幅が違うと、継ぎ目で行がずれる)。
inline BlockHScrollGeometry GetBlockHScrollGeometry(
    std::span<const Node> nodes, const LayoutCache& cache, size_t node_index, float visible_width) noexcept
{
    const Node& node = nodes[node_index];
    BlockHScrollGeometry g;
    g.visible_width = visible_width;
    g.scroll_key = static_cast<int>(node_index);
    // 表は描画と同じ TableWidth を使う。幾何が無効な間は GenTable も描かないのでスクロール不可。
    if (const auto* tl = cache[node_index].table_layout(); node.type == NodeType::Table && tl && tl->HasColGeometry()) {
        g.block_width = tl->TableWidth();
    }
    else if (node.type == NodeType::CodeBlock) {
        const size_t first = ChunkRunFirst(nodes, node_index);
        g.scroll_key = static_cast<int>(first);
        for (size_t i = first, last = ChunkRunLast(nodes, node_index); i <= last; ++i) {
            g.block_width = std::max(g.block_width, cache[i].natural_code_width());
        }
    }
    return g;
}

inline BlockHScrollGeometry GetBlockHScrollGeometry(
    std::span<const Node> nodes, const LayoutCache& cache, size_t node_index, const Theme& theme, float md_pane_width) noexcept
{
    return GetBlockHScrollGeometry(nodes, cache, node_index, layout::NodeContentWidth(nodes[node_index], theme, theme.ContentWidth(md_pane_width)));
}

// geom のブロックの横スクロール量。マップ未設定・未登録は 0。
// 保存値は幅が縮んでも残るので、描画・ヒットテスト・入力が同じ値を使うよう読み出し時に必ず clamp する。
inline float LookupBlockScrollX(const std::pmr::unordered_map<int, float>* scroll_x, const BlockHScrollGeometry& geom) noexcept
{
    if (!scroll_x) {
        return 0.0f;
    }
    const auto it = scroll_x->find(geom.scroll_key);
    return (it != scroll_x->end()) ? geom.ClampScrollX(it->second) : 0.0f;
}

struct BlockHScrollContext {
    const std::pmr::unordered_map<int, float>* scroll_x = nullptr;
    int hovered_block = -1;
    int drag_block = -1;
};
