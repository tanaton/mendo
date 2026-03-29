#include "command_generator.h"
#include "ui_constants.h"
#include <algorithm>
#include <format>

static float GetFirstLineHeight(IDWriteTextLayout* layout, float font_size)
{
    float h = font_size * 1.3f;
    if (layout) {
        DWRITE_LINE_METRICS lm;
        UINT32 lc;
        if (SUCCEEDED(layout->GetLineMetrics(&lm, 1, &lc)) && lc > 0) {
            h = lm.height;
        }
    }
    return h;
}

static void GenInlineCodeBgs(DrawCommandList& cmds,
    const std::pmr::vector<InlineCodeBg>& bgs,
    float origin_x, float origin_y, D2D1_COLOR_F color)
{
    for (const auto& bg : bgs) {
        D2D1_RECT_F rect = D2D1::RectF(
            origin_x + bg.left - 3.0f,
            origin_y + bg.top - 2.0f,
            origin_x + bg.left + bg.width + 3.0f,
            origin_y + bg.top + bg.height + 2.0f);
        cmds.push_back(FillRoundedRectCmd{ rect, 3.0f, 3.0f, color });
    }
}

const DrawCommandList& CommandGenerator::GenerateMdPane(
    const std::pmr::vector<Node>& nodes, const LayoutCache& cache,
    const PaneRect& md_pane_rect, float scroll_y,
    const TextSelection& selection,
    int first_visible,
    int hovered_copy_node,
    float dpi_scale)
{
    // 古いコマンドリストを破棄し、monotonic リソースをリセットして再利用する。
    // clear() で要素を破棄してから Reset() を呼び、新しいリストを作成する。
    // monotonic_buffer_resource::deallocate は no-op のため、
    // clear() 後のバッファポインタが残っていても安全。
    cmds_.clear();
    frame_resource_.Reset();
    cmds_ = DrawCommandList{ frame_resource_.resource() };
    auto& cmds = cmds_;

    // MDペインの境界でクリップ
    D2D1_RECT_F md_clip = D2D1::RectF(
        md_pane_rect.x, md_pane_rect.y,
        md_pane_rect.x + md_pane_rect.width, md_pane_rect.y + md_pane_rect.height);
    cmds.push_back(PushClipCmd{ md_clip });
    // スクロール位置を物理ピクセル境界にスナップし、ClearTypeヒンティングの
    // フレーム間変動によるテキストのガタつきを防止する。
    // viewport bounds にはスナップ前の scroll_y を使い、ヒットテストとの座標一致を保つ。
    float snapped_y = SnapScrollToPixel(scroll_y, dpi_scale);
    cmds.push_back(SetTransformCmd{ D2D1::Matrix3x2F::Translation(md_pane_rect.x, -snapped_y) });

    float viewport_top = scroll_y;
    float viewport_bottom = scroll_y + md_pane_rect.height;
    float offset_x = theme_->margin_left;
    float md_content_width = md_pane_rect.width - theme_->margin_left - theme_->margin_right;

    // 最初の可視ノードを二分探索で検索（事前計算済みのインデックスがあればそれを使用）
    int node_count = static_cast<int>(nodes.size());
    if (first_visible < 0) {
        first_visible = FindFirstVisibleNodeIndex(cache, nodes.size(), viewport_top);
    }

    // 同一 blockquote_group のノードをまとめてバー/背景を描画する。
    // ノード単体ごとではなく一括で描画することで、複数行の引用ブロックが連続した見た目になる。
    GenBlockQuoteGroupDecorations(cmds, nodes, cache, node_count,
        offset_x, md_content_width, first_visible, viewport_bottom);

    for (int i = first_visible; i < node_count; i++) {
        if (cache[i].y_position > viewport_bottom) {
            break;
        }
        GenerateNode(cmds, nodes[i], cache[i], cache.GetDiagram(i),
            i, offset_x, viewport_top, viewport_bottom,
            selection, md_content_width, hovered_copy_node);
    }

    cmds.push_back(SetTransformCmd{ D2D1::Matrix3x2F::Identity() });
    cmds.push_back(PopClipCmd{});
    return cmds_;
}

void CommandGenerator::GenerateNode(DrawCommandList& cmds,
    const Node& node, const NodeLayoutEntry& entry, const DiagramEntry& diagram,
    int node_index, float offset_x, float viewport_top, float viewport_bottom,
    const TextSelection& selection, float content_width,
    int hovered_copy_node)
{
    // ビューポート外のノードをカリング
    // h1/h2は見出し下線がentry.heightの外に描画されるため、カリング境界を拡張する。
    float node_bottom = entry.y_position + entry.height;
    if (node.type == NodeType::Heading && node.heading_level <= 2) {
        node_bottom += theme_->heading_spacing_below * 0.5f + theme_->GetHeadingUnderlineThickness(node.heading_level);
    }
    if (node_bottom < viewport_top || entry.y_position > viewport_bottom) {
        return;
    }

    float indent = node.indent_level * theme_->indent_width;
    float x = offset_x + indent;
    float cw = content_width - indent;

    switch (node.type) {
    case NodeType::HorizontalRule:
        GenHorizontalRule(cmds, entry, x, cw);
        return;

    case NodeType::Image:
        if (diagram.bitmap) {
            float draw_h = entry.height;
            float draw_w = (diagram.height > 0)
                ? diagram.width * (draw_h / diagram.height)
                : diagram.width;
            float dx = x;
            cmds.push_back(DrawBitmapCmd{
                diagram.bitmap.Get(),
                D2D1::RectF(dx, entry.y_position, dx + draw_w, entry.y_position + draw_h) });
        }
        else {
            GenDiagramPlaceholder(cmds, x, entry.y_position, cw, entry.height);
        }
        return;

    case NodeType::Table:
        GenTable(cmds, node, entry, node_index, x, selection, viewport_top, viewport_bottom);
        return;

    case NodeType::CodeBlock:
        if (node.code_language == SyntaxLanguage::Mermaid) {
            if (diagram.bitmap) {
                float draw_w = diagram.width;
                float draw_h = diagram.height;
                if (draw_w > cw && draw_w > 0) {
                    float scale = cw / draw_w;
                    draw_h *= scale;
                    draw_w = cw;
                }
                float dx = x + (cw - draw_w) * 0.5f;
                cmds.push_back(DrawBitmapCmd{
                    diagram.bitmap.Get(),
                    D2D1::RectF(dx, entry.y_position, dx + draw_w, entry.y_position + draw_h) });
            }
            else {
                GenDiagramPlaceholder(cmds, x, entry.y_position, cw, entry.height);
            }
            return;
        }
        GenCodeBlockBg(cmds, entry, x, cw);
        GenCopyButton(cmds, entry, x, cw, node_index == hovered_copy_node);
        break;

    case NodeType::ListItem:
        GenListBullet(cmds, node, entry, x);
        break;
    case NodeType::TaskListItem:
        break;
    case NodeType::BlockQuote:
        // バーと背景はグループ単位で GenBlockQuoteGroupDecorations から描画済み
        break;
    case NodeType::Heading:
        if (node.heading_level <= 2) {
            float line_y = entry.y_position + entry.height + theme_->heading_spacing_below * 0.5f;
            cmds.push_back(DrawLineCmd{
                D2D1::Point2F(x, line_y), D2D1::Point2F(x + cw, line_y),
                theme_->hr_color, theme_->GetHeadingUnderlineThickness(node.heading_level) });
        }
        break;
    default:
        break;
    }

    if (!entry.text_layout) {
        return;
    }

    // ノードタイプに応じたベースカラーを決定
    D2D1_COLOR_F base_color = theme_->text_color;
    if (node.type == NodeType::Heading) {
        base_color = theme_->heading_color;
    }
    else if (node.type == NodeType::BlockQuote) {
        base_color = (node.alert_type != AlertType::None)
            ? theme_->text_color
            : theme_->blockquote_text_color;
    }
    else if (node.type == NodeType::CodeBlock) {
        base_color = theme_->code_text_color;
    }

    // インラインコードの背景
    GenInlineCodeBgs(cmds, entry.inline_code_bgs, x, entry.y_position, theme_->code_bg_color);

    // 選択範囲のハイライト
    if (selection.active &&
        node_index >= selection.start_node && node_index <= selection.end_node) {
        uint32_t sel_start = 0;
        uint32_t sel_end = static_cast<uint32_t>(node.text.size());
        if (node_index == selection.start_node) {
            sel_start = selection.start_pos;
        }
        if (node_index == selection.end_node) {
            sel_end = selection.end_pos;
        }
        if (sel_end > sel_start) {
            GenSelectionHighlight(cmds, entry.text_layout.Get(),
                sel_start, sel_end - sel_start, x, entry.y_position);
        }
    }

    // メインテキスト
    cmds.push_back(DrawTextLayoutCmd{ D2D1::Point2F(x, entry.y_position),
                                      entry.text_layout.Get(), base_color });

    // タスクリストのチェックボックス
    if (node.type == NodeType::TaskListItem && formats_.icon_font) {
        const wchar_t icon = node.task_checked ? L'\u2611' : L'\u2610'; // ☑ / ☐
        float icon_size = theme_->font_size_body;
        float cb_x = x - theme_->list_bullet_offset;
        cmds.push_back(DrawTextCmd::Make(
            &icon, 1,
            D2D1::RectF(cb_x, entry.y_position, cb_x + icon_size, entry.y_position + icon_size * 1.5f),
            formats_.icon_font,
            theme_->text_color));
    }
}

// ---- サブジェネレータ ----

void CommandGenerator::GenHorizontalRule(DrawCommandList& cmds,
    const NodeLayoutEntry& entry, float x, float w)
{
    float y = entry.y_position + theme_->paragraph_spacing * 0.5f;
    cmds.push_back(DrawLineCmd{
        D2D1::Point2F(x, y), D2D1::Point2F(x + w, y),
        theme_->hr_color, theme_->hr_thickness });
}

void CommandGenerator::GenTableRowBg(DrawCommandList& cmds, bool is_header, bool is_even_row,
    float x, float y, float table_width, float row_h, float border)
{
    if (is_header) {
        cmds.push_back(FillRectCmd{
            D2D1::RectF(x, y, x + table_width, y + row_h + border),
            theme_->code_bg_color });
    }
    else if (is_even_row) {
        cmds.push_back(FillRectCmd{
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
        uint32_t cell_len = static_cast<uint32_t>(cell.text.size());
        uint32_t ov_start = std::max(sel_start, flat_offset);
        uint32_t ov_end = std::min(sel_end, flat_offset + cell_len);
        if (ov_end > ov_start) {
            GenSelectionHighlight(cmds, cell_layout,
                ov_start - flat_offset, ov_end - ov_start, text_x, text_y);
        }
    }
    if (cell_layout) {
        D2D1_COLOR_F cell_color = cell.is_header ? theme_->heading_color : theme_->text_color;
        cmds.push_back(DrawTextLayoutCmd{ D2D1::Point2F(text_x, text_y), cell_layout, cell_color });
    }
}

void CommandGenerator::GenTable(DrawCommandList& cmds,
    const Node& node, const NodeLayoutEntry& entry,
    int node_index, float offset_x, const TextSelection& selection,
    float viewport_top, float viewport_bottom)
{
    if (node.table_rows.empty() || entry.col_widths.empty()) {
        return;
    }

    float cell_padding = TABLE_CELL_PADDING;
    float border = TABLE_BORDER_WIDTH;

    float table_width = border;
    for (float cw : entry.col_widths) {
        table_width += cw + cell_padding * 2.0f + border;
    }

    bool has_selection = selection.active &&
        node_index >= selection.start_node && node_index <= selection.end_node;
    uint32_t sel_start = 0, sel_end = static_cast<uint32_t>(node.text.size());
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
    auto advance_flat_offset = [](uint32_t& offset, const TableRow& row, size_t from, size_t to) {
        for (size_t c = from; c < to; c++) {
            offset += static_cast<uint32_t>(row.cells[c].text.size());
            if (c + 1 < row.cells.size()) {
                offset++;
            }
        }
    };

    float y = entry.y_position;
    uint32_t flat_offset = 0;

    for (size_t r = 0; r < node.table_rows.size(); r++) {
        const auto& row = node.table_rows[r];
        float row_h = (r < entry.row_heights.size()) ? entry.row_heights[r] : (theme_->font_size_body * 1.4f);

        float row_bottom = y + row_h + border;
        if (row_bottom < viewport_top || y > viewport_bottom) {
            advance_flat_offset(flat_offset, row, 0, row.cells.size());
            y = row_bottom;
            if (r + 1 < node.table_rows.size()) {
                flat_offset++;
            }
            continue;
        }

        bool is_header_row = (!row.cells.empty() && row.cells[0].is_header);
        GenTableRowBg(cmds, is_header_row, r % 2 == 0, offset_x, y, table_width, row_h, border);

        // 行上部の水平線
        cmds.push_back(DrawLineCmd{
            D2D1::Point2F(offset_x, y), D2D1::Point2F(offset_x + table_width, y),
            theme_->hr_color, border });

        // セルを描画
        float cx = offset_x + border;
        size_t drawn_cols = std::min(row.cells.size(), entry.col_widths.size());
        for (size_t c = 0; c < drawn_cols; c++) {
            const auto& cell = row.cells[c];
            float cw = entry.col_widths[c];

            // 垂直の罫線
            cmds.push_back(DrawLineCmd{
                D2D1::Point2F(cx - border, y), D2D1::Point2F(cx - border, y + row_h + border),
                theme_->hr_color, border });

            float text_x = cx + cell_padding;
            float text_y = y + cell_padding;

            IDWriteTextLayout* cell_layout = nullptr;
            if (r < entry.cell_layouts.size() && c < entry.cell_layouts[r].size()) {
                cell_layout = entry.cell_layouts[r][c].Get();
            }

            // セルのインラインコード背景
            if (r < entry.cell_inline_code_bgs.size() && c < entry.cell_inline_code_bgs[r].size()) {
                GenInlineCodeBgs(cmds, entry.cell_inline_code_bgs[r][c],
                    text_x, text_y, theme_->code_bg_color);
            }

            GenTableCellContent(cmds, cell, cell_layout, text_x, text_y,
                has_selection, sel_start, sel_end, flat_offset);

            flat_offset += static_cast<uint32_t>(cell.text.size());
            if (c + 1 < row.cells.size()) {
                flat_offset++;
            }
            cx += cw + cell_padding * 2.0f + border;
        }

        advance_flat_offset(flat_offset, row, drawn_cols, row.cells.size());

        // 右罫線
        cmds.push_back(DrawLineCmd{
            D2D1::Point2F(offset_x + table_width, y),
            D2D1::Point2F(offset_x + table_width, y + row_h + border),
            theme_->hr_color, border });

        y += row_h + border;
        if (r + 1 < node.table_rows.size()) {
            flat_offset++;
        }
    }

    // 下罫線
    cmds.push_back(DrawLineCmd{
        D2D1::Point2F(offset_x, y), D2D1::Point2F(offset_x + table_width, y),
        theme_->hr_color, border });
}

void CommandGenerator::GenCodeBlockBg(DrawCommandList& cmds,
    const NodeLayoutEntry& entry, float x, float w)
{
    float pad = theme_->code_block_padding;
    D2D1_RECT_F bg_rect = D2D1::RectF(
        x - pad, entry.y_position - pad,
        x + w, entry.y_position + entry.height + pad);
    cmds.push_back(FillRoundedRectCmd{ bg_rect, 4.0f, 4.0f, theme_->code_bg_color });
}

void CommandGenerator::GenCopyButton(DrawCommandList& cmds,
    const NodeLayoutEntry& entry, float x, float w, bool is_hovered)
{
    if (!formats_.copy_btn_icon) {
        return;
    }

    float pad = theme_->code_block_padding;
    D2D1_RECT_F btn = CopyButtonRect(x + w, entry.y_position - pad);

    float bg_alpha = is_hovered
        ? (cached_is_dark_ ? 0.30f : 0.15f)
        : (cached_is_dark_ ? 0.10f : 0.05f);
    D2D1_COLOR_F bg_color = cached_is_dark_
        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, bg_alpha)
        : D2D1::ColorF(0.0f, 0.0f, 0.0f, bg_alpha);
    cmds.push_back(FillRoundedRectCmd{ btn, COPY_BTN_CORNER, COPY_BTN_CORNER, bg_color });

    float text_alpha = is_hovered
        ? (cached_is_dark_ ? 0.9f : 0.8f)
        : (cached_is_dark_ ? 0.4f : 0.35f);
    D2D1_COLOR_F icon_color = cached_is_dark_
        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, text_alpha)
        : D2D1::ColorF(0.0f, 0.0f, 0.0f, text_alpha);
    const wchar_t icon = L'\uE8C8';
    cmds.push_back(DrawTextCmd::Make(&icon, 1, btn, formats_.copy_btn_icon, icon_color));
}

void CommandGenerator::GenListBullet(DrawCommandList& cmds,
    const Node& node, const NodeLayoutEntry& entry, float x)
{
    if (node.list_number > 0) {
        // 順序付きリストの番号（1行目に合わせて配置）
        if (formats_.list_number) {
            float first_line_h = GetFirstLineHeight(entry.text_layout.Get(), theme_->font_size_body);
            wchar_t num_buf[DrawTextCmd::MAX_TEXT];
            auto fmt_result = std::format_to_n(num_buf, DrawTextCmd::MAX_TEXT, L"{}.", node.list_number);
            size_t num_len = std::min(static_cast<size_t>(fmt_result.size), static_cast<size_t>(DrawTextCmd::MAX_TEXT));
            D2D1_RECT_F num_rect = D2D1::RectF(
                x - theme_->list_bullet_offset - 8.0f,
                entry.y_position,
                x - 4.0f,
                entry.y_position + first_line_h);
            cmds.push_back(DrawTextCmd::Make(num_buf, num_len, num_rect, formats_.list_number, theme_->text_color));
        }
    }
    else {
        // 順序なしリストの箇条書き記号（1行目の中央に配置）
        float first_line_h = GetFirstLineHeight(entry.text_layout.Get(), theme_->font_size_body);
        float bullet_y = entry.y_position + first_line_h * 0.5f;
        float bullet_x = x - theme_->list_bullet_offset * 0.6f;
        float r = 3.0f;
        if (node.indent_level <= 1) {
            cmds.push_back(FillEllipseCmd{ D2D1::Point2F(bullet_x, bullet_y), r, r, theme_->text_color });
        }
        else {
            cmds.push_back(DrawEllipseCmd{ D2D1::Point2F(bullet_x, bullet_y), r, r, theme_->text_color, 1.0f });
        }
    }
}

void CommandGenerator::GenBlockQuoteGroupDecorations(DrawCommandList& cmds,
    const std::pmr::vector<Node>& nodes, const LayoutCache& cache,
    int node_count, float offset_x, float content_width,
    int first_visible, float viewport_bottom)
{
    static constexpr float BAR_EXTEND = 2.0f;
    static constexpr float ALERT_BG_PAD = 4.0f;
    static constexpr float ALERT_BG_CORNER = 4.0f;

    // first_visible がグループ途中の場合、グループ先頭まで遡る
    int i = first_visible;
    if (i < node_count && nodes[i].blockquote_group >= 0) {
        int group = nodes[i].blockquote_group;
        while (i > 0 && nodes[i - 1].blockquote_group == group) {
            i--;
        }
    }

    while (i < node_count) {
        int group = nodes[i].blockquote_group;
        if (group < 0) {
            i++;
            continue;
        }
        if (cache[i].y_position > viewport_bottom) {
            break;
        }

        // グループの範囲を特定
        float group_top = cache[i].y_position;
        float group_bottom = cache[i].y_position + cache[i].height;
        AlertType alert_type = nodes[i].alert_type;
        int bar_indent = -1;
        if (nodes[i].type == NodeType::BlockQuote) {
            bar_indent = nodes[i].indent_level;
        }

        int j = i + 1;
        while (j < node_count && nodes[j].blockquote_group == group) {
            float bottom = cache[j].y_position + cache[j].height;
            if (bottom > group_bottom) {
                group_bottom = bottom;
            }
            if (bar_indent < 0 && nodes[j].type == NodeType::BlockQuote) {
                bar_indent = nodes[j].indent_level;
            }
            j++;
        }

        if (bar_indent < 0) {
            // BlockQuote ノードがないグループはスキップ
            i = j;
            continue;
        }

        float indent = static_cast<float>(bar_indent) * theme_->indent_width;
        float x = offset_x + indent;
        float bar_x = x - theme_->indent_width * 0.5f;

        if (alert_type != AlertType::None) {
            auto idx = AlertColorIndex(alert_type);
            if (idx < ALERT_TYPE_COUNT) {
                float cw = content_width - indent;
                D2D1_RECT_F bg_rect = D2D1::RectF(
                    x - ALERT_BG_PAD, group_top - ALERT_BG_PAD,
                    x + cw, group_bottom + ALERT_BG_PAD);
                cmds.push_back(FillRoundedRectCmd{ bg_rect, ALERT_BG_CORNER, ALERT_BG_CORNER,
                    theme_->alert_bg_color[idx] });
                cmds.push_back(DrawLineCmd{
                    D2D1::Point2F(bar_x, group_top - BAR_EXTEND),
                    D2D1::Point2F(bar_x, group_bottom + BAR_EXTEND),
                    theme_->alert_color[idx], theme_->blockquote_bar_width });
            }
        }
        else {
            cmds.push_back(DrawLineCmd{
                D2D1::Point2F(bar_x, group_top - BAR_EXTEND),
                D2D1::Point2F(bar_x, group_bottom + BAR_EXTEND),
                theme_->blockquote_bar_color, theme_->blockquote_bar_width });
        }

        i = j;
    }
}

void CommandGenerator::GenDiagramPlaceholder(DrawCommandList& cmds,
    float x, float y, float w, float h)
{
    D2D1_RECT_F bg = D2D1::RectF(x, y, x + w, y + h);
    cmds.push_back(FillRoundedRectCmd{ bg, 4.0f, 4.0f, theme_->code_bg_color });
    if (formats_.placeholder_text) {
        constexpr wchar_t kText[] = L"Loading...";
        cmds.push_back(DrawTextCmd::Make(kText, static_cast<UINT32>(std::ranges::size(kText) - 1), bg, formats_.placeholder_text, theme_->blockquote_text_color));
    }
}

void CommandGenerator::GenSelectionHighlight(DrawCommandList& cmds,
    IDWriteTextLayout* layout, uint32_t start, uint32_t length,
    float origin_x, float origin_y)
{
    if (!layout || length == 0) {
        return;
    }
    UINT32 count = FetchHitTestMetrics(layout, start, length, hit_test_buffer_);
    if (count == 0) {
        return;
    }
    for (UINT32 i = 0; i < count; i++) {
        cmds.push_back(FillRectCmd{
            D2D1::RectF(
                origin_x + hit_test_buffer_[i].left,
                origin_y + hit_test_buffer_[i].top,
                origin_x + hit_test_buffer_[i].left + hit_test_buffer_[i].width,
                origin_y + hit_test_buffer_[i].top + hit_test_buffer_[i].height),
            SELECTION_COLOR });
    }
}
