#include "hit_test_service.h"
#include "layout.h"
#include "ui_constants.h"
#include <cassert>
#include <ranges>
#include <utility>

namespace {

// ノードのインデント幅を返す。
float NodeIndent(const Node& node, const Theme& theme) noexcept
{
    return node.indent_level * theme.indent_width;
}

// テーブル内のクリック座標からヒットした行を特定する。
// 見つかった場合は行インデックスとその行の上端Y座標を返す。
struct TableRowHit { int row; float row_top_y; };
TableRowHit FindTableRow(const Node& node, const NodeLayoutEntry& entry,
    const Theme& theme, float dip_y) noexcept
{
    if (!entry.has_table_layout()) {
        return { -1, 0.0f };
    }
    const auto& tl = *entry.table_layout;
    const auto row_count = node.table_rows().size();
    if (row_count == 0) {
        return { -1, 0.0f };
    }

    // 事前計算された累積Y配列で二分探索。
    // row_cum_y[r] = エントリ上端からの行 r の上端までの累積高さ（サイズ row_count+1）。
    if (tl.row_cum_y.size() == row_count + 1) {
        const float local_y = dip_y - entry.y_position;
        const auto it = std::ranges::upper_bound(tl.row_cum_y, local_y);
        if (it == tl.row_cum_y.begin()) {
            return { -1, 0.0f };
        }
        const auto idx = std::ranges::distance(tl.row_cum_y.begin(), it) - 1;
        if (static_cast<size_t>(idx) >= row_count) {
            return { -1, 0.0f };
        }
        return { static_cast<int>(idx), entry.y_position + tl.row_cum_y[static_cast<size_t>(idx)] };
    }

    // フォールバック: 線形走査
    const float border = TABLE_BORDER_WIDTH;
    float ry = entry.y_position;
    for (size_t r = 0; r < row_count; r++) {
        const float row_h = (r < tl.row_heights.size()) ? tl.row_heights[r] : (theme.font_size_body * TABLE_ROW_HEIGHT_FACTOR);
        const float row_bottom = ry + row_h + border;
        if (dip_y < row_bottom) {
            return { static_cast<int>(r), ry };
        }
        ry += row_h + border;
    }
    return { -1, 0.0f };
}

// テーブル内のクリック座標からヒットした列を特定する。
// 見つからない場合は最終列を返す（列数0の場合は0を返す）。
// cell_left_x にはヒットしたセルの左端X座標（パディング含まず）を書き込む。
int FindTableCol(const TableLayoutData& tl, float base_x, float dip_x,
    float& cell_left_x) noexcept
{
    const auto col_count = tl.col_widths.size();

    // 事前計算された累積X配列で二分探索。
    // col_cum_x[c] = base_x からの列 c の左端までの累積幅（サイズ col_count+1）。
    if (col_count > 0 && tl.col_cum_x.size() == col_count + 1) {
        const float local_x = dip_x - base_x;
        auto it = std::ranges::upper_bound(tl.col_cum_x, local_x);
        if (it == tl.col_cum_x.begin()) {
            ++it; // base_x + border より左の場合は最初の列にクランプ
        }
        size_t idx = static_cast<size_t>(std::ranges::distance(tl.col_cum_x.begin(), it) - 1);
        if (idx >= col_count) {
            idx = col_count - 1; // 最終列にクランプ
        }
        cell_left_x = base_x + tl.col_cum_x[idx];
        return static_cast<int>(idx);
    }

    // フォールバック: 線形走査
    const float cell_padding = TABLE_CELL_PADDING;
    const float border = TABLE_BORDER_WIDTH;
    float cx = base_x + border;
    for (size_t c = 0; c < col_count; c++) {
        const float col_right = cx + tl.col_widths[c] + cell_padding * 2.0f;
        if (dip_x < col_right) {
            cell_left_x = cx;
            return static_cast<int>(c);
        }
        cx += tl.col_widths[c] + cell_padding * 2.0f + border;
    }
    if (col_count > 0) {
        cell_left_x = cx - tl.col_widths[col_count - 1] - cell_padding * 2.0f - border;
        return static_cast<int>(col_count - 1);
    }
    cell_left_x = base_x + border;
    return 0;
}

} // namespace

HitTestService::HitResult HitTestService::HitTest(
    const MdPaneHitContext& ctx) const noexcept
{
    HitResult result;
    if (ctx.nodes.empty()) {
        return result;
    }

    const uint32_t gen = ctx.cache.GetEffectsGeneration();
    if (last_md_hit_.Matches(ctx, gen)) {
        return last_md_hit_.result;
    }

    const auto [dip_x, dip_y] = ScreenToPaneDip(ctx);

    const auto first = ctx.cache.cbegin();
    const auto last = first + static_cast<ptrdiff_t>(ctx.nodes.size());
    const auto it = std::ranges::partition_point(first, last, [dip_y](const NodeLayoutEntry& e) noexcept {
        return e.y_position <= dip_y;
    });
    const int candidate = (it != first) ? static_cast<int>(std::prev(it) - first) : -1;

    if (candidate >= 0 && dip_y <= ctx.cache[candidate].y_position + ctx.cache[candidate].height) {
        const auto& node = ctx.nodes[candidate];
        const auto& entry = ctx.cache[candidate];

        if (node.type == NodeType::Table) {
            result = HitTestTable(node, entry, candidate, ctx.theme, dip_x, dip_y);
            last_md_hit_.Store(ctx, gen, result);
            return result;
        }

        if (entry.text_layout) {
            const float indent = NodeIndent(node, ctx.theme);
            const float local_x = dip_x - ctx.theme.margin_left - indent - NodeTextXOffset(node, ctx.theme);
            const float local_y = dip_y - entry.y_position;

            BOOL is_trailing = FALSE;
            BOOL is_inside = FALSE;
            DWRITE_HIT_TEST_METRICS metrics{};
            entry.text_layout->HitTestPoint(local_x, local_y,
                &is_trailing, &is_inside, &metrics);

            result.node_index = candidate;
            result.text_pos = metrics.textPosition + (is_trailing ? 1 : 0);
            last_md_hit_.Store(ctx, gen, result);
            return result;
        }
    }

    // 全ノードより下をクリック → 最後のノードの末尾を選択
    for (const auto& [i, node] : ctx.nodes | std::views::enumerate | std::views::reverse) {
        if (const auto& text = node.GetText(); !text.empty()) {
            result.node_index = static_cast<int>(i);
            result.text_pos = static_cast<uint32_t>(text.size());
            break;
        }
    }
    last_md_hit_.Store(ctx, gen, result);
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

    if (!entry.has_table_layout()) {
        result.text_pos = static_cast<uint32_t>(node.GetText().size());
        return result;
    }
    const auto& tl = *entry.table_layout;

    const float indent = NodeIndent(node, theme);
    const float base_x = theme.margin_left + indent;

    const auto [hit_row, row_top_y] = FindTableRow(node, entry, theme, dip_y);
    if (hit_row < 0) {
        result.text_pos = static_cast<uint32_t>(node.GetText().size());
        return result;
    }

    float cell_left_x = 0.0f;
    const int hit_col = FindTableCol(tl, base_x, dip_x, cell_left_x);

    const uint32_t flat_offset = tl.CellFlatOffset(
        node.table_rows(), static_cast<size_t>(hit_row), static_cast<size_t>(hit_col));

    const size_t r = static_cast<size_t>(hit_row);
    const size_t c = static_cast<size_t>(hit_col);
    IDWriteTextLayout* cell_layout = tl.GetCellLayout(r, c);
    if (cell_layout) {
        const float text_x = cell_left_x + TABLE_CELL_PADDING;
        const float text_y = row_top_y + TABLE_CELL_PADDING;

        BOOL is_trailing = FALSE, is_inside = FALSE;
        DWRITE_HIT_TEST_METRICS metrics{};
        cell_layout->HitTestPoint(
            dip_x - text_x,
            dip_y - text_y,
            &is_trailing,
            &is_inside,
            &metrics
        );

        result.text_pos = flat_offset + metrics.textPosition + (is_trailing ? 1 : 0);
    }
    else {
        result.text_pos = flat_offset;
    }
    return result;
}

int HitTestService::CopyButtonHitTest(const MdPaneHitContext& ctx) const noexcept
{
    assert(ctx.content_width > 0.0f && "content_width must be set for button hit test");
    assert(ctx.md_pane_height > 0.0f && "md_pane_height must be set for button hit test");

    if (ctx.nodes.empty()) {
        return -1;
    }
    const uint32_t gen = ctx.cache.GetEffectsGeneration();
    if (last_copy_hit_.Matches(ctx, gen)) {
        return last_copy_hit_.result;
    }

    // コピーボタンはコンテンツ右端にあるため、X座標で大半のマウス位置を早期棄却
    const float btn_left_bound = ctx.theme.margin_left + ctx.content_width - COPY_BTN_MARGIN - COPY_BTN_SIZE;
    if (ScreenToPaneDip(ctx).x < btn_left_bound) {
        last_copy_hit_.Store(ctx, gen, -1);
        return -1;
    }

    return HitTestCodeBlockButton(ctx, last_copy_hit_,
        [&](int /*i*/, const Node& node, const NodeLayoutEntry& entry,
            float dip_x, float dip_y) noexcept -> bool {
            // ダイアグラム系 (Mermaid / LatexMath) はコピーボタン非対応
            if (IsDiagramLanguage(node.code_language)) {
                return false;
            }
            const float indent = NodeIndent(node, ctx.theme);
            const float x = ctx.theme.margin_left + indent;
            const float w = ctx.content_width - indent;
            const float pad = ctx.theme.code_block_padding;
            const float block_right = x + w;
            const float block_top = entry.y_position - pad;
            const D2D1_RECT_F btn = OverlayButtonRect(block_right, block_top);
            return dip_x >= btn.left && dip_x <= btn.right
                && dip_y >= btn.top && dip_y <= btn.bottom;
        });
}

int HitTestService::SaveButtonHitTest(const MdPaneHitContext& ctx) const noexcept
{
    assert(ctx.content_width > 0.0f && "content_width must be set for button hit test");
    assert(ctx.md_pane_height > 0.0f && "md_pane_height must be set for button hit test");

    return HitTestCodeBlockButton(ctx, last_save_hit_,
        [&](int i, const Node& node, const NodeLayoutEntry& entry,
            float dip_x, float dip_y) noexcept -> bool {
            if (!IsDiagramLanguage(node.code_language)) {
                return false;
            }
            const auto& diagram = ctx.cache.GetDiagram(i);
            if (!diagram.bitmap) {
                return false;
            }
            const float indent = NodeIndent(node, ctx.theme);
            const float x = ctx.theme.margin_left + indent;
            const float cw = ctx.content_width - indent;
            const auto bmp = MermaidBitmapRect(diagram.width, diagram.height, x, cw, entry.y_position);
            const D2D1_RECT_F btn = OverlayButtonRect(bmp.right, bmp.top,
                std::to_underlying(DiagramButtonSlot::Save));
            return dip_x >= btn.left && dip_x <= btn.right
                && dip_y >= btn.top && dip_y <= btn.bottom;
        });
}

HitTestService::CodeBlockButtonHit HitTestService::CodeBlockButtonsHitTest(
    const MdPaneHitContext& ctx) const noexcept
{
    assert(ctx.content_width > 0.0f && "content_width must be set for button hit test");
    assert(ctx.md_pane_height > 0.0f && "md_pane_height must be set for button hit test");

    CodeBlockButtonHit out;
    if (ctx.nodes.empty()) {
        return out;
    }
    const uint32_t gen = ctx.cache.GetEffectsGeneration();
    const bool copy_cached = last_copy_hit_.Matches(ctx, gen);
    const bool save_cached = last_save_hit_.Matches(ctx, gen);
    const bool svg_cached = last_svg_copy_hit_.Matches(ctx, gen);
    if (copy_cached && save_cached && svg_cached) {
        out.copy_node = last_copy_hit_.result;
        out.save_node = last_save_hit_.result;
        out.svg_copy_node = last_svg_copy_hit_.result;
        return out;
    }

    const auto [dip_x, dip_y] = ScreenToPaneDip(ctx);
    const float btn_left_bound = ctx.theme.margin_left + ctx.content_width - COPY_BTN_MARGIN - COPY_BTN_SIZE;
    const bool x_in_copy_band = dip_x >= btn_left_bound;

    const float viewport_top = ctx.scroll_y;
    const float viewport_bottom = ctx.scroll_y + ctx.md_pane_height;
    const int first = FindFirstVisibleNodeIndex(ctx.cache, ctx.nodes.size(), viewport_top);
    const int count = static_cast<int>(ctx.nodes.size());

    int copy_hit = -1;
    int save_hit = -1;
    int svg_copy_hit = -1;
    for (int i = first; i < count; i++) {
        const auto& entry = ctx.cache[i];
        if (entry.y_position - ctx.theme.code_block_padding > viewport_bottom) {
            break;
        }
        const auto& node = ctx.nodes[i];
        if (node.type != NodeType::CodeBlock) {
            continue;
        }
        const float indent = NodeIndent(node, ctx.theme);
        const float x = ctx.theme.margin_left + indent;
        const float w = ctx.content_width - indent;
        const float pad = ctx.theme.code_block_padding;
        if (IsDiagramLanguage(node.code_language)) {
            const auto& diagram = ctx.cache.GetDiagram(i);
            if (diagram.bitmap) {
                const auto bmp = MermaidBitmapRect(diagram.width, diagram.height, x, w, entry.y_position);
                if (save_hit < 0) {
                    const D2D1_RECT_F btn = OverlayButtonRect(bmp.right, bmp.top,
                        std::to_underlying(DiagramButtonSlot::Save));
                    if (dip_x >= btn.left && dip_x <= btn.right
                        && dip_y >= btn.top && dip_y <= btn.bottom) {
                        save_hit = i;
                    }
                }
                if (svg_copy_hit < 0 && IsSvgExportable(node.code_language)) {
                    const D2D1_RECT_F btn2 = OverlayButtonRect(bmp.right, bmp.top,
                        std::to_underlying(DiagramButtonSlot::SvgCopy));
                    if (dip_x >= btn2.left && dip_x <= btn2.right
                        && dip_y >= btn2.top && dip_y <= btn2.bottom) {
                        svg_copy_hit = i;
                    }
                }
            }
        }
        else if (x_in_copy_band && copy_hit < 0) {
            const float block_right = x + w;
            const float block_top = entry.y_position - pad;
            const D2D1_RECT_F btn = OverlayButtonRect(block_right, block_top);
            if (dip_x >= btn.left && dip_x <= btn.right
                && dip_y >= btn.top && dip_y <= btn.bottom) {
                copy_hit = i;
            }
        }
        // マウス座標は1点なので、コピー/保存/SVGコピーボタンが同時にヒットすることはない。
        // 1つでも見つかった時点で残りの可視ノード走査をスキップする。
        if (copy_hit >= 0 || save_hit >= 0 || svg_copy_hit >= 0) {
            break;
        }
    }

    last_copy_hit_.Store(ctx, gen, copy_hit);
    last_save_hit_.Store(ctx, gen, save_hit);
    last_svg_copy_hit_.Store(ctx, gen, svg_copy_hit);
    out.copy_node = copy_hit;
    out.save_node = save_hit;
    out.svg_copy_node = svg_copy_hit;
    return out;
}

NavButtonHover HitTestService::NavButtonHitTest(
    float dip_x, float dip_y, const PaneRect& md_rect) const noexcept
{
    if (NavBackButtonRect(md_rect).Contains(dip_x, dip_y)) {
        return NavButtonHover::Back;
    }
    if (NavForwardButtonRect(md_rect).Contains(dip_x, dip_y)) {
        return NavButtonHover::Forward;
    }
    return NavButtonHover::None;
}
