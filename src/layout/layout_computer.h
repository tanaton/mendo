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
// 旧 entry.y_position と等価で、margin_top + PrefixSum(i) + spacing_above[i] を返す。
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

} // namespace mendo::layout
