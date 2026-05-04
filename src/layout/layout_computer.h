#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "theme.h"
#include <limits>
#include <memory_resource>

namespace mendo::layout {

inline float NodeIndent(const Node& node, const Theme& theme) noexcept
{
    return node.indent_level * theme.indent_width;
}

inline float NodeTextXOffset(const Node& node, const Theme& theme) noexcept
{
    return (node.type == NodeType::CodeBlock) ? theme.code_block_padding : 0.0f;
}

float GetSpacingAbove(const Node& node, const Theme& theme) noexcept;
float GetSpacingBelow(const Node& node, const Theme& theme) noexcept;

// ノード i の「テキスト上端 Y」を Fenwick から O(log N) で取得する。
// entry.text_top と同値で、margin_top + PrefixSum(i) + spacing_above[i] を返す。
inline float TextTopOf(const LayoutCache& cache, size_t i, const Node& node, const Theme& theme) noexcept
{
    return cache.GetBlockTop(i, theme.margin_top) + GetSpacingAbove(node, theme);
}

void ComputeColumnWidths(std::pmr::vector<float>& out,
                         const std::pmr::vector<float>& natural_widths,
                         float available_width, size_t col_count);

float EstimateNodeHeight(const Node& node, const Theme& theme) noexcept;

void EstimateNodeHeights(const std::pmr::vector<Node>& nodes, LayoutCache& cache, const Theme& theme);

struct YPositionResult {
    float total_height = 0.0f;
    bool has_dirty_nodes = false;
};

YPositionResult RecomputeYPositions(std::pmr::vector<Node>& nodes, LayoutCache& cache, const Theme& theme,
                                    size_t from_index = 0, bool has_earlier_dirty = false,
                                    size_t safe_exit_after = std::numeric_limits<size_t>::max()) noexcept;

// 文書全体の高さを Fenwick から計算する。末尾ノードの spacing_below を引いた値を返すため、
// レガシー版 ::ComputeTotalContentHeight (cache[last].text_top + height + margin_top) と同値。
inline float ComputeTotalContentHeight(const LayoutCache& cache, const std::pmr::vector<Node>& nodes,
                                        const Theme& theme, size_t node_count) noexcept
{
    if (node_count == 0) {
        return 0.0f;
    }
    const size_t last = node_count - 1;
    return cache.GetTotalHeightFromFenwick(theme.margin_top) - GetSpacingBelow(nodes[last], theme);
}

// (node, offset) → 絶対スクロール位置を Fenwick 経由で計算する。
inline float NodeOffsetToScrollY(const LayoutCache& cache, const std::pmr::vector<Node>& nodes,
                                  const Theme& theme, int node, float offset) noexcept
{
    if (cache.size() == 0 || node < 0) {
        return 0.0f;
    }
    const int clamped = std::min(node, static_cast<int>(cache.size()) - 1);
    return std::max(0.0f, TextTopOf(cache, static_cast<size_t>(clamped), nodes[clamped], theme) + offset);
}

// 下端が viewport_top 以上の最初のノードを Fenwick lower_bound で見つける。
// レガシー版 ::FindFirstVisibleNodeIndex (block_bottom 述語) とは spacing_below 分の意味論差があり、
// ±1 ノード境界で結果が異なる場合がある。R1 リスクとして本番切替前にテストで bit-exact 比較する。
inline int FindFirstVisibleNodeIndex(const LayoutCache& cache, const std::pmr::vector<Node>& /*nodes*/,
                                      const Theme& theme, size_t node_count, float viewport_top) noexcept
{
    const size_t effective = std::min(node_count, cache.size());
    if (effective == 0) {
        return 0;
    }
    const float target = std::max(0.0f, viewport_top - theme.margin_top);
    const size_t found = cache.FindBlockTopLowerBound(target);
    return static_cast<int>(std::min(found, effective));
}

} // namespace mendo::layout
