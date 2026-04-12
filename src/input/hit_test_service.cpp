#include "hit_test_service.h"
#include "ui_constants.h"
#include <cassert>
#include <ranges>

namespace {

// 物理ピクセルをMDペインローカルのDIP座標に変換する。
struct PaneDip { float x, y; };
PaneDip ScreenToPaneDip(int screen_x, int screen_y,
    float dpi_scale, float md_pane_left, float scroll_y) noexcept
{
    return {
        screen_x / dpi_scale - md_pane_left,
        screen_y / dpi_scale + scroll_y
    };
}

// ノードのインデント幅を返す。
float NodeIndent(const Node& node, const Theme& theme) noexcept
{
    return node.indent_level * theme.indent_width;
}

} // namespace

// 指定された行・列までのフラットテキストオフセットを計算する。
// プリコンピュート済みのrow_flat_offsetsがあれば行スキャンを省略し、
// 対象行内のセルのみスキャンする（O(rows*cols) → O(cols)）。
static uint32_t ComputeTableFlatOffset(const Node& node, const NodeLayoutEntry& entry,
    int target_row, int target_col) noexcept
{
    const auto& rows = node.table_rows();

    // プリコンピュート済みオフセットを使用（レイアウト計算時に構築済み）
    if (entry.has_table_layout() && target_row >= 0 &&
        static_cast<size_t>(target_row) < entry.table_layout->row_flat_offsets.size()) {
        uint32_t offset = entry.table_layout->row_flat_offsets[target_row];
        const auto& row_cells = rows[target_row].cells;
        const auto col_count = row_cells.size();
        for (size_t c = 0; c < col_count && static_cast<int>(c) < target_col; c++) {
            offset += static_cast<uint32_t>(row_cells[c].text.size());
            if (c + 1 < col_count) {
                offset++;
            }
        }
        return offset;
    }

    // フォールバック: 全走査
    uint32_t offset = 0;
    const auto row_count = rows.size();
    for (size_t r = 0; r < row_count; r++) {
        const auto& row_cells = rows[r].cells;
        const auto col_count = row_cells.size();
        for (size_t c = 0; c < col_count; c++) {
            if (static_cast<int>(r) == target_row && static_cast<int>(c) == target_col) {
                return offset;
            }
            offset += static_cast<uint32_t>(row_cells[c].text.size());
            if (c + 1 < col_count) {
                offset++;
            }
        }
        if (r + 1 < row_count) {
            offset++;
        }
    }
    return static_cast<uint32_t>(node.GetText().size());
}

HitTestService::HitResult HitTestService::HitTest(
    const MdPaneHitContext& ctx) const noexcept
{
    HitResult result;
    if (ctx.nodes.empty()) {
        return result;
    }

    const auto [dip_x, dip_y] = ScreenToPaneDip(ctx.screen_x, ctx.screen_y, ctx.dpi_scale, ctx.md_pane_left, ctx.scroll_y);

    // dip_yを含むノードを二分探索で検索
    int lo = 0, hi = static_cast<int>(ctx.nodes.size()) - 1;
    int candidate = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (ctx.cache[mid].y_position <= dip_y) {
            candidate = mid;
            lo = mid + 1;
        }
        else {
            hi = mid - 1;
        }
    }

    if (candidate >= 0 && dip_y <= ctx.cache[candidate].y_position + ctx.cache[candidate].height) {
        const auto& node = ctx.nodes[candidate];
        const auto& entry = ctx.cache[candidate];

        if (node.type == NodeType::Table) {
            return HitTestTable(node, entry, candidate, ctx.theme, dip_x, dip_y);
        }

        if (entry.text_layout) {
            const float indent = NodeIndent(node, ctx.theme);
            const float local_x = dip_x - ctx.theme.margin_left - indent;
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
    for (const auto& [i, node] : ctx.nodes | std::views::enumerate | std::views::reverse) {
        if (const auto& text = node.GetText(); !text.empty()) {
            result.node_index = static_cast<int>(i);
            result.text_pos = static_cast<uint32_t>(text.size());
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

    const float indent = NodeIndent(node, theme);
    const float base_x = theme.margin_left + indent;
    const float cell_padding = TABLE_CELL_PADDING;
    const float border = TABLE_BORDER_WIDTH;

    if (!entry.has_table_layout()) {
        result.text_pos = static_cast<uint32_t>(node.GetText().size());
        return result;
    }
    const auto& tl = *entry.table_layout;

    // クリックされた行を特定
    float ry = entry.y_position;
    int hit_row = -1;
    const auto row_count = node.table_rows().size();
    for (size_t r = 0; r < row_count; r++) {
        const float row_h = (r < tl.row_heights.size()) ? tl.row_heights[r] : (theme.font_size_body * TABLE_ROW_HEIGHT_FACTOR);
        const float row_bottom = ry + row_h + border;
        if (dip_y < row_bottom) {
            hit_row = static_cast<int>(r);
            break;
        }
        ry += row_h + border;
    }
    if (hit_row < 0) {
        result.text_pos = static_cast<uint32_t>(node.GetText().size());
        return result;
    }

    // クリックされた列を特定
    float cx = base_x + border;
    int hit_col = static_cast<int>(tl.col_widths.size()) - 1; // デフォルトは最後の列
    const auto col_count = tl.col_widths.size();
    for (size_t c = 0; c < col_count; c++) {
        const float col_right = cx + tl.col_widths[c] + cell_padding * 2.0f;
        if (dip_x < col_right) {
            hit_col = static_cast<int>(c);
            break;
        }
        cx += tl.col_widths[c] + cell_padding * 2.0f + border;
    }
    if (hit_col < 0) {
        hit_col = 0;
    }

    // セル (hit_row, hit_col) のフラットテキストオフセットを計算
    const uint32_t flat_offset = ComputeTableFlatOffset(node, entry, hit_row, hit_col);

    // セルのテキストレイアウト内でヒットテスト
    const size_t r = static_cast<size_t>(hit_row);
    const size_t c = static_cast<size_t>(hit_col);
    IDWriteTextLayout* cell_layout = tl.GetCellLayout(r, c);
    if (cell_layout) {
        float cell_x = base_x + border;
        for (size_t cc = 0; cc < c; cc++) {
            cell_x += tl.col_widths[cc] + cell_padding * 2.0f + border;
        }
        const float cell_text_x = cell_x + cell_padding;

        float cell_y = entry.y_position;
        for (size_t rr = 0; rr < r; rr++) {
            const float rh = (rr < tl.row_heights.size()) ? tl.row_heights[rr] : (theme.font_size_body * TABLE_ROW_HEIGHT_FACTOR);
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
    const MdPaneHitContext& ctx) const noexcept
{
    assert(ctx.content_width > 0.0f && "content_width must be set for button hit test");
    assert(ctx.md_pane_height > 0.0f && "md_pane_height must be set for button hit test");
    if (ctx.nodes.empty()) {
        return -1;
    }

    const auto [dip_x, dip_y] = ScreenToPaneDip(ctx.screen_x, ctx.screen_y, ctx.dpi_scale, ctx.md_pane_left, ctx.scroll_y);

    // コピーボタンはコンテンツ右端にあるため、X座標で大半のマウス位置を早期棄却
    const float btn_left_bound = ctx.theme.margin_left + ctx.content_width - COPY_BTN_MARGIN - COPY_BTN_SIZE;
    if (dip_x < btn_left_bound) {
        return -1;
    }

    const float viewport_top = ctx.scroll_y;
    const float viewport_bottom = ctx.scroll_y + ctx.md_pane_height;

    // 可視範囲のコードブロックを検索
    const int first = FindFirstVisibleNodeIndex(ctx.cache, ctx.nodes.size(), viewport_top);
    const int count = static_cast<int>(ctx.nodes.size());
    for (int i = first; i < count; i++) {
        if (ctx.cache[i].y_position - ctx.theme.code_block_padding > viewport_bottom) {
            break;
        }

        const auto& node = ctx.nodes[i];
        if (node.type != NodeType::CodeBlock) {
            continue;
        }
        if (node.code_language == SyntaxLanguage::Mermaid) {
            continue;
        }

        const float indent = NodeIndent(node, ctx.theme);
        const float x = ctx.theme.margin_left + indent;
        const float w = ctx.content_width - indent;
        const float pad = ctx.theme.code_block_padding;

        const float block_right = x + w;
        const float block_top = ctx.cache[i].y_position - pad;

        const D2D1_RECT_F btn = OverlayButtonRect(block_right, block_top);
        if (dip_x >= btn.left && dip_x <= btn.right && dip_y >= btn.top && dip_y <= btn.bottom) {
            return i;
        }
    }
    return -1;
}

int HitTestService::SaveButtonHitTest(
    const MdPaneHitContext& ctx) const noexcept
{
    assert(ctx.content_width > 0.0f && "content_width must be set for button hit test");
    assert(ctx.md_pane_height > 0.0f && "md_pane_height must be set for button hit test");
    if (ctx.nodes.empty()) {
        return -1;
    }

    const auto [dip_x, dip_y] = ScreenToPaneDip(ctx.screen_x, ctx.screen_y, ctx.dpi_scale, ctx.md_pane_left, ctx.scroll_y);

    const float viewport_top = ctx.scroll_y;
    const float viewport_bottom = ctx.scroll_y + ctx.md_pane_height;

    const int first = FindFirstVisibleNodeIndex(ctx.cache, ctx.nodes.size(), viewport_top);
    const int count = static_cast<int>(ctx.nodes.size());
    for (int i = first; i < count; i++) {
        if (ctx.cache[i].y_position > viewport_bottom) {
            break;
        }

        const auto& node = ctx.nodes[i];
        if (node.type != NodeType::CodeBlock || node.code_language != SyntaxLanguage::Mermaid) {
            continue;
        }

        const auto& diagram = ctx.cache.GetDiagram(i);
        if (!diagram.bitmap) {
            continue;
        }

        const float indent = NodeIndent(node, ctx.theme);
        const float x = ctx.theme.margin_left + indent;
        const float cw = ctx.content_width - indent;

        const auto bmp = MermaidBitmapRect(diagram.width, diagram.height, x, cw, ctx.cache[i].y_position);
        const D2D1_RECT_F btn = OverlayButtonRect(bmp.right, bmp.top);
        if (dip_x >= btn.left && dip_x <= btn.right && dip_y >= btn.top && dip_y <= btn.bottom) {
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
