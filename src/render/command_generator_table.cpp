#include "command_generator_internal.h"
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
        cmds.emplace_back(FillRectCmd{ rect, cached_stripe_color_, BrushId::TableStripe });
    }
}

void CommandGenerator::GenTableCellContent(DrawCommandList& cmds, std::string_view cell_text, const CellDrawContext& ctx)
{
    if (!ctx.layout) {
        return;
    }
    if (ctx.has_selection) {
        const uint32_t cell_len = static_cast<uint32_t>(cell_text.size());
        const uint32_t ov_start = std::max(ctx.sel_start, ctx.flat_offset);
        const uint32_t ov_end = std::min(ctx.sel_end, ctx.flat_offset + cell_len);
        if (ov_end > ov_start) {
            // sel_start/sel_end は UTF-8 byte offset。HitTestTextRange 用に UTF-16 へ変換する。
            mendo::Utf16OffsetCursor cursor{ cell_text };
            const auto range = cursor.WideRange(ov_start - ctx.flat_offset, ov_end - ov_start);
            if (range.length > 0) {
                GenSelectionHighlight(cmds, ctx.layout, range.startPosition, range.length, ctx.text_x, ctx.text_y);
            }
        }
    }
    const D2D1_COLOR_F cell_color = ctx.is_header ? theme_->heading_color : theme_->text_color;
    const BrushId cell_brush = ctx.is_header ? BrushId::Heading : BrushId::Text;
    cmds.emplace_back(MakeTextLayoutCmd(D2D1::Point2F(ctx.text_x, ctx.text_y), ctx.layout, ctx.height, cell_color, cell_brush, cull_top_, cull_bottom_));
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
    const float viewport_top = cull_top_;
    const float viewport_bottom = cull_bottom_;
    const auto row_count = tbl->row_count;
    const auto col_count = static_cast<size_t>(tbl->col_count);

    const float table_width = tl.cached_table_width;
    // 横スクロール時は画面に映る範囲が h_scroll_x だけ右にずれる。
    const float cull_left = offset_x + fc.viewport_left + h_scroll_x;
    const float cull_right = offset_x + fc.viewport_right + h_scroll_x;
    const size_t drawn_cols = std::min(col_count, tl.col_widths.size());

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

    // cell_inline_code_bgs は cell_index 昇順 (renderer.cpp の追記/rotate が維持) なので二分探索で進める。
    const auto seek_bg = [&bgs = tl.cell_inline_code_bgs](uint32_t cell) noexcept {
        return static_cast<size_t>(std::ranges::lower_bound(bgs, cell, {}, &CellInlineCodeBg::cell_index) - bgs.begin());
    };

    // 可視行帯を row_cum_y の二分探索で求め、その範囲だけループする
    // (ApplyTableEffects / FindTableRow と同じパターン)。巨大テーブルで
    // 毎フレーム row_count 回の continue ループが走るのを防ぐ。
    const bool has_row_geometry = tl.HasRowGeometry(row_count);
    const auto [r_begin, r_end] = tl.RowsInViewport(row_count, viewport_top - entry_text_top, viewport_bottom - entry_text_top);
    float y = has_row_geometry ? entry_text_top + tl.row_cum_y[r_begin] : entry_text_top;
    size_t bg_cursor = seek_bg(static_cast<uint32_t>(r_begin * tl.col_count));

    // x は列順に単調なので、可視列は連続した区間 [c_begin, c_end) になる。
    const auto col_step = [&](size_t c) noexcept { return tl.col_widths[c] + cell_padding * 2.0f + border; };
    size_t c_begin = 0;
    float x_begin = offset_x + border;
    while (c_begin < drawn_cols && x_begin + tl.col_widths[c_begin] + cell_padding * 2.0f < cull_left) {
        x_begin += col_step(c_begin);
        ++c_begin;
    }
    size_t c_end = c_begin;
    for (float x = x_begin; c_end < drawn_cols && x - border <= cull_right; ++c_end) {
        x += col_step(c_end);
    }

    for (size_t r = r_begin; r < r_end; r++) {
        const float row_h = (r < tl.row_heights.size()) ? tl.row_heights[r] : (theme_->font_size_body * TABLE_ROW_HEIGHT_FACTOR);

        const float row_bottom = y + row_h + border;
        if (row_bottom < viewport_top || y > viewport_bottom) {
            y = row_bottom;
            bg_cursor = seek_bg(static_cast<uint32_t>((r + 1) * tl.col_count));
            continue;
        }

        const bool is_header_row = tbl->IsHeaderRow(r);
        GenTableRowBg(cmds, TableRowGeom{ offset_x, y, table_width, row_h, border }, is_header_row, r % 2 == 0);

        // 行上部の水平線
        cmds.emplace_back(DrawLineCmd{ D2D1::Point2F(offset_x, y), D2D1::Point2F(offset_x + table_width, y), theme_->hr_color, border, BrushId::Hr });

        // 可視列のみコマンド生成、画面外は cell_text_starts で flat_offset を直接取得。
        float cx = x_begin;
        for (size_t c = c_begin; c < c_end; c++) {
            const float text_x = cx + cell_padding;
            const float text_y = y + cell_padding;

            // bg_cursor は cell_index 昇順で進む。
            bg_cursor = GenCellInlineCodeBgs(cmds, tl.cell_inline_code_bgs, bg_cursor, static_cast<uint32_t>(tl.CellIndex(r, c)), text_x, text_y, theme_->code_bg_color);

            GenSearchHighlights(cmds, entry, node_index, text_x, text_y, static_cast<int>(r), static_cast<int>(c));

            GenTableCellContent(cmds, tbl->GetCellText(r, c), CellDrawContext{
                .layout = tl.GetCellLayout(r, c),
                .text_x = text_x,
                .text_y = text_y,
                .height = row_h,
                .flat_offset = tbl->CellTextStart(r, c),
                .sel_start = sel_start,
                .sel_end = sel_end,
                .has_selection = has_selection,
                .is_header = is_header_row,
            });
            cx += col_step(c);
        }
        y += row_h + border;
    }

    // r_end で打ち切った場合も下端線はテーブル全体の底に置く (可視外ならクリップされる)
    const float table_bottom = has_row_geometry ? entry_text_top + tl.row_cum_y[row_count] : y;
    cmds.emplace_back(DrawLineCmd{ D2D1::Point2F(offset_x, table_bottom), D2D1::Point2F(offset_x + table_width, table_bottom), theme_->hr_color, border, BrushId::Hr });

    // 縦罫線は可視列ごとに表の上端から下端まで 1 本で引く (行ごとに引くと 行×列 本になる)。可視外はクリップされる。
    const auto vline = [&](float x) {
        cmds.emplace_back(DrawLineCmd{ D2D1::Point2F(x, entry_text_top), D2D1::Point2F(x, table_bottom), theme_->hr_color, border, BrushId::Hr });
    };
    float cx = x_begin;
    for (size_t c = c_begin; c < c_end; c++) {
        vline(cx - border);
        cx += col_step(c);
    }
    vline(offset_x + table_width);
}
