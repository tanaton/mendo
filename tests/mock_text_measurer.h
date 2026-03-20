#pragma once
#include "text_measurer.h"
#include <cmath>

// Mock text measurer for testing LayoutEngine without DirectWrite.
// Heights are computed deterministically from text length and node type.
class MockTextMeasurer : public ITextMeasurer {
public:
    float line_height = 20.0f;       // base line height
    float chars_per_line = 40.0f;    // characters per line for wrap estimation
    float heading_scale = 1.5f;      // heading height multiplier
    float table_row_height = 28.0f;  // height per table row
    float table_border = 1.0f;       // table border width

    bool Init(const Theme&) override { return true; }
    bool RecreateFormats() override { return true; }
    void UpdateTheme(const Theme&) noexcept override {}

    void MeasureNode(Node& node, NodeLayoutEntry& entry, float max_width) override {
        if (node.type == NodeType::HorizontalRule) {
            entry.height = 10.0f;
            entry.layout_dirty = false;
            return;
        }
        if (node.type == NodeType::Table) {
            MeasureTable(node, entry, max_width);
            return;
        }

        const auto& text = node.text;
        if (text.empty()) {
            entry.height = line_height * 0.5f;
            entry.layout_dirty = false;
            return;
        }

        // Estimate lines based on text length and available width
        float effective_chars = (max_width > 0) ? (max_width / (line_height * 0.6f)) : chars_per_line;
        effective_chars = std::max(effective_chars, 1.0f);
        float lines = std::ceil(static_cast<float>(text.size()) / effective_chars);
        float h = lines * line_height;

        if (node.type == NodeType::Heading) {
            h *= heading_scale;
        }

        entry.text_layout = nullptr; // no real layout in mock
        entry.height = h;
        entry.layout_dirty = false;
        entry.effects_applied = false;
    }

    void MeasureTable(Node& node, NodeLayoutEntry& entry, float max_width) override {
        if (node.table_rows.empty()) {
            entry.height = 0;
            entry.layout_dirty = false;
            return;
        }

        size_t col_count = 0;
        for (auto& row : node.table_rows) {
            col_count = std::max(col_count, row.cells.size());
        }
        if (col_count == 0) { entry.layout_dirty = false; return; }

        entry.col_widths.assign(col_count, max_width / static_cast<float>(col_count));
        entry.row_heights.assign(node.table_rows.size(), table_row_height);
        entry.cell_layouts.resize(node.table_rows.size());
        for (size_t r = 0; r < node.table_rows.size(); r++) {
            entry.cell_layouts[r].resize(node.table_rows[r].cells.size());
        }

        float total = table_border;
        for (size_t r = 0; r < node.table_rows.size(); r++) {
            total += table_row_height + table_border;
        }
        entry.height = total;
        entry.layout_dirty = false;
    }
};
