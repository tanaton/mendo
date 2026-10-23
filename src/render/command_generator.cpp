#include "command_generator.h"
#include "d2d_util.h"
#include "doc_dwrite_bridge.h"
#include "i18n.h"
#include "layout_computer.h"
#include "profiler.h"
#include "ui_constants.h"
#include <algorithm>
#include <cstdint>
#include <format>
#include <ranges>
#include <span>
#include <utility>

using layout::NodeBoxPadBottom;
using layout::NodeBoxPadTop;
using layout::NodeContentWidth;
using layout::NodeIndent;
using layout::NodeTextXOffset;

#ifdef MENDO_USE_TRACY
namespace {

struct CmdGenStats {
    int64_t hittest_range = 0;
    int64_t sel_hl_cache_hit = 0;
    int64_t sel_hl_cache_miss = 0;
    int64_t search_hl_reset = 0;
    int64_t search_hl_computed = 0;
    int64_t search_hl_provisional = 0;
    int64_t last_visible_node_count = 0;
};
CmdGenStats g_cmd_gen_stats;

void PublishCmdGenStats() noexcept
{
    MENDO_PLOT("cmdgen.hittest_range", g_cmd_gen_stats.hittest_range);
    MENDO_PLOT("cmdgen.sel_hl_cache_hit", g_cmd_gen_stats.sel_hl_cache_hit);
    MENDO_PLOT("cmdgen.sel_hl_cache_miss", g_cmd_gen_stats.sel_hl_cache_miss);
    MENDO_PLOT("cmdgen.search_hl_reset", g_cmd_gen_stats.search_hl_reset);
    MENDO_PLOT("cmdgen.search_hl_computed", g_cmd_gen_stats.search_hl_computed);
    MENDO_PLOT("cmdgen.search_hl_provisional", g_cmd_gen_stats.search_hl_provisional);
    MENDO_PLOT("cmdgen.visible_node_count", g_cmd_gen_stats.last_visible_node_count);
}

} // namespace
#endif

namespace {

// 可視 Y 範囲 [cull_top, cull_bottom] (ペインローカル) と重なるか。
constexpr bool OverlapsY(const D2D1_RECT_F& r, float cull_top, float cull_bottom) noexcept
{
    return !IsOffscreen(r.top, r.bottom - r.top, cull_top, cull_bottom);
}

// 可視範囲より高いレイアウトだけ、executor に範囲外のグリフランを飛ばさせる。
DrawTextLayoutCmd MakeTextLayoutCmd(D2D1_POINT_2F origin, IDWriteTextLayout* layout, float height, BrushId brush_id, float cull_top, float cull_bottom) noexcept
{
    DrawTextLayoutCmd cmd{ origin, layout, brush_id };
    if (height > cull_bottom - cull_top) {
        cmd.cull_top = cull_top;
        cmd.cull_bottom = cull_bottom;
    }
    return cmd;
}

FillRoundedRectCmd MakeInlineCodeBgCmd(const D2D1_RECT_F& r) noexcept
{
    return { r, INLINE_CODE_CORNER, INLINE_CODE_CORNER, BrushId::CodeBg };
}

// bgs はパディング適用済みのレイアウト原点相対矩形。
void GenInlineCodeBgs(DrawCommandList& cmds, std::span<const InlineCodeBg> bgs, float origin_x, float origin_y, float cull_top, float cull_bottom)
{
    for (const auto& bg : bgs) {
        const auto r = OffsetRectF(bg, origin_x, origin_y);
        if (OverlapsY(r, cull_top, cull_bottom)) {
            cmds.emplace_back(MakeInlineCodeBgCmd(r));
        }
    }
}

// row_bgs は列の昇順なので、col 以下の分を先頭から消費し col のセルの背景だけを描く。
// 列を昇順に渡す限り、行全体で O(背景数) で済む。
void GenCellInlineCodeBgs(DrawCommandList& cmds, std::span<const CellInlineCodeBg>& row_bgs, size_t col, float origin_x, float origin_y)
{
    while (!row_bgs.empty() && row_bgs.front().col <= col) {
        if (row_bgs.front().col == col) {
            cmds.emplace_back(MakeInlineCodeBgCmd(OffsetRectF(row_bgs.front().rect, origin_x, origin_y)));
        }
        row_bgs = row_bgs.subspan(1);
    }
}

// h1/h2 見出し下線を描画するY座標を heading_spacing_below_h1h2 のこの比率で決める。
// 値を小さくすると下線は見出し文字に近づき、下線と次行の間隔が広がる。
constexpr float HEADING_UNDERLINE_OFFSET_RATIO = 0.25f;

// 見出し下端から下線までの距離。
constexpr float HeadingUnderlineGap(const Theme& theme) noexcept
{
    return theme.heading_spacing_below_h1h2 * HEADING_UNDERLINE_OFFSET_RATIO;
}

// 引用バーはグループの上下へ、アラート背景は四辺へこの分はみ出す。
constexpr float BAR_EXTEND = 2.0f;
constexpr float ALERT_BG_PAD = 4.0f;

// AlertBrushIdFromIndex (brush_id.h) の連番が AlertType 全種を網羅することを担保する。
static_assert(ALERT_BRUSH_COUNT == ALERT_TYPE_COUNT, "AlertBrushIdFromIndex must cover every AlertType");

// ブロックローカル横スクロールの clip と Translate を RAII で囲み、push/pop の対称性を強制する。
// scroll_x == 0 のときは Translate を省略し、ブロックがスクロールしないときは Clip も省略する。
// スクロールできるが scroll_x == 0 の場合 (初期状態) は、依然として右端で
// Clip しないとペイン余白に内容がはみ出るため、Clip と Translate を独立に判定する。
class BlockHScrollScope {
public:
    BlockHScrollScope(DrawCommandList& cmds, const D2D1_RECT_F& clip,
                      const D2D1::Matrix3x2F& base_transform, float scroll_x, bool needs_clip)
        : cmds_(cmds), restore_(base_transform),
          push_clip_(needs_clip), translate_(needs_clip && scroll_x > 0.0f)
    {
        if (push_clip_) {
            cmds_.emplace_back(PushClipCmd{ clip });
        }
        if (translate_) {
            cmds_.emplace_back(SetTransformCmd{ base_transform * D2D1::Matrix3x2F::Translation(-scroll_x, 0) });
        }
    }
    ~BlockHScrollScope()
    {
        if (translate_) {
            cmds_.emplace_back(SetTransformCmd{ restore_ });
        }
        if (push_clip_) {
            cmds_.emplace_back(PopClipCmd{});
        }
    }
    BlockHScrollScope(const BlockHScrollScope&) = delete;
    BlockHScrollScope& operator=(const BlockHScrollScope&) = delete;

private:
    DrawCommandList& cmds_;
    D2D1::Matrix3x2F restore_;
    bool push_clip_;
    bool translate_;
};

// MeasureNode 時に確定済みの first_line_height を優先利用する。
// 0（未確定）なら DirectWrite に問い合わせず font_size ベースのフォールバックで済ませ、
// 描画ホットパスから COM 越境呼び出しを排除する。
float GetFirstLineHeight(const NodeLayoutEntry& entry, float font_size) noexcept
{
    return (entry.first_line_height() > 0.0f) ? entry.first_line_height() : font_size * FALLBACK_LINE_HEIGHT_FACTOR;
}

} // namespace

// box はスクロール位置に依らない値を渡す。矩形の差から求めると丸めでキャッシュが外れる。
void CommandGenerator::EmitText(std::wstring_view text, IDWriteTextFormat* fmt, D2D1_POINT_2F origin, D2D1_SIZE_F box, BrushId brush_id, float opacity)
{
    if (!text_layouts_) {
        return;
    }
    if (auto* layout = text_layouts_->Get(fmt, text, box)) {
        cmds_.emplace_back(DrawTextLayoutCmd{ .origin = origin, .layout = layout, .brush_id = brush_id, .opacity = opacity });
    }
}

// SelectionHlCache は lazy 確保のみで自動破棄経路が無く、選択範囲外に出たノード分が
// 居残ってメモリが漸増する。前フレームの範囲との差分区間だけ巻き戻す
// (Ctrl+A 後のドラッグ等で範囲全体を毎回走査しない)。
void CommandGenerator::ReleaseStaleSelectionHlCaches(const LayoutCache& cache, const TextSelection& selection)
{
    const int new_start = selection.active ? selection.start_node : -1;
    const int new_end = selection.active ? selection.end_node : -1;
    if ((new_start != prev_sel_start_node_ || new_end != prev_sel_end_node_) && prev_sel_start_node_ >= 0) {
        const int lo = prev_sel_start_node_;
        const int hi = std::min(prev_sel_end_node_, static_cast<int>(cache.size()) - 1);
        const auto invalidate_range = [&](int first, int last) {
            for (int i = std::max(first, lo); i <= std::min(last, hi); i++) {
                cache[i].invalidate_selection_hl_cache();
            }
        };
        if (new_start < 0) {
            invalidate_range(lo, hi);
        }
        else {
            invalidate_range(lo, new_start - 1);
            invalidate_range(new_end + 1, hi);
        }
    }
    prev_sel_start_node_ = new_start;
    prev_sel_end_node_ = new_end;
}

const DrawCommandList& CommandGenerator::GenerateMdPane(
    const std::pmr::vector<Node>& nodes, const LayoutCache& cache,
    const PaneRect& md_pane_rect, float scroll_y,
    const TextSelection& selection,
    OverlayButtonHit hovered,
    float dpi_scale,
    const BlockHScrollContext& block_h_scroll)
{
    MENDO_PROFILE("GenerateMdPane");
    cmds_.clear();

    cmds_.emplace_back(PushClipCmd{ ToD2DRect(md_pane_rect) });
    // スクロール位置を物理ピクセル境界にスナップし、ClearTypeヒンティングの
    // フレーム間変動によるテキストのガタつきを防止する。スナップは描画 Y のみに
    // 適用し、二分探索 (FindFirstVisibleNodeIndex) には未スナップ scroll_y を渡す。
    const float snapped_y = SnapToPhysicalPixel(scroll_y, dpi_scale);

    // スクロールの Y 平行移動は Transform に乗せず CPU 側で entry ごとに加算する。
    // 大規模ファイルで text_top/scroll_y が 10^7 DIP オーダーになると、D2D 内部の
    // float32 行列演算で catastrophic cancellation が起きるため。
    // ペイン原点は小さい定数なので Transform に乗せ、ローカル Y を [0, ペイン高さ] に揃えて
    // 可視範囲の判定と一致させる。原点を物理ピクセルに揃えておくと、ローカル座標の snap だけで
    // bullet やスクロールバーがピクセル境界に乗る。
    const auto pane_transform = D2D1::Matrix3x2F::Translation(
        SnapToPhysicalPixel(md_pane_rect.x, dpi_scale), SnapToPhysicalPixel(md_pane_rect.y, dpi_scale));
    cmds_.emplace_back(SetTransformCmd{ pane_transform });

    const FrameContext fc{
        .offset_x = theme_->margin_left,
        // 水平カリング範囲: ペイン内ローカル座標で margin_left を起点とした相対値
        .viewport_left = -theme_->margin_left,
        .viewport_right = md_pane_rect.width - theme_->margin_left,
        .content_width = theme_->ContentWidth(md_pane_rect.width),
        .dpi_scale = dpi_scale,
        .snapped_scroll_y = snapped_y,
        .pane_transform = pane_transform,
        .selection = selection,
        .nodes = nodes,
        .cache = cache,
        .hovered = hovered,
        .h_scroll = block_h_scroll,
        .overhang = MaxNodeOverhang(),
    };

    cull_bottom_ = md_pane_rect.height;

    ReleaseStaleSelectionHlCaches(cache, selection);

    const int node_count = static_cast<int>(nodes.size());
    // 二分探索キーは未スナップのドキュメント Y (cache[i].text_top と同じ系)。
    // 下端より下にはみ出す描画 (見出し下線・空 LI の bullet 等) を上端で落とさないよう、
    // はみ出しの上限だけ手前から始める。
    const int first_visible = FindFirstVisibleNodeIndex(cache, scroll_y - fc.overhang.below);

    // 同一 blockquote_group のノードをまとめてバー/背景を描画する。
    // ノード単体ごとではなく一括で描画することで、複数行の引用ブロックが連続した見た目になる。
    GenBlockQuoteGroupDecorations(fc, nodes, cache, first_visible);

    int visible_count = 0;
    for (int i = first_visible; i < node_count; i++) {
        const float local_text_top = cache.Top(i) - snapped_y;
        if (local_text_top - fc.overhang.above > cull_bottom_) {
            break;
        }
        const Node& node = nodes[i];
        const DiagramEntry* diagram = IsBitmapDrawNode(node) ? cache.FindDiagram(i) : nullptr;
        GenerateNode(fc, node, cache[i], diagram, i, local_text_top);
        ++visible_count;
    }

    cmds_.emplace_back(SetTransformCmd{ D2D1::Matrix3x2F::Identity() });
    cmds_.emplace_back(PopClipCmd{});
    MENDO_COUNT_SET(g_cmd_gen_stats.last_visible_node_count, visible_count);
    MENDO_IF_TRACY(PublishCmdGenStats());
    return cmds_;
}

void CommandGenerator::GenerateNode(const FrameContext& fc,
    const Node& node, const NodeLayoutEntry& entry, const DiagramEntry* diagram,
    int node_index, float entry_text_top)
{
    if (entry_text_top + entry.height + fc.overhang.below < cull_top_ || entry_text_top - fc.overhang.above > cull_bottom_) {
        return;
    }

    const float indent = NodeIndent(node, *theme_);
    const float x = fc.offset_x + indent;
    const float cw = NodeContentWidth(node, *theme_, fc.content_width);
    const float text_x = x + NodeTextXOffset(node, *theme_);

    // ブロック横スクロール対象 (テーブル/コードブロック) の内容を clip + 平行移動で囲み、
    // 直後にスクロールバーを積む。幾何はヒットテスト/reducer と共有の GetBlockHScrollGeometry に寄せる。
    const auto gen_h_scroll_block = [&](const D2D1_RECT_F& clip, float pad_y, auto&& gen_content) {
        const auto geom = GetBlockHScrollGeometry(fc.nodes, fc.cache, static_cast<size_t>(node_index), cw);
        const float scroll_x = LookupBlockScrollX(fc.h_scroll.scroll_x, geom);
        {
            BlockHScrollScope guard(cmds_, clip, fc.pane_transform, scroll_x, geom.can_scroll());
            gen_content(scroll_x);
        }
        EmitBlockHScrollbarIfActive(fc, node_index, x, BlockHScrollbarBarY(entry_text_top, entry.height, pad_y), geom, scroll_x);
    };

    switch (node.type) {
    case NodeType::HorizontalRule:
        GenHorizontalRule(x, cw, entry_text_top);
        return;

    case NodeType::Image:
        if (diagram && diagram->bitmap) {
            const float draw_h = entry.height;
            const float draw_w = (diagram->height > 0) ? diagram->width * (draw_h / diagram->height) : diagram->width;
            cmds_.emplace_back(DrawBitmapCmd{ diagram->bitmap.Get(), D2D1::RectF(x, entry_text_top, x + draw_w, entry_text_top + draw_h) });
        }
        else {
            GenDiagramPlaceholder(x, entry_text_top, cw, entry.height);
        }
        return;

    case NodeType::Table:
        gen_h_scroll_block(
            D2D1::RectF(x, entry_text_top, x + cw, entry_text_top + entry.height),
            NodeBoxPadBottom(node, *theme_),
            [&](float scroll_x) { GenTable(fc, node, entry, node_index, x, entry_text_top, scroll_x); });
        return;

    case NodeType::Diagram:
        if (diagram && diagram->bitmap) {
            cmds_.emplace_back(DrawBitmapCmd{ diagram->bitmap.Get(), MermaidBitmapRect(diagram->width, diagram->height, x, cw, entry_text_top) });
            GenOverlayButtons(fc, node, diagram, x, cw, entry_text_top, node_index);
        }
        else {
            GenDiagramPlaceholder(x, entry_text_top, cw, entry.height, diagram ? std::wstring_view{ diagram->error } : std::wstring_view{});
        }
        return;

    case NodeType::CodeBlock: {
        const float pad_bottom = NodeBoxPadBottom(node, *theme_);
        const D2D1_RECT_F box = D2D1::RectF(x, entry_text_top - NodeBoxPadTop(node, *theme_), x + cw, entry_text_top + entry.height + pad_bottom);
        GenCodeBlockBg(box, !node.continues_prev(), !node.continues_next());
        // 背景とコピーボタンはクリップ外で固定描画 (GitHub と同じ挙動)。テキスト本体だけ scroll_x 分平行移動。
        gen_h_scroll_block(box, pad_bottom, [&](float) { GenNodeTextDecorations(fc, node, entry, node_index, text_x, entry_text_top); });
        GenOverlayButtons(fc, node, nullptr, x, cw, entry_text_top, node_index);
        return;
    }

    case NodeType::ListItem:
        GenListBullet(fc, node, entry, x, entry_text_top);
        break;

    case NodeType::TaskListItem:
        // GenNodeTextDecorations は text_layout=nullptr (loose で空 TaskListItem) で early return
        // するため、checkbox は case 側で emit する。
        GenTaskListCheckbox(node, x, entry_text_top);
        break;

    case NodeType::BlockQuote:
        // バーと背景はグループ単位で GenBlockQuoteGroupDecorations から描画済み
        break;

    case NodeType::Heading: {
        const int8_t lv = node.heading_level();
        if (lv <= 2) {
            const float line_y = entry_text_top + entry.height + HeadingUnderlineGap(*theme_);
            cmds_.emplace_back(DrawLineCmd{
                D2D1::Point2F(x, line_y), D2D1::Point2F(x + cw, line_y),
                theme_->GetHeadingUnderlineThickness(lv), BrushId::Hr });
        }
        break;
    }

    case NodeType::Paragraph:
        break;

    default:
        std::unreachable();
    }

    GenNodeTextDecorations(fc, node, entry, node_index, text_x, entry_text_top);
}

CommandGenerator::NodeOverhang CommandGenerator::MaxNodeOverhang() const noexcept
{
    const float underline = HeadingUnderlineGap(*theme_) + std::max(theme_->GetHeadingUnderlineThickness(1), theme_->GetHeadingUnderlineThickness(2));
    // 空 LI は height=0 だが bullet/checkbox は text_top より下に描かれる。
    const float empty_li = layout::EmptyListItemHeight(*theme_);
    // インラインコード背景・コードブロックの padding 内の装飾・アラート背景・引用バーは上下両方へ出る。
    const float box = std::max({ INLINE_CODE_PAD_Y, theme_->code_block_padding, ALERT_BG_PAD, BAR_EXTEND });
    // コピー/保存ボタンはズームに依らない固定サイズで box 上端から下へ伸びるため、縮小表示の
    // 低いブロックでは box の下へはみ出す。ノード上端からのボタン下端までを下側の上限に含める。
    const float copy_button = COPY_BTN_MARGIN + COPY_BTN_SIZE;
    return { box, std::max({ box, underline, empty_li, copy_button }) };
}

BrushId CommandGenerator::GetNodeBaseBrush(const Node& node) noexcept
{
    switch (node.type) {
    case NodeType::Heading:
        return BrushId::Heading;
    case NodeType::BlockQuote:
        return (node.alert_type != AlertType::None) ? BrushId::Text : BrushId::BlockquoteText;
    case NodeType::CodeBlock:
        return BrushId::CodeText;
    case NodeType::Paragraph:
    case NodeType::HorizontalRule:
    case NodeType::ListItem:
    case NodeType::Table:
    case NodeType::TaskListItem:
    case NodeType::Image:
    case NodeType::Diagram:
        return BrushId::Text;
    }
    std::unreachable();
}

void CommandGenerator::GenNodeTextDecorations(const FrameContext& fc, const Node& node, const NodeLayoutEntry& entry, int node_index, float text_x, float entry_text_top)
{
    if (!entry.text_layout()) {
        return;
    }

    GenInlineCodeBgs(cmds_, entry.view_inline_code_bgs(), text_x, entry_text_top, cull_top_, cull_bottom_);

    // 検索マッチのハイライト（選択より先に描画し、選択が最前面になるようにする）
    GenSearchHighlights(entry, node_index, text_x, entry_text_top);

    if (const auto [sel_start, sel_end] = fc.selection.RangeIn(node_index, node.GetText().size()); sel_end > sel_start) {
        GenSelectionHighlightCached(node, entry, sel_start, sel_end - sel_start, text_x, entry_text_top);
    }

    auto cmd = MakeTextLayoutCmd(D2D1::Point2F(text_x, entry_text_top), entry.text_layout(), entry.height, GetNodeBaseBrush(node), cull_top_, cull_bottom_);
    cmd.syntax_tokens = entry.syntax_tokens();
    cmds_.emplace_back(cmd);
}

void CommandGenerator::GenTaskListCheckbox(const Node& node, float x, float entry_text_top)
{
    const std::wstring_view icon = node.task_checked() ? L"\u2611" : L"\u2610"; // ☑ / ☐
    const float icon_size = theme_->font_size_body;
    const float cb_x = x - theme_->list_bullet_offset;
    EmitText(icon, formats_.icon_font, D2D1::Point2F(cb_x, entry_text_top),
             D2D1::SizeF(icon_size, icon_size * TASK_CHECKBOX_HEIGHT_FACTOR), BrushId::Text);
}

void CommandGenerator::GenHorizontalRule(float x, float w, float entry_text_top)
{
    const float y = entry_text_top + theme_->paragraph_spacing * 0.5f;
    cmds_.emplace_back(DrawLineCmd{
        D2D1::Point2F(x, y), D2D1::Point2F(x + w, y),
        theme_->hr_thickness, BrushId::Hr });
}

void CommandGenerator::GenCodeBlockBg(const D2D1_RECT_F& box, bool round_top, bool round_bottom)
{
    if (!round_top && !round_bottom) {
        cmds_.emplace_back(FillRectCmd{ box, BrushId::CodeBg });
        return;
    }
    cmds_.emplace_back(FillRoundedRectCmd{ box, CODE_BLOCK_CORNER, CODE_BLOCK_CORNER, BrushId::CodeBg });
    // 背景は不透明なので、丸めない側の角を矩形で塗りつぶす。
    if (!round_top) {
        cmds_.emplace_back(FillRectCmd{ D2D1::RectF(box.left, box.top, box.right, box.top + CODE_BLOCK_CORNER), BrushId::CodeBg });
    }
    if (!round_bottom) {
        cmds_.emplace_back(FillRectCmd{ D2D1::RectF(box.left, box.bottom - CODE_BLOCK_CORNER, box.right, box.bottom), BrushId::CodeBg });
    }
}

void CommandGenerator::EmitBlockHScrollbarIfActive(const FrameContext& fc, int node_index, float block_x, float bar_y, const BlockHScrollGeometry& geom, float scroll_x)
{
    if (!geom.can_scroll()) {
        return;
    }
    if (node_index != fc.h_scroll.hovered_block && node_index != fc.h_scroll.drag_block) {
        return;
    }
    const float track_w = geom.visible_width;
    const float thumb_w = BlockHScrollbarThumbWidth(geom.visible_width, geom.block_width);
    const float scroll_max = geom.scroll_max();
    const float ratio = std::clamp(scroll_x / scroll_max, 0.0f, 1.0f);
    // サブピクセル位置だとアンチエイリアスが上下に漏れて thumb の太さがブレるため snap する。
    const float thumb_x = SnapToPhysicalPixel(block_x + ratio * (track_w - thumb_w), fc.dpi_scale);
    const float thumb_y = SnapToPhysicalPixel(bar_y, fc.dpi_scale);
    const float w_snapped = SnapToPhysicalPixel(thumb_w, fc.dpi_scale);
    const float h_snapped = SnapToPhysicalPixel(PANE_SCROLLBAR_WIDTH, fc.dpi_scale);
    cmds_.emplace_back(FillRoundedRectCmd{
        D2D1::RectF(thumb_x, thumb_y, thumb_x + w_snapped, thumb_y + h_snapped),
        h_snapped / 2.0f, h_snapped / 2.0f,
        BrushId::ScrollbarThumb });
}

void CommandGenerator::GenOverlayButtons(const FrameContext& fc, const Node& node, const DiagramEntry* diagram, float x, float cw, float entry_text_top, int node_index)
{
    layout::ForEachOverlayButton(node, diagram, x, cw, entry_text_top, *theme_, [&](const D2D1_RECT_F& btn, OverlayButton kind) {
        // 保存は Download、コピーは Copy (Segoe Fluent Icons)。
        const wchar_t icon = kind == OverlayButton::DiagramSave ? L'\uE896' : L'\uE8C8';
        GenOverlayButton(btn, icon, fc.hovered == OverlayButtonHit{ node_index, kind });
    });
}

void CommandGenerator::GenOverlayButton(D2D1_RECT_F btn, wchar_t icon, bool is_hovered)
{
    if (!formats_.copy_btn_icon) {
        return;
    }
    const bool dark = theme_->IsDark();
    const BrushId overlay = OverlayBrushId(dark);
    const float bg_alpha = is_hovered ? (dark ? 0.30f : 0.15f) : (dark ? 0.10f : 0.05f);
    cmds_.emplace_back(FillRoundedRectCmd{ btn, COPY_BTN_CORNER, COPY_BTN_CORNER, overlay, bg_alpha });

    const float text_alpha = is_hovered ? (dark ? 0.9f : 0.8f) : (dark ? 0.4f : 0.35f);
    EmitText({ &icon, 1 }, formats_.copy_btn_icon, D2D1::Point2F(btn.left, btn.top),
             D2D1::SizeF(COPY_BTN_SIZE, COPY_BTN_SIZE), overlay, text_alpha);
}

void CommandGenerator::GenListBullet(const FrameContext& fc, const Node& node, const NodeLayoutEntry& entry, float x, float entry_text_top)
{
    const float first_line_h = GetFirstLineHeight(entry, theme_->font_size_body);
    if (node.list_ordered()) {
        wchar_t num_buf[16];
        const auto fmt_result = std::format_to_n(num_buf, std::size(num_buf), L"{}.", node.list_number());
        const size_t num_len = std::min(static_cast<size_t>(fmt_result.size), std::size(num_buf));
        EmitText({ num_buf, num_len }, formats_.list_number,
                 D2D1::Point2F(x - theme_->list_bullet_offset - LIST_NUMBER_PAD_RIGHT, entry_text_top),
                 D2D1::SizeF(theme_->list_bullet_offset + LIST_NUMBER_PAD_RIGHT - LIST_NUMBER_PAD_LEFT, first_line_h),
                 BrushId::Text);
        return;
    }

    const float bullet_x = SnapToPhysicalPixel(x - theme_->list_bullet_offset * LIST_BULLET_X_FACTOR, fc.dpi_scale);
    const float bullet_y = SnapToPhysicalPixel(entry_text_top + first_line_h * 0.5f, fc.dpi_scale);
    const float r = theme_->list_bullet_radius;
    if (node.indent_level <= 1) {
        cmds_.emplace_back(FillEllipseCmd{ D2D1::Point2F(bullet_x, bullet_y), r, r, BrushId::Text });
    }
    else {
        cmds_.emplace_back(DrawEllipseCmd{ D2D1::Point2F(bullet_x, bullet_y), r, r, 1.0f, BrushId::Text });
    }
}

void CommandGenerator::GenBlockQuoteGroupDecorations(const FrameContext& fc, const std::pmr::vector<Node>& nodes, const LayoutCache& cache, int first_visible)
{
    const float snap = fc.snapped_scroll_y;
    const int node_count = static_cast<int>(nodes.size());

    // バーと背景はクリップされるので、グループの可視範囲外の部分は走査しない (巨大な引用でも
    // 可視ノード数ぶんで済む)。first_visible がグループ途中なら、可視範囲より上にある直前の
    // 1 ノードから始めれば、上から続くバーと背景が可視範囲の上端より外から描かれる。
    int i = first_visible;
    if (i > 0 && i < node_count && nodes[i].blockquote_group >= 0 && nodes[i - 1].blockquote_group == nodes[i].blockquote_group) {
        --i;
    }

    while (i < node_count) {
        const int group = nodes[i].blockquote_group;
        if (group < 0) {
            // text_top は単調なので、非引用ノードが下端を超えたら以降のグループも全て可視域外。
            // ここで break しないと引用が可視域以降に無い文書で毎フレーム末尾まで全走査する。
            if (cache.Top(i) - snap > cull_bottom_) {
                break;
            }
            i++;
            continue;
        }
        const float group_top = cache.Top(i) - snap;
        if (group_top - fc.overhang.above > cull_bottom_) {
            break;
        }

        float group_bottom = group_top + cache[i].height;
        int max_depth = nodes[i].quote_depth;

        // 可視範囲の下端を越えたノードまで含めれば、下へ続くバーと背景は可視範囲の外まで伸びる。
        int j = i + 1;
        while (j < node_count && nodes[j].blockquote_group == group) {
            group_bottom = std::max(group_bottom, cache.Bottom(j) - snap);
            max_depth = std::max<int>(max_depth, nodes[j].quote_depth);
            j++;
            if (group_bottom > cull_bottom_) {
                break;
            }
        }

        if (max_depth > 0 && nodes[i].quote_outer_indent > 0) {
            GenBlockQuoteGroup(fc, nodes, cache, i, j, max_depth, group_top, group_bottom);
        }
        // 下端で打ち切ったグループの残りを別グループとして描かない。
        if (j < node_count && nodes[j].blockquote_group == group) {
            break;
        }
        i = j;
    }
}

void CommandGenerator::GenBlockQuoteGroup(const FrameContext& fc, const std::pmr::vector<Node>& nodes, const LayoutCache& cache, int first, int last, int max_depth, float group_top, float group_bottom)
{
    static constexpr float ALERT_BG_CORNER = 4.0f;

    const AlertType alert_type = nodes[first].alert_type;
    const int outer_indent = nodes[first].quote_outer_indent;
    const float outer_x_indent = static_cast<float>(outer_indent) * theme_->indent_width;
    if (const auto idx = AlertColorIndex(alert_type); idx < ALERT_TYPE_COUNT) {
        const float outer_x = fc.offset_x + outer_x_indent;
        const float cw = fc.content_width - outer_x_indent;
        const D2D1_RECT_F bg_rect = D2D1::RectF(outer_x - ALERT_BG_PAD, group_top - ALERT_BG_PAD, outer_x + cw, group_bottom + ALERT_BG_PAD);
        cmds_.emplace_back(FillRoundedRectCmd{ bg_rect, ALERT_BG_CORNER, ALERT_BG_CORNER, AlertBgBrushIdFromIndex(idx) });
    }

    // GitHub と同様に、外側のバーはネストした範囲を貫通して連続描画する。
    // 各 level ごとに quote_depth >= level の連続区間を求めてバーを発行。
    for (int level = 1; level <= max_depth; ++level) {
        const float bar_indent_x = static_cast<float>(outer_indent + level - 1) * theme_->indent_width;
        const float bar_x = fc.offset_x + bar_indent_x - theme_->indent_width * 0.5f;
        const bool is_alert_bar = (level == 1 && alert_type != AlertType::None);
        const BrushId bar_brush = is_alert_bar ? AlertBrushIdFromIndex(AlertColorIndex(alert_type)) : BrushId::BlockquoteBar;

        const auto emit_bar = [&](float top, float bottom) {
            cmds_.emplace_back(DrawLineCmd{
                D2D1::Point2F(bar_x, top - BAR_EXTEND),
                D2D1::Point2F(bar_x, bottom + BAR_EXTEND),
                theme_->blockquote_bar_width, bar_brush });
        };

        bool in_region = false;
        float region_top = 0.0f;
        float region_bottom = 0.0f;
        for (int k = first; k < last; ++k) {
            const float local_top_k = cache.Top(k) - fc.snapped_scroll_y;
            if (nodes[k].quote_depth >= level) {
                if (!in_region) {
                    in_region = true;
                    region_top = local_top_k;
                }
                region_bottom = local_top_k + cache[k].height;
            }
            else if (in_region) {
                emit_bar(region_top, region_bottom);
                in_region = false;
            }
        }
        if (in_region) {
            emit_bar(region_top, region_bottom);
        }
    }
}

void CommandGenerator::GenDiagramPlaceholder(float x, float y, float w, float h, std::wstring_view error)
{
    GenCodeBlockBg(D2D1::RectF(x, y, x + w, y + h), true, true);
    const auto origin = D2D1::Point2F(x, y);
    const auto box = D2D1::SizeF(w, h);
    if (!error.empty()) {
        EmitText(error, formats_.placeholder_text, origin, box, BrushId::AlertCaution);
        return;
    }
    EmitText(i18n::S().loading, formats_.placeholder_text, origin, box, BrushId::BlockquoteText);
}

void CommandGenerator::GenTableRowBg(const TableRowGeom& g, bool is_header, bool is_even_row)
{
    if (!is_header && !is_even_row) {
        return;
    }
    const D2D1_RECT_F rect = D2D1::RectF(g.x, g.y, g.x + g.table_width, g.y + g.row_h + g.border);
    if (is_header) {
        cmds_.emplace_back(FillRectCmd{ rect, BrushId::CodeBg });
    }
    else {
        cmds_.emplace_back(FillRectCmd{ rect, BrushId::TableStripe });
    }
}

void CommandGenerator::GenTableCellContent(std::string_view cell_text, const CellDrawContext& ctx)
{
    if (!ctx.layout) {
        return;
    }
    if (ctx.selection.end > ctx.selection.start) {
        const uint32_t cell_len = static_cast<uint32_t>(cell_text.size());
        const uint32_t ov_start = std::max(ctx.selection.start, ctx.flat_offset);
        const uint32_t ov_end = std::min(ctx.selection.end, ctx.flat_offset + cell_len);
        if (ov_end > ov_start) {
            // sel_start/sel_end は UTF-8 byte offset。HitTestTextRange 用に UTF-16 へ変換する。
            Utf16OffsetCursor cursor{ cell_text };
            const auto range = cursor.WideRange(ov_start - ctx.flat_offset, ov_end - ov_start);
            if (range.length > 0) {
                GenSelectionHighlight(ctx.layout, range.startPosition, range.length, ctx.text_x, ctx.text_y);
            }
        }
    }
    const BrushId cell_brush = ctx.is_header ? BrushId::Heading : BrushId::Text;
    cmds_.emplace_back(MakeTextLayoutCmd(D2D1::Point2F(ctx.text_x, ctx.text_y), ctx.layout, ctx.height, cell_brush, cull_top_, cull_bottom_));
}

void CommandGenerator::GenTable(const FrameContext& fc,
    const Node& node, const NodeLayoutEntry& entry,
    int node_index, float offset_x, float entry_text_top, float h_scroll_x)
{
    const auto* tbl = node.table_data();
    const auto* tl_ptr = entry.table_layout();
    if (!tbl || tbl->row_count == 0 || !tl_ptr || !tl_ptr->HasGeometry(tbl->row_count, tbl->col_count)) {
        return;
    }

    const float border = TABLE_BORDER_WIDTH;
    const auto& tl = *tl_ptr;
    const auto row_count = tbl->row_count;
    const auto col_count = static_cast<size_t>(tbl->col_count);

    const float table_width = tl.TableWidth();
    // 横スクロール時は画面に映る範囲が h_scroll_x だけ右にずれる。
    const float cull_left = offset_x + fc.viewport_left + h_scroll_x;
    const float cull_right = offset_x + fc.viewport_right + h_scroll_x;

    const auto selection = fc.selection.RangeIn(node_index, node.GetText().size());

    // 可視行帯を row_cum_y の二分探索で求め、その範囲だけループする
    // (FindTableRow と同じパターン)。巨大テーブルで
    // 毎フレーム row_count 回の continue ループが走るのを防ぐ。
    const auto [r_begin, r_end] = tl.VisibleRowRange(cull_top_ - entry_text_top, cull_bottom_ - entry_text_top);
    float y = entry_text_top + tl.row_cum_y[r_begin];

    // x は列順に単調なので、可視列は連続した区間 [c_begin, c_end) になる。
    const auto cum_begin = tl.col_cum_x.begin();
    const auto cum_cols_end = cum_begin + static_cast<ptrdiff_t>(col_count);
    const size_t c_begin = static_cast<size_t>(std::lower_bound(cum_begin + 1, cum_cols_end + 1, cull_left - offset_x + border) - (cum_begin + 1));
    const size_t c_end = static_cast<size_t>(std::upper_bound(cum_begin + static_cast<ptrdiff_t>(c_begin), cum_cols_end, cull_right - offset_x + border) - cum_begin);

    const auto search_nm = PrepareNodeSearchMatches(entry, node_index);

    for (size_t r = r_begin; r < r_end; r++) {
        const float row_h = tl.row_heights[r];
        const bool is_header_row = tbl->IsHeaderRow(r);
        GenTableRowBg(TableRowGeom{ offset_x, y, table_width, row_h, border }, is_header_row, r % 2 == 0);

        cmds_.emplace_back(DrawLineCmd{ D2D1::Point2F(offset_x, y), D2D1::Point2F(offset_x + table_width, y), border, BrushId::Hr });

        std::span<const CellInlineCodeBg> row_bgs;
        if (r < tl.row_code_bgs.size()) {
            row_bgs = tl.row_code_bgs[r];
        }
        for (size_t c = c_begin; c < c_end; c++) {
            const auto [text_x, text_y] = TableCellTextOrigin(offset_x + tl.col_cum_x[c], y);

            GenCellInlineCodeBgs(cmds_, row_bgs, c, text_x, text_y);

            IDWriteTextLayout* const cell_layout = tl.GetCellLayout(r, c);
            GenCellSearchHighlights(search_nm, cell_layout, r, c, text_x, text_y);

            GenTableCellContent(tbl->GetCellText(r, c), CellDrawContext{
                .layout = cell_layout,
                .text_x = text_x,
                .text_y = text_y,
                .height = row_h,
                .flat_offset = tbl->CellTextStart(r, c),
                .selection = selection,
                .is_header = is_header_row,
            });
        }
        y += row_h + border;
    }

    // r_end で打ち切った場合も下端線はテーブル全体の底に置く (可視外ならクリップされる)
    const float table_bottom = entry_text_top + tl.row_cum_y[row_count];
    cmds_.emplace_back(DrawLineCmd{ D2D1::Point2F(offset_x, table_bottom), D2D1::Point2F(offset_x + table_width, table_bottom), border, BrushId::Hr });

    // 縦罫線は可視列ごとに表の上端から下端まで 1 本で引く (行ごとに引くと 行×列 本になる)。可視外はクリップされる。
    const auto vline = [&](float x) {
        cmds_.emplace_back(DrawLineCmd{ D2D1::Point2F(x, entry_text_top), D2D1::Point2F(x, table_bottom), border, BrushId::Hr });
    };
    for (size_t c = c_begin; c < c_end; c++) {
        vline(offset_x + tl.col_cum_x[c] - border);
    }
    vline(offset_x + table_width);
}

std::span<const DWRITE_HIT_TEST_METRICS> CommandGenerator::HitTestRange(IDWriteTextLayout* layout, uint32_t start, uint32_t length)
{
    MENDO_COUNT_INC(g_cmd_gen_stats.hittest_range);
    const UINT32 count = FetchHitTestMetrics(layout, start, length, hit_test_buffer_);
    return { hit_test_buffer_.data(), count };
}

void CommandGenerator::EmitFillRectIfVisible(const D2D1_RECT_F& r, BrushId brush)
{
    if (OverlapsY(r, cull_top_, cull_bottom_)) {
        cmds_.emplace_back(FillRectCmd{ r, brush });
    }
}

void CommandGenerator::GenSelectionHighlight(IDWriteTextLayout* layout, uint32_t start, uint32_t length, float origin_x, float origin_y)
{
    if (!layout || length == 0) {
        return;
    }
    for (const auto& m : HitTestRange(layout, start, length)) {
        EmitFillRectIfVisible(RectFromHitTest(m, origin_x, origin_y), BrushId::Selection);
    }
}

void CommandGenerator::CollectHitTestRects(IDWriteTextLayout* layout, uint32_t start, uint32_t length, std::pmr::vector<D2D1_RECT_F>& out)
{
    // 少しずつ追記されるので reserve はしない (要求ぴったりの再確保が毎回走る)。
    for (const auto& m : HitTestRange(layout, start, length)) {
        out.emplace_back(RectFromHitTest(m));
    }
}

const LineWindow& CommandGenerator::LineWindowAround(const NodeLayoutEntry& entry, float origin_y) const
{
    return EnsureLineWindow(entry, { cull_top_ - origin_y, cull_bottom_ - origin_y });
}

void CommandGenerator::GenSelectionHighlightCached(const Node& node, const NodeLayoutEntry& entry, uint32_t doc_start, uint32_t doc_length, float origin_x, float origin_y)
{
    auto* layout = entry.text_layout();
    if (!layout || doc_length == 0) {
        return;
    }
    auto& cache = entry.ensure_selection_hl_cache();
    // キャッシュキーは UTF-8 byte 単位 (selection 状態と直接対応)。
    // 位置変換は選択が変わったときだけ前方走査で行う。
    if (cache.layout_ptr != layout || cache.start != doc_start || cache.length != doc_length) {
        Utf16OffsetCursor cursor{ node.GetText() };
        const auto wr = cursor.WideRange(doc_start, doc_length);
        if (cache.layout_ptr != layout) {
            cache.rects.clear();
            cache.rects_begin_w = 0;
            cache.rects_end_w = 0;
        }
        cache.layout_ptr = layout;
        cache.start = doc_start;
        cache.length = doc_length;
        cache.start_w = wr.startPosition;
        cache.end_w = wr.startPosition + wr.length;
    }
    // 背の高いノードでは選択範囲を可視範囲の前後の行に絞って引き、全行選択でも矩形を可視行数ぶんで済ませる。
    const auto& window = LineWindowAround(entry, origin_y);
    uint32_t begin = std::max(cache.start_w, window.text_begin);
    uint32_t end = std::min(cache.end_w, window.text_end);
    if (end <= begin) {
        begin = end = 0;
    }
    if (begin != cache.rects_begin_w || end != cache.rects_end_w) {
        MENDO_COUNT_INC(g_cmd_gen_stats.sel_hl_cache_miss);
        cache.rects.clear();
        if (end > begin) {
            CollectHitTestRects(layout, begin, end - begin, cache.rects);
        }
        cache.rects_begin_w = begin;
        cache.rects_end_w = end;
    }
    else {
        MENDO_COUNT_INC(g_cmd_gen_stats.sel_hl_cache_hit);
    }
    for (const auto& local : cache.rects) {
        EmitFillRectIfVisible(OffsetRectF(local, origin_x, origin_y), BrushId::Selection);
    }
}

CommandGenerator::NodeSearchMatches CommandGenerator::PrepareNodeSearchMatches(const NodeLayoutEntry& entry, int node_index)
{
    if (!search_matches_ || search_matches_->empty()) {
        return {};
    }
    const std::span<const SearchMatch> matches = *search_matches_;
    const auto range = std::ranges::equal_range(matches, node_index, {}, &SearchMatch::node_index);
    if (range.empty()) {
        return {};
    }
    const size_t first_global = static_cast<size_t>(range.begin() - matches.begin());

    // layout 変更時は invalidate_search_hl_cache() でキャッシュ自体が破棄されており、SearchState の
    // generation は 1 から始まるため、cache.gen == search_generation_ のみでキャッシュ有効性を判定できる。
    auto& cache = entry.ensure_search_hl_cache();
    if (cache.gen != search_generation_) {
        MENDO_COUNT_INC(g_cmd_gen_stats.search_hl_reset);
        cache.rects.clear();
        cache.spans.assign(range.size(), {});
        cache.gen = search_generation_;
    }
    return { matches.subspan(first_global, range.size()), first_global, &cache };
}

void CommandGenerator::EmitSearchMatches(const NodeSearchMatches& nm, IDWriteTextLayout* layout, size_t begin, size_t end, float origin_x, float origin_y)
{
    auto& cache = *nm.cache;
    for (size_t i = begin; i < end; ++i) {
        auto& span = cache.spans[i];
        if (span.count == SearchHlCache::Span::NOT_COMPUTED) {
            // evict 中のセルは未計算のまま残し、layout が復元されてから計算する。
            if (!layout) {
                MENDO_COUNT_INC(g_cmd_gen_stats.search_hl_provisional);
                continue;
            }
            MENDO_COUNT_INC(g_cmd_gen_stats.search_hl_computed);
            const auto& m = nm.matches[i];
            span.begin = static_cast<uint32_t>(cache.rects.size());
            if (m.length_w > 0) {
                CollectHitTestRects(layout, m.start_w, m.length_w, cache.rects);
            }
            span.count = static_cast<uint32_t>(cache.rects.size()) - span.begin;
        }

        const bool is_current = (static_cast<int>(nm.first_global + i) == current_match_index_);
        const BrushId hl_brush = is_current ? BrushId::SearchHighlightCurrent : BrushId::SearchHighlight;
        for (const auto& r : std::span{ cache.rects }.subspan(span.begin, span.count)) {
            EmitFillRectIfVisible(OffsetRectF(r, origin_x, origin_y), hl_brush);
        }
    }
}

void CommandGenerator::GenSearchHighlights(const NodeLayoutEntry& entry, int node_index, float origin_x, float origin_y)
{
    const auto nm = PrepareNodeSearchMatches(entry, node_index);
    if (nm.matches.empty()) {
        return;
    }
    // マッチは start_w 昇順で重ならないので、終端も昇順に並ぶ。
    const auto& window = LineWindowAround(entry, origin_y);
    const auto first = std::ranges::partition_point(nm.matches, [b = window.text_begin](const SearchMatch& m) noexcept {
        return m.start_w + m.length_w <= b;
    });
    const auto last = std::ranges::partition_point(first, nm.matches.end(), [e = window.text_end](const SearchMatch& m) noexcept {
        return m.start_w < e;
    });
    EmitSearchMatches(nm, entry.text_layout(),
        static_cast<size_t>(first - nm.matches.begin()), static_cast<size_t>(last - nm.matches.begin()), origin_x, origin_y);
}

void CommandGenerator::GenCellSearchHighlights(const NodeSearchMatches& nm, IDWriteTextLayout* layout, size_t row, size_t col, float origin_x, float origin_y)
{
    if (nm.matches.empty()) {
        return;
    }
    // テーブル内 match は (row, col, start) 昇順なので、セル範囲を二分探索で切り出す。
    const auto cell = std::ranges::equal_range(
        nm.matches, std::pair{ static_cast<int>(row), static_cast<int>(col) }, {},
        [](const SearchMatch& m) static noexcept { return std::pair{ m.table_row, m.table_col }; });
    EmitSearchMatches(nm, layout,
        static_cast<size_t>(cell.begin() - nm.matches.begin()), static_cast<size_t>(cell.end() - nm.matches.begin()), origin_x, origin_y);
}
