#include "hit_test_service.h"
#include "doc_dwrite_bridge.h"
#include "layout.h"
#include "layout_computer.h"
#include "ui_constants.h"
#include <ranges>
#include <utility>

namespace {

struct TableRowHit {
    int row;
    float row_top_y;
};
TableRowHit FindTableRow(const Node& node, const NodeLayoutEntry& entry, float entry_text_top, const Theme& theme, float dip_y) noexcept
{
    if (!entry.has_table_layout()) {
        return { -1, 0.0f };
    }
    const auto& tl = *entry.table_layout;
    const auto* tbl = node.table_data();
    const size_t row_count = tbl ? tbl->row_count : 0;
    if (row_count == 0) {
        return { -1, 0.0f };
    }

    // row_cum_y[r]: 行 r の上端（サイズ row_count+1）。
    if (tl.row_cum_y.size() == row_count + 1) {
        const float local_y = dip_y - entry_text_top;
        const auto it = std::ranges::upper_bound(tl.row_cum_y, local_y);
        if (it == tl.row_cum_y.begin()) {
            return { -1, 0.0f };
        }
        const auto idx = std::ranges::distance(tl.row_cum_y.begin(), it) - 1;
        if (static_cast<size_t>(idx) >= row_count) {
            return { -1, 0.0f };
        }
        return { static_cast<int>(idx), entry_text_top + tl.row_cum_y[static_cast<size_t>(idx)] };
    }

    const float border = TABLE_BORDER_WIDTH;
    float ry = entry_text_top;
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

// 見つからない場合は最終列にクランプ。
int FindTableCol(const TableLayoutData& tl, float base_x, float dip_x, float& cell_left_x) noexcept
{
    const auto col_count = tl.col_widths.size();

    // col_cum_x[c]: 列 c の左端（サイズ col_count+1）。
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

HitTestService::HitResult HitTestService::HitTest(const MdPaneHitContext& ctx) const noexcept
{
    HitResult result;
    if (ctx.nodes.empty()) {
        return result;
    }

    md_wv_cache_.ResetIfBufferChanged(ctx.nodes.data(), ctx.nodes.size());
    cell_wv_cache_.ResetIfBufferChanged(ctx.nodes.data(), ctx.nodes.size());

    const uint32_t gen = ctx.cache.GetEffectsGeneration();
    if (last_md_hit_.Matches(ctx, gen)) {
        return last_md_hit_.result;
    }

    const auto [dip_x, dip_y] = ScreenToPaneDip(ctx);

    const auto first = ctx.cache.cbegin();
    // nodes と cache のサイズは非同期リロード中などに過渡的に不一致になりうる。
    const auto last = first + static_cast<ptrdiff_t>(std::min(ctx.nodes.size(), ctx.cache.size()));
    const auto it = std::ranges::partition_point(first, last, [dip_y](const NodeLayoutEntry& e) noexcept {
        return e.text_top <= dip_y;
    });
    const int candidate = (it != first) ? static_cast<int>(std::prev(it) - first) : -1;

    // partition_point は entry.text_top で比較しているため、局所座標 (local_y) の基準も
    // 同じ entry.text_top を直接参照する。TextTopOf (Fenwick 経由) は等価であるべきだが、
    // 累積誤差や部分更新中の不同期で乖離するとマウス位置と一致しないため避ける。
    const float candidate_text_top = (candidate >= 0) ? ctx.cache[candidate].text_top : 0.0f;
    // CodeBlock は背景が text 範囲の上下に padding 分はみ出る。padding 部分 (横スクロールバーを
    // 置きたい領域) もそのノードのヒットとして扱い、ホバーが切れないようにする。
    const float bottom_extension =
        (candidate >= 0 && ctx.nodes[candidate].type == NodeType::CodeBlock)
            ? ctx.theme.code_block_padding
            : 0.0f;
    if (candidate >= 0 && dip_y <= candidate_text_top + ctx.cache[candidate].height + bottom_extension) {
        const auto& node = ctx.nodes[candidate];
        const auto& entry = ctx.cache[candidate];

        const float h_scroll_x = LookupBlockScrollX(ctx, candidate);

        if (node.type == NodeType::Table) {
            result = HitTestTable(node, entry, candidate_text_top, candidate, ctx.theme, dip_x, dip_y, h_scroll_x);
            last_md_hit_.Store(ctx, gen, result, candidate, h_scroll_x);
            return result;
        }

        if (entry.text_layout) {
            const float indent = NodeIndent(node, ctx.theme);
            const float local_x = dip_x - ctx.theme.margin_left - indent - NodeTextXOffset(node, ctx.theme) + h_scroll_x;
            const float local_y = dip_y - candidate_text_top;

            BOOL is_trailing = FALSE;
            BOOL is_inside = FALSE;
            DWRITE_HIT_TEST_METRICS metrics{};
            entry.text_layout->HitTestPoint(local_x, local_y, &is_trailing, &is_inside, &metrics);

            result.node_index = candidate;
            const auto& wv = md_wv_cache_.Get(node.GetText());
            result.text_pos = wv.DocOffsetFromWideOffset(metrics.textPosition + (is_trailing ? 1 : 0));
            last_md_hit_.Store(ctx, gen, result, candidate, h_scroll_x);
            return result;
        }
    }

    // 高さ範囲外 (ノード間の余白等) は最寄りの非空ノードにクランプする。
    // 文書全体の末尾へ飛ばすと、余白クリックからのドラッグで巨大選択になる。
    if (candidate >= 0) {
        for (int i = candidate; i >= 0; --i) {
            if (const auto& text = ctx.nodes[static_cast<size_t>(i)].GetText(); !text.empty()) {
                result.node_index = i;
                result.text_pos = static_cast<uint32_t>(text.size());
                break;
            }
        }
    }
    else {
        for (const auto& [i, node] : ctx.nodes | std::views::enumerate) {
            if (!node.GetText().empty()) {
                result.node_index = static_cast<int>(i);
                result.text_pos = 0;
                break;
            }
        }
    }
    last_md_hit_.Store(ctx, gen, result);
    return result;
}

HitTestService::HitResult HitTestService::HitTestTable(
    const Node& node, const NodeLayoutEntry& entry,
    float entry_text_top,
    int node_index,
    const Theme& theme,
    float dip_x, float dip_y, float h_scroll_x) const noexcept
{
    HitResult result;
    result.node_index = node_index;

    if (!entry.has_table_layout()) {
        result.text_pos = 0;
        return result;
    }
    const auto& tl = *entry.table_layout;

    const float indent = NodeIndent(node, theme);
    // テーブルが scroll_x 分左にスライドして見えるため、列の自然座標と一致させるには
    // base_x を scroll_x 分左にずらして与える。
    const float base_x = theme.margin_left + indent - h_scroll_x;

    const auto* tbl = node.table_data();
    const auto [hit_row, row_top_y] = FindTableRow(node, entry, entry_text_top, theme, dip_y);
    if (hit_row < 0) {
        result.text_pos = tbl ? static_cast<uint32_t>(tbl->concat_text.size()) : 0u;
        return result;
    }

    float cell_left_x = 0.0f;
    const int hit_col = FindTableCol(tl, base_x, dip_x, cell_left_x);

    const uint32_t flat_offset = tbl->CellTextStart(static_cast<size_t>(hit_row), static_cast<size_t>(hit_col));

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
            &metrics);

        const auto& wv = cell_wv_cache_.Get(tbl->GetCellText(r, c));
        const auto cell_doc_off = wv.DocOffsetFromWideOffset(metrics.textPosition + (is_trailing ? 1 : 0));
        result.text_pos = flat_offset + cell_doc_off;
    }
    else {
        result.text_pos = flat_offset;
    }
    return result;
}

HitTestService::CodeBlockButtonHit HitTestService::CodeBlockButtonsHitTest(const MdPaneHitContext& ctx) const noexcept
{
    if (ctx.nodes.empty()) {
        return {};
    }
    const uint32_t gen = ctx.cache.GetEffectsGeneration();
    if (button_cache_.Matches(ctx, gen)) {
        return button_cache_.result;
    }

    const auto [dip_x, dip_y] = ScreenToPaneDip(ctx);
    const float btn_left_bound = ctx.theme.margin_left + ctx.content_width - COPY_BTN_MARGIN - COPY_BTN_SIZE;
    const bool x_in_copy_band = dip_x >= btn_left_bound;

    const float viewport_top = ctx.scroll_y;
    const float viewport_bottom = ctx.scroll_y + ctx.md_pane_height;
    // nodes と cache のサイズは非同期リロード中などに過渡的に不一致になりうるため両者の最小で抑える。
    const size_t safe_count = std::min(ctx.nodes.size(), ctx.cache.size());
    const int first = FindFirstVisibleNodeIndex(ctx.cache, safe_count, viewport_top);
    const int count = static_cast<int>(safe_count);

    CodeBlockButtonHit out;
    for (int i = first; i < count; i++) {
        const auto& entry = ctx.cache[i];
        const float entry_text_top = entry.text_top;
        if (entry_text_top - ctx.theme.code_block_padding > viewport_bottom) {
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
        if (IsDiagramLanguage(node.code_language())) {
            const auto& diagram = ctx.cache.GetDiagram(i);
            if (diagram.bitmap) {
                const auto bmp = MermaidBitmapRect(diagram.width, diagram.height, x, w, entry_text_top);
                if (out.save_node < 0) {
                    const D2D1_RECT_F btn = OverlayButtonRect(bmp.right, bmp.top, std::to_underlying(DiagramButtonSlot::Save));
                    if (PointInRectInclusive(dip_x, dip_y, btn)) {
                        out.save_node = i;
                    }
                }
                if (out.svg_copy_node < 0 && IsSvgExportable(node.code_language())) {
                    const D2D1_RECT_F btn2 = OverlayButtonRect(bmp.right, bmp.top, std::to_underlying(DiagramButtonSlot::SvgCopy));
                    if (PointInRectInclusive(dip_x, dip_y, btn2)) {
                        out.svg_copy_node = i;
                    }
                }
            }
        }
        else if (x_in_copy_band && out.copy_node < 0) {
            const float block_right = x + w;
            const float block_top = entry_text_top - pad;
            const D2D1_RECT_F btn = OverlayButtonRect(block_right, block_top);
            if (PointInRectInclusive(dip_x, dip_y, btn)) {
                out.copy_node = i;
            }
        }
        // マウス座標は1点なので、コピー/保存/SVGコピーボタンが同時にヒットすることはない。
        // 1つでも見つかった時点で残りの可視ノード走査をスキップする。
        if (out.copy_node >= 0 || out.save_node >= 0 || out.svg_copy_node >= 0) {
            break;
        }
    }

    button_cache_.Store(ctx, gen, out);
    return out;
}

NavButtonHover HitTestService::NavButtonHitTest(float dip_x, float dip_y, const PaneRect& md_rect) const noexcept
{
    if (NavBackButtonRect(md_rect).Contains(dip_x, dip_y)) {
        return NavButtonHover::Back;
    }
    if (NavForwardButtonRect(md_rect).Contains(dip_x, dip_y)) {
        return NavButtonHover::Forward;
    }
    return NavButtonHover::None;
}
