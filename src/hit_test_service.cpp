#include "hit_test_service.h"
#include "ui_constants.h"

// 指定された行・列までのフラットテキストオフセットを計算する。
static uint32_t ComputeTableFlatOffset(const Node& node, int target_row, int target_col)
{
    uint32_t offset = 0;
    for (size_t r = 0; r < node.table_rows().size(); r++) {
        const auto& row_cells = node.table_rows()[r].cells;
        for (size_t c = 0; c < row_cells.size(); c++) {
            if (static_cast<int>(r) == target_row && static_cast<int>(c) == target_col) {
                return offset;
            }
            offset += static_cast<uint32_t>(row_cells[c].text.size());
            if (c + 1 < row_cells.size()) {
                offset++;
            }
        }
        if (r + 1 < node.table_rows().size()) {
            offset++;
        }
    }
    return static_cast<uint32_t>(node.text.size());
}

HitTestService::HitResult HitTestService::HitTest(
    const std::pmr::vector<Node>& nodes,
    const LayoutCache& cache,
    const Theme& theme,
    float scroll_y,
    float md_pane_left,
    float dpi_scale,
    int screen_x, int screen_y) const noexcept
{

    HitResult result;
    if (nodes.empty()) {
        return result;
    }

    // 物理ピクセルをDIPに変換
    float dip_x = screen_x / dpi_scale;
    const float dip_y = screen_y / dpi_scale + scroll_y;

    // MDペインの位置でオフセット
    dip_x -= md_pane_left;

    // dip_yを含むノードを二分探索で検索
    int lo = 0, hi = static_cast<int>(nodes.size()) - 1;
    int candidate = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cache[mid].y_position <= dip_y) {
            candidate = mid;
            lo = mid + 1;
        }
        else {
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
            const float indent = node.indent_level * theme.indent_width;
            const float local_x = dip_x - theme.margin_left - indent;
            const float local_y = dip_y - entry.y_position;

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

    // 全ノードより下をクリック → 最後のノードの末尾を選択
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
    float dip_x, float dip_y) const noexcept
{

    HitResult result;
    result.node_index = node_index;

    const float indent = node.indent_level * theme.indent_width;
    const float base_x = theme.margin_left + indent;
    const float cell_padding = TABLE_CELL_PADDING;
    const float border = TABLE_BORDER_WIDTH;

    // クリックされた行を特定
    float ry = entry.y_position;
    int hit_row = -1;
    for (size_t r = 0; r < node.table_rows().size(); r++) {
        const float row_h = (r < entry.row_heights.size()) ? entry.row_heights[r] : (theme.font_size_body * 1.4f);
        const float row_bottom = ry + row_h + border;
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

    // クリックされた列を特定
    float cx = base_x + border;
    int hit_col = static_cast<int>(entry.col_widths.size()) - 1; // デフォルトは最後の列
    for (size_t c = 0; c < entry.col_widths.size(); c++) {
        const float col_right = cx + entry.col_widths[c] + cell_padding * 2.0f;
        if (dip_x < col_right) {
            hit_col = static_cast<int>(c);
            break;
        }
        cx += entry.col_widths[c] + cell_padding * 2.0f + border;
    }
    if (hit_col < 0) {
        hit_col = 0;
    }

    // セル (hit_row, hit_col) のフラットテキストオフセットを計算
    const uint32_t flat_offset = ComputeTableFlatOffset(node, hit_row, hit_col);

    // セルのテキストレイアウト内でヒットテスト
    const size_t r = static_cast<size_t>(hit_row);
    const size_t c = static_cast<size_t>(hit_col);
    IDWriteTextLayout* cell_layout = nullptr;
    if (r < entry.cell_layouts.size() && c < entry.cell_layouts[r].size()) {
        cell_layout = entry.cell_layouts[r][c].Get();
    }
    if (cell_layout) {
        float cell_x = base_x + border;
        for (size_t cc = 0; cc < c; cc++) {
            cell_x += entry.col_widths[cc] + cell_padding * 2.0f + border;
        }
        const float cell_text_x = cell_x + cell_padding;

        float cell_y = entry.y_position;
        for (size_t rr = 0; rr < r; rr++) {
            const float rh = (rr < entry.row_heights.size()) ? entry.row_heights[rr] : (theme.font_size_body * 1.4f);
            cell_y += rh + border;
        }
        const float cell_text_y = cell_y + cell_padding;

        BOOL is_trailing = FALSE, is_inside = FALSE;
        DWRITE_HIT_TEST_METRICS metrics{};
        cell_layout->HitTestPoint(
            dip_x - cell_text_x, dip_y - cell_text_y,
            &is_trailing, &is_inside, &metrics);

        result.text_pos = flat_offset + metrics.textPosition + (is_trailing ? 1 : 0);
    }
    else {
        result.text_pos = flat_offset;
    }
    return result;
}

int HitTestService::CopyButtonHitTest(
    const std::pmr::vector<Node>& nodes,
    const LayoutCache& cache,
    const Theme& theme,
    float scroll_y,
    float md_pane_left,
    float content_width,
    float md_pane_height,
    float dpi_scale,
    int screen_x, int screen_y) const noexcept
{
    if (nodes.empty()) {
        return -1;
    }

    // 物理ピクセルをDIPに変換（ドキュメント空間）
    const float dip_x = screen_x / dpi_scale - md_pane_left;
    const float dip_y = screen_y / dpi_scale + scroll_y;

    // コピーボタンはコンテンツ右端にあるため、X座標で大半のマウス位置を早期棄却
    const float btn_left_bound = theme.margin_left + content_width - COPY_BTN_MARGIN - COPY_BTN_SIZE;
    if (dip_x < btn_left_bound) {
        return -1;
    }

    const float viewport_top = scroll_y;
    const float viewport_bottom = scroll_y + md_pane_height;

    // 可視範囲のコードブロックを検索
    const int first = FindFirstVisibleNodeIndex(cache, nodes.size(), viewport_top);
    const int count = static_cast<int>(nodes.size());
    for (int i = first; i < count; i++) {
        if (cache[i].y_position - theme.code_block_padding > viewport_bottom) {
            break;
        }

        const auto& node = nodes[i];
        if (node.type != NodeType::CodeBlock) {
            continue;
        }
        if (node.code_language == SyntaxLanguage::Mermaid) {
            continue;
        }

        const float indent = node.indent_level * theme.indent_width;
        const float x = theme.margin_left + indent;
        const float w = content_width - indent;
        const float pad = theme.code_block_padding;

        const float block_right = x + w;
        const float block_top = cache[i].y_position - pad;

        const D2D1_RECT_F btn = CopyButtonRect(block_right, block_top);
        if (dip_x >= btn.left && dip_x <= btn.right &&
            dip_y >= btn.top && dip_y <= btn.bottom) {
            return i;
        }
    }
    return -1;
}

HitTestService::NavButtonHover HitTestService::NavButtonHitTest(
    float dip_x, float dip_y, const PaneRect& md_rect) const noexcept
{
    const float base_x = md_rect.x + md_rect.width - NAV_BTN_MARGIN - NAV_BTN_SIZE * 2 - NAV_BTN_GAP - NAV_BTN_SCROLLBAR_OFFSET;
    const float base_y = md_rect.y + md_rect.height - NAV_BTN_MARGIN - NAV_BTN_SIZE;

    if (dip_y < base_y || dip_y > base_y + NAV_BTN_SIZE) {
        return NavButtonHover::None;
    }

    // 戻るボタン
    if (dip_x >= base_x && dip_x <= base_x + NAV_BTN_SIZE) {
        return NavButtonHover::Back;
    }
    // 進むボタン
    const float fwd_x = base_x + NAV_BTN_SIZE + NAV_BTN_GAP;
    if (dip_x >= fwd_x && dip_x <= fwd_x + NAV_BTN_SIZE) {
        return NavButtonHover::Forward;
    }

    return NavButtonHover::None;
}
