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

void ComputeColumnWidths(std::pmr::vector<float>& out,
                         const std::pmr::vector<float>& natural_widths,
                         float available_width, size_t col_count);

std::pmr::wstring BuildLinearizedTableText(const std::pmr::vector<TableRow>& rows);

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
