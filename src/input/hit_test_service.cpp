#include "hit_test_service.h"
#include "block_h_scroll.h"
#include "doc_dwrite_bridge.h"
#include "inline_math.h"
#include "layout_computer.h"
#include "chrome_layout.h"
#include <ranges>

using layout::NodeBlockX;
using layout::NodeBoxPadBottom;
using layout::NodeContentWidth;
using layout::NodeTextXOffset;

namespace {

// layout の position にある inline object が、描いている画像か (HitResult::on_image)。
bool IsDrawnImageAt(IDWriteTextLayout* layout, UINT32 position) noexcept
{
    Microsoft::WRL::ComPtr<IDWriteInlineObject> object;
    if (FAILED(layout->GetInlineObject(position, &object, nullptr)) || !object) {
        return false;
    }
    const auto image = InlineImageObjectOf(object.Get());
    return image && !image->alt && image->size.width > 0.0f;
}

// layout 上の点 (x, y) をヒットテストし、base (ノードまたはセルの先頭) 起点の text_pos / char_pos を書く。
// images はノードが画像を持つか (持たなければ画像の判定を省く)。
void ResolveTextHit(hit_test::HitResult& result, IDWriteTextLayout* layout, float x, float y,
                    std::string_view text, uint32_t base, bool images) noexcept
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
        result.on_image = images && IsDrawnImageAt(layout, metrics.textPosition);
    }
}

// ノード高さ範囲外 (ノード間の余白等) のヒットを最寄りの非空ノードにクランプする。
// 文書全体の末尾へ飛ばすと、余白クリックからのドラッグで巨大選択になる。
hit_test::HitResult ClampToNearestTextNode(std::span<const Node> nodes, int candidate) noexcept
{
    for (int i = candidate; i >= 0; --i) {
        if (const auto text = nodes[static_cast<size_t>(i)].GetText(); !text.empty()) {
            return { i, static_cast<uint32_t>(text.size()) };
        }
    }
    // candidate 以前に非空ノードが無ければ (文頭が HR/図 等)、先頭の非空ノードの先頭へ倒す。
    for (const auto& [i, node] : nodes | std::views::enumerate) {
        if (!node.GetText().empty()) {
            return { static_cast<int>(i), 0 };
        }
    }
    return {};
}

// 点を含む表か text_layout を持つノード。余白や水平線の上などは -1 にして ClampToNearestTextNode に委ねる。
struct DirectHit {
    int node_index = -1;
    // ClampToNearestTextNode に渡す候補 (点より上に上端がある最後のノード)。
    int candidate = -1;
    // candidate の上端からの点の Y (MdPaneHitContext::LocalYExact)。
    double local_y = 0.0;
};

DirectHit FindDirectHit(const MdPaneHitContext& ctx) noexcept
{
    DirectHit hit;
    hit.candidate = static_cast<int>(FindFirstNodeBelow(ctx.cache, ctx.point.y)) - 1;
    if (hit.candidate < 0) {
        return hit;
    }
    const auto c = static_cast<size_t>(hit.candidate);
    hit.local_y = ctx.LocalYExact(c);
    // 背景の下側のはみ出し (横スクロールバーを置く領域) もそのノードに含め、ホバーを切らさない。
    const bool inside = hit.local_y <= ctx.cache[c].height + NodeBoxPadBottom(ctx.nodes[c], ctx.theme);
    const NodeType type = ctx.nodes[c].type;
    if (inside && (type == NodeType::Table || ctx.cache[c].text_layout())) {
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

    const auto direct = FindDirectHit(ctx);
    if (direct.node_index < 0) {
        return ClampToNearestTextNode(ctx.nodes, direct.candidate);
    }

    const auto& node = ctx.nodes[direct.node_index];
    const auto& entry = ctx.cache[direct.node_index];
    const float h_scroll_x = LookupBlockScrollX(
        ctx.block_scroll_x, GetBlockHScrollGeometry(ctx.nodes, ctx.cache, static_cast<size_t>(direct.node_index), NodeContentWidth(node, ctx.theme, ctx.content_width)));
    if (node.type == NodeType::Table) {
        return HitTestTable(node, entry, direct.node_index, ctx.theme, ctx.point.x, direct.local_y, h_scroll_x);
    }
    // FindDirectHit は表か、text_layout を持つノードしか返さない。
    const float local_x = ctx.point.x - NodeBlockX(node, ctx.theme) - NodeTextXOffset(node, ctx.theme) + h_scroll_x;

    HitResult result{ direct.node_index };
    ResolveTextHit(result, entry.text_layout(), local_x, static_cast<float>(direct.local_y), node.GetText(), 0, node.has_images());
    return result;
}

int hit_test::HitNode(const MdPaneHitContext& ctx) noexcept
{
    if (ctx.nodes.empty()) {
        return -1;
    }
    const auto direct = FindDirectHit(ctx);
    return direct.node_index >= 0 ? direct.node_index : ClampToNearestTextNode(ctx.nodes, direct.candidate).node_index;
}

hit_test::HitResult hit_test::HitTestTable(
    const Node& node, const NodeLayoutEntry& entry,
    int node_index,
    const Theme& theme,
    float dip_x, double table_y, float h_scroll_x) noexcept
{
    HitResult result;
    result.node_index = node_index;

    const auto* tl_ptr = entry.table_layout();
    if (!tl_ptr) {
        result.text_pos = static_cast<uint32_t>(node.GetText().size());
        return result;
    }
    const auto& tl = *tl_ptr;

    // 表は scroll_x 分左にずれて見えるので、base_x も同じだけ左にずらして列の自然座標に合わせる。
    const float base_x = NodeBlockX(node, theme) - h_scroll_x;

    const auto* tbl = node.table_data();
    const auto [hit_row, row_top] = FindTableRow(tl, table_y);
    if (hit_row < 0) {
        result.text_pos = static_cast<uint32_t>(node.GetText().size());
        return result;
    }
    // 表の上端からの Y は巨大な表で float の刻みが粗くなるので、行の上端からの差にしてから float にする。
    const auto row_y = static_cast<float>(table_y - row_top);

    const auto [hit_col, cell_left] = FindTableCol(tl, dip_x - base_x);
    const float cell_left_x = base_x + cell_left;

    const auto r = static_cast<size_t>(hit_row);
    const auto c = static_cast<size_t>(hit_col);
    const uint32_t flat_offset = tbl->CellTextStart(r, c);
    if (IDWriteTextLayout* cell_layout = tl.GetCellLayout(r, c)) {
        const auto [text_x, text_y] = TableCellTextOrigin(cell_left_x, 0.0f);
        ResolveTextHit(result, cell_layout, dip_x - text_x, row_y - text_y, tbl->GetCellText(r, c), flat_offset, node.has_images());
    }
    else {
        result.text_pos = flat_offset;
    }
    return result;
}

OverlayButtonHit hit_test::OverlayButtonHitTest(const MdPaneHitContext& ctx) noexcept
{
    const int first = FindFirstVisibleNodeIndex(ctx.cache, ctx.scroll_y);
    const int count = static_cast<int>(ctx.cache.size());

    OverlayButtonHit out;
    for (int i = first; i < count && !out.Any(); i++) {
        const float local_y = ctx.LocalY(static_cast<size_t>(i));
        // ボタンは上端 (コードブロックは padding だけ上) から OVERLAY_BTN_MARGIN 下に 1 段だけ並ぶ。上端は単調に増えるので、
        // 段がポインタより下から始まるノードより先には当たるボタンが無い。
        if (OVERLAY_BTN_MARGIN - ctx.theme.code_block_padding > local_y) {
            break;
        }
        const auto& node = ctx.nodes[i];
        if (!layout::MayHaveOverlayButtons(node) || local_y > OVERLAY_BTN_MARGIN + OVERLAY_BTN_SIZE) {
            continue;
        }
        const float x = NodeBlockX(node, ctx.theme);
        const float w = NodeContentWidth(node, ctx.theme, ctx.content_width);
        layout::ForEachOverlayButton(node, ctx.cache, static_cast<size_t>(i), x, w, 0.0f, ctx.theme, [&](const D2D1_RECT_F& btn, OverlayButton kind) {
            if (PointInRectInclusive(ctx.point.x, local_y, btn)) {
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
    const DiagramEntry* diagram = ctx.cache.FindDiagram(static_cast<size_t>(node_index));
    if (!layout::HasDiagramButtons(diagram)) {
        return false;
    }
    const D2D1_RECT_F image = DiagramRect(diagram->width, diagram->height, NodeBlockX(*node, ctx.theme),
                                          NodeContentWidth(*node, ctx.theme, ctx.content_width), 0.0f);
    return PointInRectInclusive(ctx.point.x, ctx.LocalY(static_cast<size_t>(node_index)), image);
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
    const auto& node = ctx.nodes[i];
    const float bar_y = BlockHScrollbarBarY(ctx.cache[i].height, NodeBoxPadBottom(node, ctx.theme));
    const float block_x = NodeBlockX(node, ctx.theme);
    return PointInRect(ctx.point.x, ctx.LocalY(i), BlockHScrollbarHitRect(block_x, visible_width, bar_y));
}
