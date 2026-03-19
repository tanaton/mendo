#include "hit_test_service.h"

HitTestService::HitResult HitTestService::HitTest(
    const std::vector<Node>& nodes,
    const LayoutCache& cache,
    const Theme& theme,
    float scroll_y,
    float md_pane_left,
    float dpi_scale,
    int screen_x, int screen_y) const {

    HitResult result;
    if (nodes.empty()) return result;

    // Convert physical pixels to DIPs
    float dip_x = screen_x / dpi_scale;
    float dip_y = screen_y / dpi_scale + scroll_y;

    // Offset by MD pane position
    dip_x -= md_pane_left;

    // Binary search for the node containing dip_y
    int lo = 0, hi = static_cast<int>(nodes.size()) - 1;
    int candidate = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cache[mid].y_position <= dip_y) {
            candidate = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    if (candidate >= 0 && dip_y <= cache[candidate].y_position + cache[candidate].height) {
        const auto& node = nodes[candidate];
        const auto& entry = cache[candidate];

        if (node.type == NodeType::Table) {
            return HitTestTable(node, entry, candidate, theme, dip_x, dip_y);
        }

        if (entry.text_layout) {
            float indent = node.indent_level * theme.indent_width;
            float local_x = dip_x - theme.margin_left - indent;
            float local_y = dip_y - entry.y_position;

            BOOL is_trailing = FALSE;
            BOOL is_inside = FALSE;
            DWRITE_HIT_TEST_METRICS metrics{};
            entry.text_layout->HitTestPoint(local_x, local_y,
                                           &is_trailing, &is_inside, &metrics);

            result.node_index = candidate;
            result.text_pos = metrics.textPosition + (is_trailing ? 1 : 0);
            return result;
        }
    }

    // Click below all nodes → select end of last node
    for (int i = static_cast<int>(nodes.size()) - 1; i >= 0; i--) {
        if (!nodes[i].text.empty()) {
            result.node_index = i;
            result.text_pos = static_cast<uint32_t>(nodes[i].text.size());
            return result;
        }
    }
    return result;
}

HitTestService::HitResult HitTestService::HitTestTable(
    const Node& node, const NodeLayoutEntry& entry,
    int node_index,
    const Theme& theme,
    float dip_x, float dip_y) const {

    HitResult result;
    result.node_index = node_index;

    float indent = node.indent_level * theme.indent_width;
    float base_x = theme.margin_left + indent;
    float cell_padding = 8.0f;
    float border = 1.0f;

    // Find which row was clicked
    float ry = entry.y_position;
    int hit_row = -1;
    for (size_t r = 0; r < node.table_rows.size(); r++) {
        float row_h = (r < entry.row_heights.size()) ? entry.row_heights[r] : (theme.font_size_body * 1.4f);
        float row_bottom = ry + row_h + border;
        if (dip_y < row_bottom) {
            hit_row = static_cast<int>(r);
            break;
        }
        ry += row_h + border;
    }
    if (hit_row < 0) {
        result.text_pos = static_cast<uint32_t>(node.text.size());
        return result;
    }

    // Find which column was clicked
    float cx = base_x + border;
    int hit_col = static_cast<int>(entry.col_widths.size()) - 1; // default to last
    for (size_t c = 0; c < entry.col_widths.size(); c++) {
        float col_right = cx + entry.col_widths[c] + cell_padding * 2.0f;
        if (dip_x < col_right) {
            hit_col = static_cast<int>(c);
            break;
        }
        cx += entry.col_widths[c] + cell_padding * 2.0f + border;
    }
    if (hit_col < 0) hit_col = 0;

    // Compute flat text offset for cell (hit_row, hit_col)
    uint32_t flat_offset = 0;
    for (size_t r = 0; r < node.table_rows.size(); r++) {
        const auto& row_cells = node.table_rows[r].cells;
        for (size_t c = 0; c < row_cells.size(); c++) {
            if (static_cast<int>(r) == hit_row && static_cast<int>(c) == hit_col) {
                // Hit test within the cell's text layout
                IDWriteTextLayout* cell_layout = nullptr;
                if (r < entry.cell_layouts.size() && c < entry.cell_layouts[r].size()) {
                    cell_layout = entry.cell_layouts[r][c].Get();
                }
                if (cell_layout) {
                    // Compute cell text position
                    float cell_x = base_x + border;
                    for (size_t cc = 0; cc < c; cc++) {
                        cell_x += entry.col_widths[cc] + cell_padding * 2.0f + border;
                    }
                    float cell_text_x = cell_x + cell_padding;

                    float cell_y = entry.y_position;
                    for (size_t rr = 0; rr < r; rr++) {
                        float rh = (rr < entry.row_heights.size()) ? entry.row_heights[rr] : (theme.font_size_body * 1.4f);
                        cell_y += rh + border;
                    }
                    float cell_text_y = cell_y + cell_padding;

                    BOOL is_trailing = FALSE, is_inside = FALSE;
                    DWRITE_HIT_TEST_METRICS metrics{};
                    cell_layout->HitTestPoint(
                        dip_x - cell_text_x, dip_y - cell_text_y,
                        &is_trailing, &is_inside, &metrics);

                    result.text_pos = flat_offset + metrics.textPosition + (is_trailing ? 1 : 0);
                } else {
                    result.text_pos = flat_offset;
                }
                return result;
            }
            flat_offset += static_cast<uint32_t>(row_cells[c].text.size());
            if (c + 1 < row_cells.size()) flat_offset++; // tab
        }
        if (r + 1 < node.table_rows.size()) flat_offset++; // newline
    }

    result.text_pos = static_cast<uint32_t>(node.text.size());
    return result;
}

HitTestService::NavButtonHover HitTestService::NavButtonHitTest(
    float dip_x, float dip_y, const PaneRect& md_rect) const {
    // Must match the constants in renderer.cpp
    constexpr float BTN_SIZE = 32.0f;
    constexpr float BTN_MARGIN = 16.0f;
    constexpr float BTN_GAP = 2.0f;

    float base_x = md_rect.x + md_rect.width - BTN_MARGIN - BTN_SIZE * 2 - BTN_GAP - 16.0f;
    float base_y = md_rect.y + md_rect.height - BTN_MARGIN - BTN_SIZE;

    if (dip_y < base_y || dip_y > base_y + BTN_SIZE) return NavButtonHover::None;

    // Back button
    if (dip_x >= base_x && dip_x <= base_x + BTN_SIZE)
        return NavButtonHover::Back;
    // Forward button
    float fwd_x = base_x + BTN_SIZE + BTN_GAP;
    if (dip_x >= fwd_x && dip_x <= fwd_x + BTN_SIZE)
        return NavButtonHover::Forward;

    return NavButtonHover::None;
}
