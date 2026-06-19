#include "command_generator.h"
#include "layout.h"
#include "layout_computer.h"
#include <algorithm>

void CommandGenerator::GenTableRowBg(DrawCommandList& cmds, const TableRowGeom& g, bool is_header, bool is_even_row)
{
    if (!is_header && !is_even_row) {
        return;
    }
    const D2D1_RECT_F rect = D2D1::RectF(g.x, g.y, g.x + g.table_width, g.y + g.row_h + g.border);
    if (is_header) {
        cmds.emplace_back(FillRectCmd{ rect, theme_->code_bg_color, BrushId::CodeBg });
    }
    else {
        // cached_stripe_color_ と Renderer の brushes_[TableStripe] は同じ式 (renderer_resources.cpp) で算出する。
        cmds.emplace_back(FillRectCmd{ rect, cached_stripe_color_, BrushId::TableStripe });
    }
}

void CommandGenerator::GenTableCellContent(DrawCommandList& cmds, std::string_view cell_text, const CellDrawContext& ctx)
{
    if (ctx.has_selection && ctx.layout) {
        const uint32_t cell_len = static_cast<uint32_t>(cell_text.size());
        const uint32_t ov_start = std::max(ctx.sel_start, ctx.flat_offset);
        const uint32_t ov_end = std::min(ctx.sel_end, ctx.flat_offset + cell_len);
        if (ov_end > ov_start) {
            // sel_start/sel_end は UTF-8 byte offset。HitTestTextRange 用に UTF-16 へ変換する。
            const auto wr = cell_wv_.WideRange(cell_text, ov_start - ctx.flat_offset, ov_end - ov_start);
            if (wr.length > 0) {
                GenSelectionHighlight(cmds, ctx.layout, wr.startPosition, wr.length, ctx.text_x, ctx.text_y);
            }
        }
    }
    if (ctx.layout) {
        const D2D1_COLOR_F cell_color = ctx.is_header ? theme_->heading_color : theme_->text_color;
        const BrushId cell_brush = ctx.is_header ? BrushId::Heading : BrushId::Text;
        cmds.emplace_back(DrawTextLayoutCmd{ D2D1::Point2F(ctx.text_x, ctx.text_y), ctx.layout, cell_color, cell_brush });
    }
}

void CommandGenerator::GenTable(
    DrawCommandList& cmds,
    const FrameContext& fc,
    const Node& node, const NodeLayoutEntry& entry,
    int node_index, float offset_x, float entry_text_top, float h_scroll_x)
{
    const auto* tbl = node.table_data();
    if (!tbl || tbl->row_count == 0 || !entry.has_table_layout() || entry.table_layout->col_widths.empty()) {
        return;
    }

    const float cell_padding = TABLE_CELL_PADDING;
    const float border = TABLE_BORDER_WIDTH;
    const auto& tl = *entry.table_layout;
    const auto& selection = fc.selection;
    const float viewport_top = fc.viewport_top;
    const float viewport_bottom = fc.viewport_bottom;
    const auto row_count = tbl->row_count;
    const auto col_count = static_cast<size_t>(tbl->col_count);

    const float table_width = tl.cached_table_width;

    bool has_selection = selection.active && (node_index >= selection.start_node) && (node_index <= selection.end_node);
    uint32_t sel_start = 0, sel_end = 0;
    if (has_selection) {
        const auto range = selection.ClampedRange(node_index, tbl->concat_text.size());
        sel_start = range.start;
        sel_end = range.end;
        if (sel_end <= sel_start) {
            has_selection = false;
        }
    }

    float y = entry_text_top;
    size_t bg_cursor = 0;

    // 可視行帯を row_cum_y の二分探索で求め、その範囲だけループする
    // (ApplyTableEffects / FindTableRow と同じパターン)。巨大テーブルで
    // 毎フレーム row_count 回の continue ループが走るのを防ぐ。
    size_t r_begin = 0;
    size_t r_end = row_count;
    const bool has_row_geometry = tl.row_cum_y.size() == row_count + 1;
    if (has_row_geometry) {
        const auto [rb, re] = tl.VisibleRowRange(viewport_top - entry_text_top, viewport_bottom - entry_text_top);
        r_begin = rb;
        r_end = re;
        y = entry_text_top + tl.row_cum_y[r_begin];
        // bg リストは追記順が乱れうるため二分探索せず、既存セマンティクス
        // (前進スキップ) で r_begin 直前まで進める。サイズは bg 持ちセル数のみ。
        const uint32_t first_cell = static_cast<uint32_t>(r_begin * tl.col_count);
        while (bg_cursor < tl.cell_inline_code_bgs.size() && tl.cell_inline_code_bgs[bg_cursor].cell_index < first_cell) {
            ++bg_cursor;
        }
    }

    for (size_t r = r_begin; r < r_end; r++) {
        const float row_h = (r < tl.row_heights.size()) ? tl.row_heights[r] : (theme_->font_size_body * TABLE_ROW_HEIGHT_FACTOR);

        const float row_bottom = y + row_h + border;
        if (row_bottom < viewport_top || y > viewport_bottom) {
            y = row_bottom;
            if (!tl.cell_inline_code_bgs.empty() && bg_cursor < tl.cell_inline_code_bgs.size()) {
                const uint32_t next_cell = static_cast<uint32_t>((r + 1) * tl.col_count);
                while (bg_cursor < tl.cell_inline_code_bgs.size() && tl.cell_inline_code_bgs[bg_cursor].cell_index < next_cell) {
                    ++bg_cursor;
                }
            }
            continue;
        }

        const bool is_header_row = tbl->IsHeaderRow(r);
        GenTableRowBg(cmds, TableRowGeom{ offset_x, y, table_width, row_h, border }, is_header_row, r % 2 == 0);

        // 行上部の水平線
        cmds.emplace_back(DrawLineCmd{ D2D1::Point2F(offset_x, y), D2D1::Point2F(offset_x + table_width, y), theme_->hr_color, border, BrushId::Hr });

        // 可視列のみコマンド生成、画面外は cell_text_starts で flat_offset を直接取得。
        // 横スクロール時は画面に映る範囲が h_scroll_x だけ右にずれる。
        const float cull_left = offset_x + fc.viewport_left + h_scroll_x;
        const float cull_right = offset_x + fc.viewport_right + h_scroll_x;
        float cx = offset_x + border;
        const size_t drawn_cols = std::min(col_count, tl.col_widths.size());
        for (size_t c = 0; c < drawn_cols; c++) {
            const float cw = tl.col_widths[c];
            const float col_right = cx + cw + cell_padding * 2.0f;
            const bool col_visible = (col_right >= cull_left) && (cx - border <= cull_right);

            if (col_visible) {
                cmds.emplace_back(DrawLineCmd{ D2D1::Point2F(cx - border, y), D2D1::Point2F(cx - border, y + row_h + border), theme_->hr_color, border, BrushId::Hr });

                const float text_x = cx + cell_padding;
                const float text_y = y + cell_padding;
                IDWriteTextLayout* cell_layout = tl.GetCellLayout(r, c);

                // bg_cursor は cell_index 昇順で進む。
                bg_cursor = GenCellInlineCodeBgs(cmds, tl.cell_inline_code_bgs, bg_cursor, static_cast<uint32_t>(tl.CellIndex(r, c)), text_x, text_y, theme_->code_bg_color);

                GenSearchHighlights(cmds, entry, node_index, text_x, text_y, static_cast<int>(r), static_cast<int>(c));

                const auto cell_text = tbl->GetCellText(r, c);
                const uint32_t cell_flat = tbl->CellTextStart(r, c);
                GenTableCellContent(cmds, cell_text, CellDrawContext{
                    .layout = cell_layout,
                    .text_x = text_x,
                    .text_y = text_y,
                    .flat_offset = cell_flat,
                    .sel_start = sel_start,
                    .sel_end = sel_end,
                    .has_selection = has_selection,
                    .is_header = is_header_row,
                });
            }

            cx += cw + cell_padding * 2.0f + border;
        }
        cmds.emplace_back(DrawLineCmd{ D2D1::Point2F(offset_x + table_width, y), D2D1::Point2F(offset_x + table_width, y + row_h + border), theme_->hr_color, border, BrushId::Hr });
        y += row_h + border;
    }

    // r_end で打ち切った場合も下端線はテーブル全体の底に置く (可視外ならクリップされる)
    const float table_bottom = has_row_geometry ? entry_text_top + tl.row_cum_y[row_count] : y;
    cmds.emplace_back(DrawLineCmd{ D2D1::Point2F(offset_x, table_bottom), D2D1::Point2F(offset_x + table_width, table_bottom), theme_->hr_color, border, BrushId::Hr });
}
