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

// layout 上の点 (x, y) をヒットテストし、base (ノードまたはセルの先頭) 起点の text_pos / char_pos を書く。
void ResolveTextHit(hit_test::HitResult& result, IDWriteTextLayout* layout, float x, float y,
                    std::string_view text, uint32_t base) noexcept
{
    BOOL is_trailing = FALSE;
    BOOL is_inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    layout->HitTestPoint(x, y, &is_trailing, &is_inside, &metrics);
    mendo::Utf16OffsetCursor cursor{ text };
    const uint32_t char_pos = cursor.ByteAt(metrics.textPosition);
    result.text_pos = base + (is_trailing ? cursor.ByteAt(metrics.textPosition + 1) : char_pos);
    if (is_inside) {
        result.char_pos = base + char_pos;
    }
}

// ノード高さ範囲外 (ノード間の余白等) のヒットを最寄りの非空ノードにクランプする。
// 文書全体の末尾へ飛ばすと、余白クリックからのドラッグで巨大選択になる。
hit_test::HitResult ClampToNearestTextNode(const std::pmr::vector<Node>& nodes, int candidate) noexcept
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
    const auto indices = std::views::iota(size_t{ 0 }, ctx.cache.size());
    const auto it = std::ranges::partition_point(indices, [&ctx, dip_y](size_t i) noexcept {
        return ctx.cache.Top(i) <= dip_y;
    });
    const size_t first_above = (it == indices.end()) ? indices.size() : *it;
    DirectHit hit;
    hit.candidate = static_cast<int>(first_above) - 1;
    if (hit.candidate < 0) {
        return hit;
    }
    // partition_point と同じ text_top を局所座標 (local_y) の基準にする。
    const auto c = static_cast<size_t>(hit.candidate);
    hit.text_top = ctx.cache.Top(c);
    // 背景の下側はみ出し部分 (横スクロールバーを置きたい領域) もそのノードのヒットとして扱い、
    // ホバーが切れないようにする。
    const bool inside = dip_y <= hit.text_top + ctx.cache[c].height + NodeBoxPadY(ctx.nodes[c], ctx.theme);
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
    const float h_scroll_x = LookupBlockScrollX(ctx.block_scroll_x, direct.node_index);
    if (node.type == NodeType::Table) {
        return HitTestTable(node, entry, direct.text_top, direct.node_index, ctx.theme, dip_x, dip_y, h_scroll_x);
    }
    if (!entry.text_layout()) {
        return { direct.node_index, static_cast<uint32_t>(node.GetText().size()) };
    }

    const float indent = NodeIndent(node, ctx.theme);
    const float local_x = dip_x - ctx.theme.margin_left - indent - NodeTextXOffset(node, ctx.theme) + h_scroll_x;
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
        return result;
    }
    const auto& tl = *tl_ptr;

    const float indent = NodeIndent(node, theme);
    // テーブルが scroll_x 分左にスライドして見えるため、列の自然座標と一致させるには
    // base_x を scroll_x 分左にずらして与える。
    const float base_x = theme.margin_left + indent - h_scroll_x;

    const auto* tbl = node.table_data();
    const size_t row_count = tbl ? tbl->row_count : 0;
    const auto [hit_row, row_top] = FindTableRow(tl, row_count, dip_y - entry_text_top);
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
        const auto [text_x, text_y] = TableCellTextOrigin(cell_left_x, row_top_y);
        ResolveTextHit(result, cell_layout, dip_x - text_x, dip_y - text_y, tbl->GetCellText(r, c), flat_offset);
    }
    else {
        result.text_pos = flat_offset;
    }
    return result;
}

hit_test::CodeBlockButtonHit hit_test::CodeBlockButtonsHitTest(const MdPaneHitContext& ctx) noexcept
{
    if (ctx.nodes.empty()) {
        return {};
    }
    const auto [dip_x, dip_y] = ScreenToPaneDip(ctx);
    const float btn_left_bound = ctx.theme.margin_left + ctx.content_width - COPY_BTN_MARGIN - COPY_BTN_SIZE;
    const bool x_in_copy_band = dip_x >= btn_left_bound;

    const float viewport_top = ctx.scroll_y;
    const float viewport_bottom = ctx.scroll_y + ctx.md_rect.height;
    const int first = FindFirstVisibleNodeIndex(ctx.cache, viewport_top);
    const int count = static_cast<int>(ctx.cache.size());

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
                const D2D1_RECT_F btn = OverlayButtonRect(bmp.right, bmp.top, DiagramButtonSlot::Save);
                if (PointInRectInclusive(dip_x, dip_y, btn)) {
                    out.save_node = i;
                }
                const D2D1_RECT_F btn2 = OverlayButtonRect(bmp.right, bmp.top, DiagramButtonSlot::Copy);
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
        if (out.Any()) {
            break;
        }
    }

    return out;
}

NavButtonHover hit_test::NavButtonHitTest(float dip_x, float dip_y, const PaneRect& md_rect) noexcept
{
    if (PointInRectInclusive(dip_x, dip_y, NavBackButtonRect(md_rect))) {
        return NavButtonHover::Back;
    }
    if (PointInRectInclusive(dip_x, dip_y, NavForwardButtonRect(md_rect))) {
        return NavButtonHover::Forward;
    }
    return NavButtonHover::None;
}

bool hit_test::BlockHScrollbarHitTest(const MdPaneHitContext& ctx, int node_index, float visible_width) noexcept
{
    const auto i = static_cast<size_t>(node_index);
    if (i >= ctx.cache.size()) {
        return false;
    }
    const auto [dip_x, dip_y] = ScreenToPaneDip(ctx);
    const auto& node = ctx.nodes[i];
    const float bar_y = BlockHScrollbarBarY(ctx.cache.Top(i), ctx.cache[i].height, NodeBoxPadY(node, ctx.theme));
    const float block_x = ctx.theme.margin_left + NodeIndent(node, ctx.theme);
    return PointInRect(dip_x, dip_y, BlockHScrollbarHitRect(block_x, visible_width, bar_y));
}
