#include "layout.h"
#include <algorithm>

// Named constants for magic numbers
static constexpr float MIN_COLUMN_WIDTH = 30.0f;
static constexpr float COLUMN_WIDTH_PADDING = 4.0f;

// ---- Free functions ----

std::vector<float> ComputeColumnWidths(const std::vector<float>& natural_widths,
                                        float available_width, size_t col_count) {
    std::vector<float> widths(col_count);
    available_width = std::max(available_width, static_cast<float>(col_count) * MIN_COLUMN_WIDTH);

    float total_natural = 0;
    for (float w : natural_widths) total_natural += w;

    if (total_natural > 0 && total_natural > available_width) {
        for (size_t c = 0; c < col_count; c++) {
            widths[c] = std::max(MIN_COLUMN_WIDTH, available_width * natural_widths[c] / total_natural);
        }
    } else {
        float even = available_width / static_cast<float>(col_count);
        for (size_t c = 0; c < col_count; c++) {
            widths[c] = std::max(natural_widths[c] + COLUMN_WIDTH_PADDING, even);
        }
    }
    return widths;
}

std::wstring BuildLinearizedTableText(const std::vector<TableRow>& rows) {
    size_t total = 0;
    for (size_t r = 0; r < rows.size(); r++) {
        const auto& row = rows[r];
        for (size_t c = 0; c < row.cells.size(); c++) {
            if (c > 0) total++;
            total += row.cells[c].text.size();
        }
        if (r + 1 < rows.size()) total++;
    }
    std::wstring text;
    text.reserve(total);
    for (size_t r = 0; r < rows.size(); r++) {
        const auto& row = rows[r];
        for (size_t c = 0; c < row.cells.size(); c++) {
            if (c > 0) text += L'\t';
            text += row.cells[c].text;
        }
        if (r + 1 < rows.size()) text += L'\n';
    }
    return text;
}

YPositionResult RecomputeYPositions(std::vector<Node>& nodes, LayoutCache& cache, const Theme& theme,
                                    size_t from_index, bool has_earlier_dirty) {
    YPositionResult result;
    result.has_dirty_nodes = has_earlier_dirty;
    float y = theme.margin_top;

    // Resume from the previous node's end position when starting mid-stream.
    if (from_index > 0 && from_index < nodes.size()) {
        auto& prev = cache[from_index - 1];
        y = prev.y_position + prev.height;
        if (nodes[from_index - 1].type == NodeType::Heading) {
            y += theme.heading_spacing_below;
        } else {
            y += theme.paragraph_spacing;
        }
    }

    for (size_t i = from_index; i < nodes.size(); i++) {
        auto& entry = cache[i];
        if (entry.layout_dirty) result.has_dirty_nodes = true;

        if (nodes[i].type == NodeType::Heading) {
            y += theme.heading_spacing_above;
        }

        entry.y_position = y;
        y += entry.height;

        if (nodes[i].type == NodeType::Heading) {
            y += theme.heading_spacing_below;
        } else {
            y += theme.paragraph_spacing;
        }
    }

    result.total_height = y + theme.margin_top;
    return result;
}

// ---- LayoutEngine ----

bool LayoutEngine::Init(ITextMeasurer* measurer, const Theme& theme) {
    measurer_ = measurer;
    theme_ = &theme;
    return measurer_->Init(theme);
}

bool LayoutEngine::RecreateFormats() {
    if (!measurer_) return false;
    last_viewport_width_ = 0.0f;
    return measurer_->RecreateFormats();
}

void LayoutEngine::CreateTextLayout(Node& node, NodeLayoutEntry& entry, float max_width) {
    measurer_->MeasureNode(node, entry, max_width);
}

void LayoutEngine::ComputeLayout(std::vector<Node>& nodes, LayoutCache& cache,
                                  float viewport_width,
                                  float viewport_top, float viewport_bottom) {
    cache.Resize(nodes.size());

    bool width_changed = (viewport_width != last_viewport_width_);
    bool partial = (viewport_top >= 0.0f);

    // In partial mode, don't update last_viewport_width_ so that
    // subsequent batch processing still detects the width change
    if (!partial) {
        last_viewport_width_ = viewport_width;
    }

    float content_width = viewport_width - theme_->margin_left - theme_->margin_right;
    float y = theme_->margin_top;
    bool any_dirty = false;
    bool any_height_changed = false;
    bool broke_early = false;

    for (size_t i = 0; i < nodes.size(); i++) {
        auto& node = nodes[i];
        auto& entry = cache[i];
        float indent = node.indent_level * theme_->indent_width;
        float node_width = content_width - indent;

        bool needs_layout = width_changed || entry.layout_dirty;

        if (needs_layout) {
            if (partial) {
                // In partial mode, only compute layouts for visible nodes
                float node_bottom = y + entry.height; // estimate using old height
                bool visible = (node_bottom >= viewport_top && y <= viewport_bottom);
                if (visible) {
                    float old_height = entry.height;
                    CreateTextLayout(node, entry, node_width);
                    if (entry.height != old_height) any_height_changed = true;
                } else {
                    entry.layout_dirty = true;
                }
            } else {
                CreateTextLayout(node, entry, node_width);
            }
        }

        if (entry.layout_dirty) any_dirty = true;

        // Set y position directly in this pass
        if (node.type == NodeType::Heading) {
            y += theme_->heading_spacing_above;
        }
        entry.y_position = y;
        y += entry.height;
        if (node.type == NodeType::Heading) {
            y += theme_->heading_spacing_below;
        } else {
            y += theme_->paragraph_spacing;
        }

        // Early exit: in partial mode without width change, once past viewport
        // and no height changes, remaining Y positions are unchanged.
        if (partial && !width_changed && !any_height_changed && y > viewport_bottom) {
            // Conservatively assume dirty nodes may exist beyond the break point.
            // ProcessDirtyBatch will quickly confirm and clear if none exist.
            any_dirty = true;
            broke_early = true;
            break;
        }
    }

    if (!broke_early) {
        total_height_ = y + theme_->margin_top;
    }
    has_dirty_nodes_ = any_dirty;
}

void LayoutEngine::LayoutNodes(std::vector<Node>& nodes, LayoutCache& cache, float viewport_width) {
    last_viewport_width_ = 0.0f; // Force width change detection
    ComputeLayout(nodes, cache, viewport_width + theme_->margin_left + theme_->margin_right);
}

bool LayoutEngine::EnsureVisibleLayout(std::vector<Node>& nodes, LayoutCache& cache,
                                        float viewport_width,
                                        float viewport_top, float viewport_bottom) {
    float content_width = viewport_width - theme_->margin_left - theme_->margin_right;
    bool any_updated = false;

    // Find the first node whose bottom edge >= viewport_top
    int lo = FindFirstVisibleNodeIndex(cache, nodes.size(), viewport_top);

    for (int i = lo; i < static_cast<int>(nodes.size()); i++) {
        auto& entry = cache[i];
        if (entry.y_position > viewport_bottom) break;
        if (!entry.layout_dirty) continue;
        float indent = nodes[i].indent_level * theme_->indent_width;
        CreateTextLayout(nodes[i], entry, content_width - indent);
        any_updated = true;
    }

    if (any_updated) {
        auto result = RecomputeYPositions(nodes, cache, *theme_, static_cast<size_t>(lo), has_dirty_nodes_);
        total_height_ = result.total_height;
        has_dirty_nodes_ = result.has_dirty_nodes;
    }
    return any_updated;
}

bool LayoutEngine::ProcessDirtyBatch(std::vector<Node>& nodes, LayoutCache& cache,
                                      float viewport_width, int batch_size) {
    float content_width = viewport_width - theme_->margin_left - theme_->margin_right;
    int processed = 0;
    size_t first_dirty = nodes.size();

    for (size_t i = 0; i < nodes.size(); i++) {
        auto& entry = cache[i];
        if (!entry.layout_dirty) continue;

        if (first_dirty == nodes.size()) first_dirty = i;
        float indent = nodes[i].indent_level * theme_->indent_width;
        CreateTextLayout(nodes[i], entry, content_width - indent);

        if (++processed >= batch_size) break;
    }

    if (processed == 0) {
        has_dirty_nodes_ = false;
        return false;
    }

    auto result = RecomputeYPositions(nodes, cache, *theme_, first_dirty, false);
    total_height_ = result.total_height;
    has_dirty_nodes_ = result.has_dirty_nodes;

    // Update last_viewport_width_ when all dirty nodes are processed
    if (!has_dirty_nodes_) {
        last_viewport_width_ = viewport_width;
    }
    return has_dirty_nodes_;
}
