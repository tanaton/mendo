#include "command_generator.h"
#include "ui_constants.h"
#include <algorithm>
#include <ranges>

void CommandGenerator::GenTableRowBg(DrawCommandList& cmds, bool is_header, bool is_even_row,
    float x, float y, float table_width, float row_h, float border)
{
    if (is_header) {
        cmds.emplace_back(FillRectCmd{
            D2D1::RectF(x, y, x + table_width, y + row_h + border),
            theme_->code_bg_color });
    }
    else if (is_even_row) {
        cmds.emplace_back(FillRectCmd{
            D2D1::RectF(x, y, x + table_width, y + row_h + border),
            cached_stripe_color_ });
    }
}

void CommandGenerator::GenTableCellContent(DrawCommandList& cmds, const TableCell& cell,
    IDWriteTextLayout* cell_layout,
    float text_x, float text_y,
    bool has_selection, uint32_t sel_start, uint32_t sel_end,
    uint32_t flat_offset)
{
    if (has_selection && cell_layout) {
        const uint32_t cell_len = static_cast<uint32_t>(cell.text.size());
        const uint32_t ov_start = std::max(sel_start, flat_offset);
        const uint32_t ov_end = std::min(sel_end, flat_offset + cell_len);
        if (ov_end > ov_start) {
            GenSelectionHighlight(cmds, cell_layout, ov_start - flat_offset, ov_end - ov_start, text_x, text_y);
        }
    }
    if (cell_layout) {
        const D2D1_COLOR_F cell_color = cell.is_header ? theme_->heading_color : theme_->text_color;
        cmds.emplace_back(DrawTextLayoutCmd{ D2D1::Point2F(text_x, text_y), cell_layout, cell_color });
    }
}

void CommandGenerator::GenTable(DrawCommandList& cmds,
    const Node& node, const NodeLayoutEntry& entry,
    int node_index, float offset_x)
{
    if (node.table_rows().empty() || !entry.has_table_layout() || entry.table_layout->col_widths.empty()) {
        return;
    }

    const float cell_padding = TABLE_CELL_PADDING;
    const float border = TABLE_BORDER_WIDTH;
    const auto& tl = *entry.table_layout;
    const auto& selection = *frame_selection_;
    const float viewport_top = frame_viewport_top_;
    const float viewport_bottom = frame_viewport_bottom_;

    const float table_width = std::ranges::fold_left(
        tl.col_widths,
        border,
        [cell_padding, border](float acc, float cw) noexcept { return acc + cw + cell_padding * 2.0f + border; }
    );

    bool has_selection = selection.active && (node_index >= selection.start_node) && (node_index <= selection.end_node);
    uint32_t sel_start = 0, sel_end = static_cast<uint32_t>(node.GetText().size());
    if (has_selection) {
        if (node_index == selection.start_node) {
            sel_start = selection.start_pos;
        }
        if (node_index == selection.end_node) {
            sel_end = selection.end_pos;
        }
        if (sel_end <= sel_start) {
            has_selection = false;
        }
    }

    // セル範囲の flat_offset を進めるヘルパー
    const auto advance_flat_offset = [](uint32_t& offset, const TableRow& row, size_t from, size_t to) static noexcept {
        for (size_t c = from; c < to; c++) {
            offset += static_cast<uint32_t>(row.cells[c].text.size());
            if (c + 1 < row.cells.size()) {
                offset++;
            }
        }
    };

    float y = entry.y_position;
    uint32_t flat_offset = 0;

    for (size_t r = 0; r < node.table_rows().size(); r++) {
        const auto& row = node.table_rows()[r];
        const float row_h = (r < tl.row_heights.size()) ? tl.row_heights[r] : (theme_->font_size_body * TABLE_ROW_HEIGHT_FACTOR);

        const float row_bottom = y + row_h + border;
        if (row_bottom < viewport_top || y > viewport_bottom) {
            // プリコンピュート済みの行オフセットを使い、O(cells) の走査を O(1) に削減
            if (r + 1 < node.table_rows().size() && r + 1 < tl.row_flat_offsets.size()) {
                flat_offset = tl.row_flat_offsets[r + 1];
            } else {
                advance_flat_offset(flat_offset, row, 0, row.cells.size());
            }
            y = row_bottom;
            continue;
        }

        const bool is_header_row = (!row.cells.empty() && row.cells[0].is_header);
        GenTableRowBg(cmds, is_header_row, r % 2 == 0, offset_x, y, table_width, row_h, border);

        // 行上部の水平線
        cmds.emplace_back(DrawLineCmd{
            D2D1::Point2F(offset_x, y), D2D1::Point2F(offset_x + table_width, y),
            theme_->hr_color, border });

        // セルを描画
        float cx = offset_x + border;
        const size_t drawn_cols = std::min(row.cells.size(), tl.col_widths.size());
        for (size_t c = 0; c < drawn_cols; c++) {
            const auto& cell = row.cells[c];
            const float cw = tl.col_widths[c];

            // 垂直の罫線
            cmds.emplace_back(DrawLineCmd{
                D2D1::Point2F(cx - border, y), D2D1::Point2F(cx - border, y + row_h + border),
                theme_->hr_color, border });

            const float text_x = cx + cell_padding;
            const float text_y = y + cell_padding;

            IDWriteTextLayout* cell_layout = tl.GetCellLayout(r, c);

            // セルのインラインコード背景
            if (const size_t ci = tl.CellIndex(r, c); ci < tl.cell_inline_code_bgs.size()) {
                GenInlineCodeBgs(cmds, tl.cell_inline_code_bgs[ci], text_x, text_y, theme_->code_bg_color);
            }

            // 検索マッチのハイライト（テーブルセル）
            GenSearchHighlights(cmds, cell_layout, node_index, text_x, text_y,
                static_cast<int>(r), static_cast<int>(c));

            GenTableCellContent(cmds, cell, cell_layout, text_x, text_y, has_selection, sel_start, sel_end, flat_offset);

            flat_offset += static_cast<uint32_t>(cell.text.size());
            if (c + 1 < row.cells.size()) {
                flat_offset++;
            }
            cx += cw + cell_padding * 2.0f + border;
        }

        advance_flat_offset(flat_offset, row, drawn_cols, row.cells.size());

        // 右罫線
        cmds.emplace_back(DrawLineCmd{
            D2D1::Point2F(offset_x + table_width, y),
            D2D1::Point2F(offset_x + table_width, y + row_h + border),
            theme_->hr_color, border });

        y += row_h + border;
        if (r + 1 < node.table_rows().size()) {
            flat_offset++;
        }
    }

    // 下罫線
    cmds.emplace_back(DrawLineCmd{
        D2D1::Point2F(offset_x, y), D2D1::Point2F(offset_x + table_width, y),
        theme_->hr_color, border });
}
