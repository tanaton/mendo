#pragma once
#include "types.h"
#include "layout_cache.h"
#include "theme.h"

// Abstract text measurement interface.
// Separates DirectWrite dependency from layout logic, enabling mock-based testing.
class ITextMeasurer {
public:
    virtual ~ITextMeasurer() = default;

    virtual bool Init(const Theme& theme) = 0;
    virtual bool RecreateFormats() = 0;
    virtual void UpdateTheme(const Theme& theme) noexcept = 0;

    // Create and measure a text layout for a non-table node.
    // Sets entry.text_layout, entry.height, entry.layout_dirty, entry.effects_applied.
    virtual void MeasureNode(Node& node, NodeLayoutEntry& entry, float max_width) = 0;

    // Create and measure table cell layouts.
    // Sets entry.cell_layouts, entry.col_widths, entry.row_heights, entry.height, entry.layout_dirty.
    virtual void MeasureTable(Node& node, NodeLayoutEntry& entry, float max_width) = 0;
};
