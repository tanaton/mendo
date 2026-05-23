#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "layout_computer.h"
#include "theme.h"

struct BlockHScrollGeometry {
    float natural_width = 0.0f;
    float visible_width = 0.0f;

    constexpr bool can_scroll() const noexcept
    {
        return natural_width > visible_width && visible_width > 0.0f;
    }
    constexpr float scroll_max() const noexcept
    {
        return natural_width > visible_width ? natural_width - visible_width : 0.0f;
    }
};

inline BlockHScrollGeometry GetBlockHScrollGeometry(
    const Node& node, const NodeLayoutEntry& entry, const Theme& theme, float md_pane_width) noexcept
{
    BlockHScrollGeometry g;
    g.visible_width = theme.ContentWidth(md_pane_width) - mendo::layout::NodeIndent(node, theme);
    if (node.type == NodeType::Table && entry.has_table_layout()) {
        g.natural_width = entry.table_layout->natural_total_width;
    }
    else if (IsScrollableCodeBlock(node)) {
        g.natural_width = entry.natural_code_width;
    }
    return g;
}
