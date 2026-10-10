#include "hit_test_service.h"
#include "doc_dwrite_bridge.h"
#include "layout_computer.h"
#include "ui_constants.h"
#include <algorithm>
#include <ranges>
#include <utility>

using mendo::layout::NodeBoxPadY;
using mendo::layout::NodeIndent;
using mendo::layout::NodeTextXOffset;

namespace {

// ウィンドウのピクセル座標を、ペイン左端基準の X とドキュメント Y に変換する。
constexpr DipPoint ScreenToPaneDip(const MdPaneHitContext& ctx) noexcept
{
    const auto dip = PixelToDip(ctx.screen_x, ctx.screen_y, ctx.dpi_scale);
    return { dip.x - ctx.md_rect.x, dip.y - ctx.md_rect.y + ctx.scroll_y };
}

// ノード高さ範囲外 (ノード間の余白等) のヒットを最寄りの非空ノードにクランプする。
// 文書全体の末尾へ飛ばすと、余白クリックからのドラッグで巨大選択になる。
HitTestService::HitResult ClampToNearestTextNode(const std::pmr::vector<Node>& nodes, int candidate) noexcept
{
    for (int i = candidate; i >= 0; --i) {
        if (const auto& text = nodes[static_cast<size_t>(i)].GetText(); !text.empty()) {
            return { i, static_cast<uint32_t>(text.size()) };
        }
    }
    // candidate より前に非空ノードが無い場合 (文頭が HR/Image 等) も含め、
    // 先頭の非空ノードの先頭へ倒して常に有効なヒットを返す。
    for (const auto& [i, node] : nodes | std::views::enumerate) {
        if (!node.GetText().empty()) {
            return { static_cast<int>(i), 0 };
        }
    }
    return {};
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

    const auto [dip_x, dip_y] = ScreenToPaneDip(ctx);

    // nodes と cache のサイズは非同期リロード中などに過渡的に不一致になりうる。
    const auto indices = std::views::iota(size_t{ 0 }, std::min(ctx.nodes.size(), ctx.cache.size()));
    const auto it = std::ranges::partition_point(indices, [&ctx, dip_y](size_t i) noexcept {
        return ctx.cache.Top(i) <= dip_y;
    });
    const size_t first_above = (it == indices.end()) ? indices.size() : *it;
    const int candidate = static_cast<int>(first_above) - 1;

    // partition_point と同じ text_top を局所座標 (local_y) の基準にする。
    const float candidate_text_top = (candidate >= 0) ? ctx.cache.Top(static_cast<size_t>(candidate)) : 0.0f;
    // 背景の下側はみ出し部分 (横スクロールバーを置きたい領域) もそのノードのヒットとして扱い、
    // ホバーが切れないようにする。
    if (candidate >= 0 && dip_y <= candidate_text_top + ctx.cache[candidate].height + NodeBoxPadY(ctx.nodes[candidate], ctx.theme)) {
        const auto& node = ctx.nodes[candidate];
        const auto& entry = ctx.cache[candidate];

        const float h_scroll_x = LookupBlockScrollX(ctx, candidate);

        if (node.type == NodeType::Table) {
            return HitTestTable(node, entry, candidate_text_top, candidate, ctx.theme, dip_x, dip_y, h_scroll_x);
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
            return result;
        }
    }

    return ClampToNearestTextNode(ctx.nodes, candidate);
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
        return result;
    }
    const auto& tl = *entry.table_layout;

    const float indent = NodeIndent(node, theme);
    // テーブルが scroll_x 分左にスライドして見えるため、列の自然座標と一致させるには
    // base_x を scroll_x 分左にずらして与える。
    const float base_x = theme.margin_left + indent - h_scroll_x;

    const auto* tbl = node.table_data();
    const size_t row_count = tbl ? tbl->row_count : 0;
    const auto [hit_row, row_top] = FindTableRow(tl, row_count, theme.font_size_body * TABLE_ROW_HEIGHT_FACTOR, dip_y - entry_text_top);
    if (hit_row < 0) {
        result.text_pos = tbl ? static_cast<uint32_t>(tbl->concat_text.size()) : 0u;
        return result;
    }
    const float row_top_y = entry_text_top + row_top;

    const auto [hit_col, cell_left] = FindTableCol(tl, dip_x - base_x);
    const float cell_left_x = base_x + cell_left;

    const auto r = static_cast<size_t>(hit_row);
    const auto c = static_cast<size_t>(hit_col);
    const uint32_t flat_offset = tbl->CellTextStart(r, c);
    if (IDWriteTextLayout* cell_layout = tl.GetCellLayout(r, c)) {
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
    const auto [dip_x, dip_y] = ScreenToPaneDip(ctx);
    const float btn_left_bound = ctx.theme.margin_left + ctx.content_width - COPY_BTN_MARGIN - COPY_BTN_SIZE;
    const bool x_in_copy_band = dip_x >= btn_left_bound;

    const float viewport_top = ctx.scroll_y;
    const float viewport_bottom = ctx.scroll_y + ctx.md_rect.height;
    // nodes と cache のサイズは非同期リロード中などに過渡的に不一致になりうるため両者の最小で抑える。
    const size_t safe_count = std::min(ctx.nodes.size(), ctx.cache.size());
    const int first = FindFirstVisibleNodeIndex(ctx.cache, safe_count, viewport_top);
    const int count = static_cast<int>(safe_count);

    CodeBlockButtonHit out;
    for (int i = first; i < count; i++) {
        const float entry_text_top = ctx.cache.Top(static_cast<size_t>(i));
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
        // 1 つ見つかった時点でループを抜けるため、ここに来る時点で out は全て未ヒット。
        if (IsDiagramLanguage(node.code_language())) {
            const auto* diagram = ctx.cache.FindDiagram(i);
            if (diagram && diagram->bitmap) {
                const auto bmp = MermaidBitmapRect(diagram->width, diagram->height, x, w, entry_text_top);
                const D2D1_RECT_F btn = OverlayButtonRect(bmp.right, bmp.top, std::to_underlying(DiagramButtonSlot::Save));
                if (PointInRectInclusive(dip_x, dip_y, btn)) {
                    out.save_node = i;
                }
                const D2D1_RECT_F btn2 = OverlayButtonRect(bmp.right, bmp.top, std::to_underlying(DiagramButtonSlot::Copy));
                if (PointInRectInclusive(dip_x, dip_y, btn2)) {
                    out.diagram_copy_node = i;
                }
            }
        }
        else if (x_in_copy_band) {
            const D2D1_RECT_F btn = OverlayButtonRect(x + w, entry_text_top - NodeBoxPadY(node, ctx.theme));
            if (PointInRectInclusive(dip_x, dip_y, btn)) {
                out.copy_node = i;
            }
        }
        // マウス座標は1点なので、コピー/保存/ダイアグラムコピーボタンが同時にヒットすることはない。
        // 1つでも見つかった時点で残りの可視ノード走査をスキップする。
        if (out.copy_node >= 0 || out.save_node >= 0 || out.diagram_copy_node >= 0) {
            break;
        }
    }

    return out;
}

NavButtonHover HitTestService::NavButtonHitTest(float dip_x, float dip_y, const PaneRect& md_rect) const noexcept
{
    if (PointInRectInclusive(dip_x, dip_y, NavBackButtonRect(md_rect))) {
        return NavButtonHover::Back;
    }
    if (PointInRectInclusive(dip_x, dip_y, NavForwardButtonRect(md_rect))) {
        return NavButtonHover::Forward;
    }
    return NavButtonHover::None;
}

bool HitTestService::BlockHScrollbarHitTest(const MdPaneHitContext& ctx, int node_index, float visible_width) const noexcept
{
    const auto i = static_cast<size_t>(node_index);
    if (i >= ctx.nodes.size() || i >= ctx.cache.size()) {
        return false;
    }
    const auto [dip_x, dip_y] = ScreenToPaneDip(ctx);
    const auto& node = ctx.nodes[i];
    const float bar_y = BlockHScrollbarBarY(ctx.cache.Top(i), ctx.cache[i].height, mendo::layout::NodeBoxPadY(node, ctx.theme));
    const float block_x = ctx.theme.margin_left + NodeIndent(node, ctx.theme);
    return PointInRect(dip_x, dip_y, BlockHScrollbarHitRect(block_x, visible_width, bar_y));
}
