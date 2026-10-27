#include "hit_test_service.h"
#include "block_h_scroll.h"
#include "doc_dwrite_bridge.h"
#include "layout_computer.h"
#include "ui_constants.h"
#include <algorithm>
#include <ranges>
#include <utility>

using layout::NodeBlockX;
using layout::NodeBoxPadBottom;
using layout::NodeContentWidth;
using layout::NodeTextXOffset;

namespace {

// ウィンドウのピクセル座標を、ペイン左端基準の X とドキュメント Y に変換する。
constexpr DipPoint ScreenToPaneDip(const MdPaneHitContext& ctx) noexcept
{
    const auto dip = PixelToDip(ctx.screen_x, ctx.screen_y, ctx.dpi_scale);
    return { dip.x - ctx.md_rect.x, dip.y - ctx.md_rect.y + ctx.scroll_y };
}

// layout 上の点 (x, y) をヒットテストし、base (ノードまたはセルの先頭) 起点の text_pos / char_pos を書く。
void ResolveTextHit(hit_test::HitResult& result, IDWriteTextLayout* layout, float x, float y,
                    std::string_view text, uint32_t base) noexcept
{
    BOOL is_trailing = FALSE;
    BOOL is_inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    layout->HitTestPoint(x, y, &is_trailing, &is_inside, &metrics);
    Utf16OffsetCursor cursor{ text };
    const uint32_t char_pos = cursor.ByteAt(metrics.textPosition);
    // 結合文字やサロゲートペアの途中に置かないよう、クラスタの長さ分進める。
    result.text_pos = base + (is_trailing ? cursor.ByteAt(metrics.textPosition + metrics.length) : char_pos);
    if (is_inside) {
        result.char_pos = base + char_pos;
    }
}

// ノード高さ範囲外 (ノード間の余白等) のヒットを最寄りの非空ノードにクランプする。
// 文書全体の末尾へ飛ばすと、余白クリックからのドラッグで巨大選択になる。
hit_test::HitResult ClampToNearestTextNode(const std::vector<Node>& nodes, int candidate) noexcept
{
    for (int i = candidate; i >= 0; --i) {
        if (const auto text = nodes[static_cast<size_t>(i)].GetText(); !text.empty()) {
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

// dip_y を含むノード。表・画像・テキストレイアウトを持つノードだけ返し、
// 余白や水平線の上などでは -1 を返して ClampToNearestTextNode に委ねる。
// 画像は alt が空でも src のツールチップを出すため、前のノードへ寄せずに返す。
struct DirectHit {
    int node_index = -1;
    // ClampToNearestTextNode に渡す候補 (dip_y 以下に上端がある最後のノード)。
    int candidate = -1;
    float text_top = 0.0f;
};

DirectHit FindDirectHit(const MdPaneHitContext& ctx, float dip_y) noexcept
{
    DirectHit hit;
    hit.candidate = static_cast<int>(FindFirstNodeBelow(ctx.cache, dip_y)) - 1;
    if (hit.candidate < 0) {
        return hit;
    }
    // 探索と同じ text_top を局所座標 (local_y) の基準にする。
    const auto c = static_cast<size_t>(hit.candidate);
    hit.text_top = ctx.cache.Top(c);
    // 背景の下側はみ出し部分 (横スクロールバーを置きたい領域) もそのノードのヒットとして扱い、
    // ホバーが切れないようにする。
    const bool inside = dip_y <= hit.text_top + ctx.cache[c].height + NodeBoxPadBottom(ctx.nodes[c], ctx.theme);
    const NodeType type = ctx.nodes[c].type;
    if (inside && (type == NodeType::Table || type == NodeType::Image || ctx.cache[c].text_layout())) {
        hit.node_index = hit.candidate;
    }
    return hit;
}

} // namespace

hit_test::HitResult hit_test::HitTest(const MdPaneHitContext& ctx) noexcept
{
    if (ctx.nodes.empty()) {
        return {};
    }

    const auto [dip_x, dip_y] = ScreenToPaneDip(ctx);
    const auto direct = FindDirectHit(ctx, dip_y);
    if (direct.node_index < 0) {
        return ClampToNearestTextNode(ctx.nodes, direct.candidate);
    }

    const auto& node = ctx.nodes[direct.node_index];
    const auto& entry = ctx.cache[direct.node_index];
    const float h_scroll_x = LookupBlockScrollX(
        ctx.block_scroll_x, GetBlockHScrollGeometry(ctx.nodes, ctx.cache, static_cast<size_t>(direct.node_index), NodeContentWidth(node, ctx.theme, ctx.content_width)));
    if (node.type == NodeType::Table) {
        return HitTestTable(node, entry, direct.text_top, direct.node_index, ctx.theme, dip_x, dip_y, h_scroll_x);
    }
    if (!entry.text_layout()) {
        return { direct.node_index, static_cast<uint32_t>(node.GetText().size()) };
    }

    const float local_x = dip_x - NodeBlockX(node, ctx.theme) - NodeTextXOffset(node, ctx.theme) + h_scroll_x;
    const float local_y = dip_y - direct.text_top;

    HitResult result{ direct.node_index };
    ResolveTextHit(result, entry.text_layout(), local_x, local_y, node.GetText(), 0);
    return result;
}

int hit_test::HitNode(const MdPaneHitContext& ctx) noexcept
{
    if (ctx.nodes.empty()) {
        return -1;
    }
    const auto direct = FindDirectHit(ctx, ScreenToPaneDip(ctx).y);
    return direct.node_index >= 0 ? direct.node_index : ClampToNearestTextNode(ctx.nodes, direct.candidate).node_index;
}

hit_test::HitResult hit_test::HitTestTable(
    const Node& node, const NodeLayoutEntry& entry,
    float entry_text_top,
    int node_index,
    const Theme& theme,
    float dip_x, float dip_y, float h_scroll_x) noexcept
{
    HitResult result;
    result.node_index = node_index;

    const auto* tl_ptr = entry.table_layout();
    if (!tl_ptr) {
        result.text_pos = static_cast<uint32_t>(node.GetText().size());
        return result;
    }
    const auto& tl = *tl_ptr;

    // テーブルが scroll_x 分左にスライドして見えるため、列の自然座標と一致させるには
    // base_x を scroll_x 分左にずらして与える。
    const float base_x = NodeBlockX(node, theme) - h_scroll_x;

    const auto* tbl = node.table_data();
    const auto [hit_row, row_top] = FindTableRow(tl, dip_y - entry_text_top);
    if (hit_row < 0) {
        result.text_pos = static_cast<uint32_t>(node.GetText().size());
        return result;
    }
    const float row_top_y = entry_text_top + row_top;

    const auto [hit_col, cell_left] = FindTableCol(tl, dip_x - base_x);
    const float cell_left_x = base_x + cell_left;

    const auto r = static_cast<size_t>(hit_row);
    const auto c = static_cast<size_t>(hit_col);
    const uint32_t flat_offset = tbl->CellTextStart(r, c);
    if (IDWriteTextLayout* cell_layout = tl.GetCellLayout(r, c)) {
        const auto [text_x, text_y] = TableCellTextOrigin(cell_left_x, row_top_y);
        ResolveTextHit(result, cell_layout, dip_x - text_x, dip_y - text_y, tbl->GetCellText(r, c), flat_offset);
    }
    else {
        result.text_pos = flat_offset;
    }
    return result;
}

OverlayButtonHit hit_test::OverlayButtonHitTest(const MdPaneHitContext& ctx) noexcept
{
    const auto [dip_x, dip_y] = ScreenToPaneDip(ctx);
    const float viewport_bottom = ctx.scroll_y + ctx.md_rect.height;
    const int first = FindFirstVisibleNodeIndex(ctx.cache, ctx.scroll_y);
    const int count = static_cast<int>(ctx.cache.size());

    OverlayButtonHit out;
    for (int i = first; i < count && !out.Any(); i++) {
        const float entry_text_top = ctx.cache.Top(static_cast<size_t>(i));
        if (entry_text_top - ctx.theme.code_block_padding > viewport_bottom) {
            break;
        }
        const auto& node = ctx.nodes[i];
        if (node.type != NodeType::CodeBlock && node.type != NodeType::Diagram) {
            continue;
        }
        // ボタンは上端 (コードブロックは padding だけ上) から COPY_BTN_MARGIN 下に 1 段だけ並ぶ。
        if (dip_y < entry_text_top - ctx.theme.code_block_padding + COPY_BTN_MARGIN || dip_y > entry_text_top + COPY_BTN_MARGIN + COPY_BTN_SIZE) {
            continue;
        }
        const float x = NodeBlockX(node, ctx.theme);
        const float w = NodeContentWidth(node, ctx.theme, ctx.content_width);
        const DiagramEntry* diagram = node.type == NodeType::Diagram ? ctx.cache.FindDiagram(static_cast<size_t>(i)) : nullptr;
        layout::ForEachOverlayButton(node, diagram, x, w, entry_text_top, ctx.theme, [&](const D2D1_RECT_F& btn, OverlayButton kind) {
            if (PointInRectInclusive(dip_x, dip_y, btn)) {
                out = { i, kind };
            }
        });
    }
    return out;
}

bool hit_test::DiagramImageHitTest(const MdPaneHitContext& ctx, int node_index) noexcept
{
    const Node* node = NodeOfType(ctx.nodes, node_index, NodeType::Diagram);
    if (!node) {
        return false;
    }
    // ボタンを出す条件 (layout::ForEachOverlayButton) と同じく、描けていて元の PNG がある図だけ。
    const DiagramEntry* diagram = ctx.cache.FindDiagram(static_cast<size_t>(node_index));
    if (!diagram || !diagram->bitmap || !diagram->png) {
        return false;
    }
    const auto [dip_x, dip_y] = ScreenToPaneDip(ctx);
    const D2D1_RECT_F image = MermaidBitmapRect(diagram->width, diagram->height, NodeBlockX(*node, ctx.theme),
                                                NodeContentWidth(*node, ctx.theme, ctx.content_width), ctx.cache.Top(static_cast<size_t>(node_index)));
    return PointInRectInclusive(dip_x, dip_y, image);
}

NavDirection hit_test::NavButtonHitTest(float dip_x, float dip_y, const PaneRect& md_rect) noexcept
{
    if (PointInRectInclusive(dip_x, dip_y, NavBackButtonRect(md_rect))) {
        return NavDirection::Back;
    }
    if (PointInRectInclusive(dip_x, dip_y, NavForwardButtonRect(md_rect))) {
        return NavDirection::Forward;
    }
    return NavDirection::None;
}

bool hit_test::BlockHScrollbarHitTest(const MdPaneHitContext& ctx, int node_index, float visible_width) noexcept
{
    const auto i = static_cast<size_t>(node_index);
    if (i >= ctx.cache.size()) {
        return false;
    }
    const auto [dip_x, dip_y] = ScreenToPaneDip(ctx);
    const auto& node = ctx.nodes[i];
    const float bar_y = BlockHScrollbarBarY(ctx.cache.Top(i), ctx.cache[i].height, NodeBoxPadBottom(node, ctx.theme));
    const float block_x = NodeBlockX(node, ctx.theme);
    return PointInRect(dip_x, dip_y, BlockHScrollbarHitRect(block_x, visible_width, bar_y));
}
