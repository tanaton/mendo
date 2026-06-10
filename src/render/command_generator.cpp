#include "command_generator.h"
#include "i18n.h"
#include "layout.h"
#include "layout_computer.h"
#include "profiler.h"
#include "ui_constants.h"
#include <algorithm>
#include <format>
#include <ranges>
#include <utility>

#ifdef MENDO_USE_TRACY
namespace {

// 累積カウンタ + 直近フレーム値（UI スレッド単一前提のため非アトミック）。
struct CmdGenStats {
    int64_t hittest_range = 0;           // FetchHitTestMetrics (HitTestTextRange) 呼び出し
    int64_t sel_hl_cache_hit = 0;        // SelectionHlCache ヒット
    int64_t sel_hl_cache_miss = 0;       // SelectionHlCache ミス (HitTestTextRange を回す)
    int64_t search_hl_rebuild = 0;       // SearchHlCache 再構築
    int64_t search_hl_provisional = 0;   // 表セルレイアウト失効中の暫定空 rect プッシュ (次フレーム再構築待ち)
    int64_t last_visible_node_count = 0; // 累積ではなく直近フレームのスナップショット
};
CmdGenStats g_cmd_gen_stats;

void PublishCmdGenStats() noexcept
{
    MENDO_PLOT("cmdgen.hittest_range", g_cmd_gen_stats.hittest_range);
    MENDO_PLOT("cmdgen.sel_hl_cache_hit", g_cmd_gen_stats.sel_hl_cache_hit);
    MENDO_PLOT("cmdgen.sel_hl_cache_miss", g_cmd_gen_stats.sel_hl_cache_miss);
    MENDO_PLOT("cmdgen.search_hl_rebuild", g_cmd_gen_stats.search_hl_rebuild);
    MENDO_PLOT("cmdgen.search_hl_provisional", g_cmd_gen_stats.search_hl_provisional);
    MENDO_PLOT("cmdgen.visible_node_count", g_cmd_gen_stats.last_visible_node_count);
}

} // namespace
#endif

// h1/h2 見出し下線を描画するY座標を heading_spacing_below_h1h2 のこの比率で決める。
// 値を小さくすると下線は見出し文字に近づき、下線と次行の間隔が広がる。
static constexpr float HEADING_UNDERLINE_OFFSET_RATIO = 0.25f;

namespace {

// ブロックローカル横スクロールの clip と Translate を RAII で囲み、push/pop の対称性を強制する。
// scroll_x == 0 のときは Translate を省略し、自然幅がペイン幅以下のときは Clip も省略する。
// 自然幅が visible_width を超えるが scroll_x == 0 の場合 (初期状態) は、依然として右端で
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

} // namespace

// DWRITE_HIT_TEST_METRICS を origin 加算付きの D2D1_RECT_F に変換する。
static inline D2D1_RECT_F RectFromHitTest(const DWRITE_HIT_TEST_METRICS& m, float origin_x = 0.0f, float origin_y = 0.0f) noexcept
{
    return D2D1::RectF(
        origin_x + m.left,
        origin_y + m.top,
        origin_x + m.left + m.width,
        origin_y + m.top + m.height);
}

// MeasureNode 時に確定済みの first_line_height を優先利用する。
// 0（未確定）なら DirectWrite に問い合わせず font_size ベースのフォールバックで済ませ、
// 描画ホットパスから COM 越境呼び出しを排除する。
static float GetFirstLineHeight(const NodeLayoutEntry& entry, float font_size) noexcept
{
    return (entry.first_line_height > 0.0f) ? entry.first_line_height : font_size * FALLBACK_LINE_HEIGHT_FACTOR;
}

const DrawCommandList& CommandGenerator::GenerateMdPane(
    const std::pmr::vector<Node>& nodes, const LayoutCache& cache,
    const PaneRect& md_pane_rect, float scroll_y,
    const TextSelection& selection,
    int first_visible,
    HoveredButtons hovered,
    float dpi_scale,
    const BlockHScrollContext& block_h_scroll)
{
    MENDO_PROFILE("GenerateMdPane");
    // ムーブ代入で内部 pmr::vector の data ptr を捨ててから arena をリセットする。
    // clear() だけだと dangling な data ptr が残り、次フレームの push_back が
    // arena 上で別系統 allocate と衝突して UAF を起こす。
    cmds_ = DrawCommandList{ frame_resource_.resource() };
    frame_resource_.Reset();
    auto& cmds = cmds_;
    // 倍々成長による monotonic 死蔵を抑える。+16 は last_cmds_size_<8 で 12.5% が 0 に丸まる
    // 小規模フレーム向けの下駄。
    if (last_cmds_size_ > 0) {
        cmds.reserve(last_cmds_size_ + last_cmds_size_ / 8 + 16);
    }

    const D2D1_RECT_F md_clip = D2D1::RectF(
        md_pane_rect.x, md_pane_rect.y,
        md_pane_rect.x + md_pane_rect.width, md_pane_rect.y + md_pane_rect.height);
    cmds.emplace_back(PushClipCmd{ md_clip });
    // スクロール位置を物理ピクセル境界にスナップし、ClearTypeヒンティングの
    // フレーム間変動によるテキストのガタつきを防止する。スナップは描画 Y のみに
    // 適用し、二分探索 (FindFirstVisibleNodeIndex) には未スナップ scroll_y を渡す。
    const float snapped_y = SnapToPhysicalPixel(scroll_y, dpi_scale);

    // Y 平行移動は Transform に乗せず CPU 側で entry ごとに加算する。
    // 大規模ファイルで text_top/scroll_y が 10^7 DIP オーダーになると、D2D 内部の
    // float32 行列演算で catastrophic cancellation が起きるため (詳細は header)。
    const auto pane_transform = D2D1::Matrix3x2F::Translation(md_pane_rect.x, 0.0f);
    cmds.emplace_back(SetTransformCmd{ pane_transform });

    const FrameContext fc{
        .offset_x = theme_->margin_left,
        .viewport_top = 0.0f,
        .viewport_bottom = md_pane_rect.height,
        // 水平カリング範囲: ペイン内ローカル座標で margin_left を起点とした相対値
        .viewport_left = -theme_->margin_left,
        .viewport_right = md_pane_rect.width - theme_->margin_left,
        .content_width = theme_->ContentWidth(md_pane_rect.width),
        .dpi_scale = dpi_scale,
        .md_pane_x = md_pane_rect.x,
        .snapped_scroll_y = snapped_y,
        .pane_transform = pane_transform,
        .selection = selection,
        .hovered = hovered,
        .h_scroll = block_h_scroll,
    };

    // SelectionHlCache は lazy 確保のみで自動破棄経路が無く、選択範囲外に出たノード分が
    // 居残ってメモリが漸増する。前フレームの範囲との差分で、外れたノードを巻き戻す。
    {
        const int new_start = selection.active ? selection.start_node : -1;
        const int new_end = selection.active ? selection.end_node : -1;
        if ((new_start != prev_sel_start_node_ || new_end != prev_sel_end_node_) && prev_sel_start_node_ >= 0) {
            const int upper = std::min(prev_sel_end_node_, static_cast<int>(cache.size()) - 1);
            for (int i = prev_sel_start_node_; i <= upper; i++) {
                if (new_start < 0 || i < new_start || i > new_end) {
                    cache[i].invalidate_selection_hl_cache();
                }
            }
        }
        prev_sel_start_node_ = new_start;
        prev_sel_end_node_ = new_end;
    }
    // ドキュメント切替で string_view が dangling 化するため reset する。PMR pool が
    // 同じアドレスを再利用しうるので (data, size) の両方で同一性を判定する。
    node_wv_.ResetIfBufferChanged(nodes.data(), nodes.size());
    cell_wv_.ResetIfBufferChanged(nodes.data(), nodes.size());

    const int node_count = static_cast<int>(nodes.size());
    if (first_visible < 0) {
        // 二分探索キーは未スナップのドキュメント Y (cache[i].text_top と同じ系)。
        first_visible = FindFirstVisibleNodeIndex(cache, nodes.size(), scroll_y);
    }

    // 同一 blockquote_group のノードをまとめてバー/背景を描画する。
    // ノード単体ごとではなく一括で描画することで、複数行の引用ブロックが連続した見た目になる。
    GenBlockQuoteGroupDecorations(cmds, fc, nodes, cache, node_count, first_visible);

    int visible_count = 0;
    for (int i = first_visible; i < node_count; i++) {
        const float local_text_top = cache[i].text_top - snapped_y;
        if (local_text_top > fc.viewport_bottom) {
            break;
        }
        GenerateNode(cmds, fc, nodes[i], cache[i], cache.GetDiagram(i), i, local_text_top);
        ++visible_count;
    }

    cmds.emplace_back(SetTransformCmd{ D2D1::Matrix3x2F::Identity() });
    cmds.emplace_back(PopClipCmd{});
    last_cmds_size_ = cmds_.size();
    MENDO_COUNT_SET(g_cmd_gen_stats.last_visible_node_count, visible_count);
    MENDO_IF_TRACY(PublishCmdGenStats());
    return cmds_;
}

void CommandGenerator::GenerateNode(
    DrawCommandList& cmds,
    const FrameContext& fc,
    const Node& node, const NodeLayoutEntry& entry, const DiagramEntry& diagram,
    int node_index, float entry_text_top)
{
    // h1/h2は見出し下線がentry.heightの外に描画されるため、カリング境界を拡張する。
    float node_bottom = entry_text_top + entry.height;
    if (node.type == NodeType::Heading) {
        const int8_t lv = node.heading_level();
        if (lv <= 2) {
            node_bottom += theme_->heading_spacing_below_h1h2 * HEADING_UNDERLINE_OFFSET_RATIO + theme_->GetHeadingUnderlineThickness(lv);
        }
    }
    // 空 LI は height=0 だが bullet/checkbox は entry_text_top より下に描かれるため、
    // node_bottom を実描画下端まで拡張しないと viewport 上端で一瞬消える。
    // TaskListItem の checkbox (1.5x) は ListItem の bullet 1 行分 (1.3x) より背が高い。
    else if (IsEmptyListItemContainer(node)) {
        const float factor = (node.type == NodeType::TaskListItem)
                                 ? TASK_CHECKBOX_HEIGHT_FACTOR
                                 : FALLBACK_LINE_HEIGHT_FACTOR;
        node_bottom += theme_->font_size_body * factor;
    }
    if (node_bottom < fc.viewport_top || entry_text_top > fc.viewport_bottom) {
        return;
    }

    const float indent = NodeIndent(node, *theme_);
    const float x = fc.offset_x + indent;
    const float cw = fc.content_width - indent;
    const float text_x = x + NodeTextXOffset(node, *theme_);

    switch (node.type) {
    case NodeType::HorizontalRule:
        GenHorizontalRule(cmds, entry, x, cw, entry_text_top);
        return;

    case NodeType::Image:
        if (diagram.bitmap) {
            const float draw_h = entry.height;
            const float draw_w = (diagram.height > 0) ? diagram.width * (draw_h / diagram.height) : diagram.width;
            const float dx = x;
            cmds.emplace_back(DrawBitmapCmd{ diagram.bitmap.Get(), D2D1::RectF(dx, entry_text_top, dx + draw_w, entry_text_top + draw_h) });
        }
        else {
            GenDiagramPlaceholder(cmds, x, entry_text_top, cw, entry.height);
        }
        return;

    case NodeType::Table: {
        // 幾何はヒットテスト/reducer と共有の GetBlockHScrollGeometry に寄せる
        const auto geom = GetBlockHScrollGeometry(node, entry, cw);
        const float scroll_x = geom.can_scroll() ? std::clamp(fc.h_scroll.GetScrollX(node_index), 0.0f, geom.scroll_max()) : 0.0f;
        {
            BlockHScrollScope guard(
                cmds,
                D2D1::RectF(x, entry_text_top, x + cw, entry_text_top + entry.height),
                fc.pane_transform, scroll_x, geom.can_scroll());
            GenTable(cmds, fc, node, entry, node_index, x, entry_text_top, scroll_x);
        }
        EmitBlockHScrollbarIfActive(cmds, fc, node_index, x, BlockHScrollbarBarY(entry_text_top, entry.height, 0.0f), geom, scroll_x);
        return;
    }

    case NodeType::CodeBlock: {
        const auto lang = node.code_language();
        if (IsDiagramLanguage(lang)) {
            if (diagram.bitmap) {
                const auto bmp = MermaidBitmapRect(diagram.width, diagram.height, x, cw, entry_text_top);
                cmds.emplace_back(DrawBitmapCmd{ diagram.bitmap.Get(), bmp });
                GenSaveButton(cmds, bmp.right, bmp.top, node_index == fc.hovered.save);
                if (IsSvgExportable(lang)) {
                    GenSvgCopyButton(cmds, bmp.right, bmp.top, node_index == fc.hovered.svg_copy);
                }
            }
            else {
                GenDiagramPlaceholder(cmds, x, entry_text_top, cw, entry.height);
            }
            return;
        }
        GenCodeBlockBg(cmds, entry, x, cw, entry_text_top);
        // 背景とコピーボタンはクリップ外で固定描画 (GitHub と同じ挙動)。テキスト本体だけ scroll_x 分平行移動。
        {
            const auto geom = GetBlockHScrollGeometry(node, entry, cw);
            const float scroll_x = geom.can_scroll() ? std::clamp(fc.h_scroll.GetScrollX(node_index), 0.0f, geom.scroll_max()) : 0.0f;
            const float pad = theme_->code_block_padding;
            {
                BlockHScrollScope guard(
                    cmds,
                    D2D1::RectF(x, entry_text_top - pad, x + cw, entry_text_top + entry.height + pad),
                    fc.pane_transform, scroll_x, geom.can_scroll());
                GenNodeTextDecorations(cmds, fc, node, entry, node_index, x, text_x, entry_text_top);
            }
            EmitBlockHScrollbarIfActive(cmds, fc, node_index, x, BlockHScrollbarBarY(entry_text_top, entry.height, pad), geom, scroll_x);
        }
        GenCopyButton(cmds, entry, x, cw, node_index == fc.hovered.copy, entry_text_top);
        return;
    }

    case NodeType::ListItem:
        GenListBullet(cmds, fc, node, entry, x, entry_text_top);
        break;

    case NodeType::TaskListItem:
        // GenNodeTextDecorations は text_layout=nullptr (loose で空 TaskListItem) で early return
        // するため、checkbox は case 側で emit する。
        GenTaskListCheckbox(cmds, node, x, entry_text_top);
        break;

    case NodeType::BlockQuote:
        // バーと背景はグループ単位で GenBlockQuoteGroupDecorations から描画済み
        break;

    case NodeType::Heading: {
        const int8_t lv = node.heading_level();
        if (lv <= 2) {
            const float line_y = entry_text_top + entry.height + theme_->heading_spacing_below_h1h2 * HEADING_UNDERLINE_OFFSET_RATIO;
            cmds.emplace_back(DrawLineCmd{
                D2D1::Point2F(x, line_y), D2D1::Point2F(x + cw, line_y),
                theme_->hr_color, theme_->GetHeadingUnderlineThickness(lv), BrushId::Hr });
        }
        break;
    }

    case NodeType::Paragraph:
        break;

    default:
        std::unreachable();
    }

    GenNodeTextDecorations(cmds, fc, node, entry, node_index, x, text_x, entry_text_top);
}

CommandGenerator::NodeBaseStyle CommandGenerator::GetNodeBaseStyle(const Node& node) const noexcept
{
    switch (node.type) {
    case NodeType::Heading:
        return { theme_->heading_color, BrushId::Heading };
    case NodeType::BlockQuote:
        return (node.alert_type != AlertType::None)
                   ? NodeBaseStyle{ theme_->text_color, BrushId::Text }
                   : NodeBaseStyle{ theme_->blockquote_text_color, BrushId::BlockquoteText };
    case NodeType::CodeBlock:
        return { theme_->code_text_color, BrushId::CodeText };
    case NodeType::Paragraph:
    case NodeType::HorizontalRule:
    case NodeType::ListItem:
    case NodeType::Table:
    case NodeType::TaskListItem:
    case NodeType::Image:
        return { theme_->text_color, BrushId::Text };
    }
    std::unreachable();
}

void CommandGenerator::GenNodeTextDecorations(DrawCommandList& cmds, const FrameContext& fc, const Node& node, const NodeLayoutEntry& entry, int node_index, float /*x*/, float text_x, float entry_text_top)
{
    if (!entry.text_layout) {
        return;
    }

    const auto [base_color, base_brush] = GetNodeBaseStyle(node);

    GenInlineCodeBgs(cmds, entry.view_inline_code_bgs(), text_x, entry_text_top, theme_->code_bg_color);

    // 検索マッチのハイライト（選択より先に描画し、選択が最前面になるようにする）
    GenSearchHighlights(cmds, entry, node_index, text_x, entry_text_top);

    const auto& selection = fc.selection;
    if (selection.active && node_index >= selection.start_node && node_index <= selection.end_node) {
        const auto [sel_start, sel_end] = selection.ClampedRange(node_index, node.GetText().size());
        if (sel_end > sel_start) {
            GenSelectionHighlightCached(cmds, node, entry, sel_start, sel_end - sel_start, text_x, entry_text_top);
        }
    }

    cmds.emplace_back(DrawTextLayoutCmd{ D2D1::Point2F(text_x, entry_text_top), entry.text_layout.Get(), base_color, base_brush });
}

void CommandGenerator::GenTaskListCheckbox(DrawCommandList& cmds, const Node& node, float x, float entry_text_top)
{
    if (formats_.icon_font) {
        const wchar_t icon = node.task_checked() ? L'\u2611' : L'\u2610'; // ☑ / ☐
        const float icon_size = theme_->font_size_body;
        const float cb_x = x - theme_->list_bullet_offset;
        cmds.emplace_back(MakeTextCmd(
            &icon, 1,
            D2D1::RectF(cb_x, entry_text_top, cb_x + icon_size, entry_text_top + icon_size * TASK_CHECKBOX_HEIGHT_FACTOR),
            formats_.icon_font,
            theme_->text_color,
            BrushId::Text));
    }
}

void CommandGenerator::GenHorizontalRule(DrawCommandList& cmds, const NodeLayoutEntry& /*entry*/, float x, float w, float entry_text_top)
{
    const float y = entry_text_top + theme_->paragraph_spacing * 0.5f;
    cmds.emplace_back(DrawLineCmd{
        D2D1::Point2F(x, y), D2D1::Point2F(x + w, y),
        theme_->hr_color, theme_->hr_thickness, BrushId::Hr });
}

void CommandGenerator::GenCodeBlockBg(DrawCommandList& cmds, const NodeLayoutEntry& entry, float x, float w, float entry_text_top)
{
    const float pad = theme_->code_block_padding;
    const D2D1_RECT_F bg_rect = D2D1::RectF(
        x,
        entry_text_top - pad,
        x + w,
        entry_text_top + entry.height + pad);
    cmds.emplace_back(FillRoundedRectCmd{ bg_rect, CODE_BLOCK_CORNER, CODE_BLOCK_CORNER, theme_->code_bg_color, BrushId::CodeBg });
}

void CommandGenerator::GenCopyButton(DrawCommandList& cmds, const NodeLayoutEntry& /*entry*/, float x, float w, bool is_hovered, float entry_text_top)
{
    if (!formats_.copy_btn_icon) {
        return;
    }

    const float pad = theme_->code_block_padding;
    const D2D1_RECT_F btn = OverlayButtonRect(x + w, entry_text_top - pad);
    GenOverlayButton(cmds, btn, L'\uE8C8', is_hovered);
}

void CommandGenerator::GenSaveButton(DrawCommandList& cmds, float bitmap_right, float bitmap_top, bool is_hovered)
{
    if (!formats_.copy_btn_icon) {
        return;
    }
    const D2D1_RECT_F btn = OverlayButtonRect(bitmap_right, bitmap_top, std::to_underlying(DiagramButtonSlot::Save));
    GenOverlayButton(cmds, btn, L'\uE896', is_hovered);
}

void CommandGenerator::GenSvgCopyButton(DrawCommandList& cmds, float bitmap_right, float bitmap_top, bool is_hovered)
{
    if (!formats_.copy_btn_icon) {
        return;
    }
    const D2D1_RECT_F btn = OverlayButtonRect(bitmap_right, bitmap_top, std::to_underlying(DiagramButtonSlot::SvgCopy));
    GenOverlayButton(cmds, btn, L'\uE8C8', is_hovered);
}

void CommandGenerator::EmitBlockHScrollbarIfActive(DrawCommandList& cmds, const FrameContext& fc, int node_index, float block_x, float bar_y, const BlockHScrollGeometry& geom, float scroll_x)
{
    if (!geom.can_scroll()) {
        return;
    }
    if (node_index != fc.h_scroll.hovered_block && node_index != fc.h_scroll.drag_block) {
        return;
    }
    const float track_w = geom.visible_width;
    const float thumb_w = BlockHScrollbarThumbWidth(geom.visible_width, geom.natural_width);
    const float scroll_max = geom.scroll_max();
    const float ratio = (scroll_max > 0.0f) ? std::clamp(scroll_x / scroll_max, 0.0f, 1.0f) : 0.0f;
    // bullet と同じく Identity transform + 物理ピクセル snap で描画する。
    // pane_transform の md_x が非整数 (DPI 1 以外) のとき、サブピクセル位置で
    // アンチエイリアスが上下に漏れて thumb の太さがブレる現象を防ぐ。
    const float abs_x = SnapToPhysicalPixel(fc.md_pane_x + block_x + ratio * (track_w - thumb_w), fc.dpi_scale);
    // bar_y は entry_text_top から算出されたローカル Y。Identity transform でも追加減算は不要。
    const float abs_y = SnapToPhysicalPixel(bar_y, fc.dpi_scale);
    const float w_snapped = SnapToPhysicalPixel(thumb_w, fc.dpi_scale);
    const float h_snapped = SnapToPhysicalPixel(PANE_SCROLLBAR_WIDTH, fc.dpi_scale);
    cmds.emplace_back(SetTransformCmd{ D2D1::Matrix3x2F::Identity() });
    cmds.emplace_back(FillRoundedRectCmd{
        D2D1::RectF(abs_x, abs_y, abs_x + w_snapped, abs_y + h_snapped),
        h_snapped / 2.0f, h_snapped / 2.0f,
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f),
        BrushId::ScrollbarThumb });
    cmds.emplace_back(SetTransformCmd{ fc.pane_transform });
}

void CommandGenerator::GenOverlayButton(DrawCommandList& cmds, D2D1_RECT_F btn, wchar_t icon, bool is_hovered)
{
    const float bg_alpha = is_hovered ? (cached_is_dark_ ? 0.30f : 0.15f) : (cached_is_dark_ ? 0.10f : 0.05f);
    cmds.emplace_back(FillRoundedRectCmd{ btn, COPY_BTN_CORNER, COPY_BTN_CORNER, mendo::MonochromeOverlay(cached_is_dark_, bg_alpha) });

    const float text_alpha = is_hovered ? (cached_is_dark_ ? 0.9f : 0.8f) : (cached_is_dark_ ? 0.4f : 0.35f);
    cmds.emplace_back(MakeTextCmd(&icon, 1, btn, formats_.copy_btn_icon, mendo::MonochromeOverlay(cached_is_dark_, text_alpha)));
}

void CommandGenerator::GenListBullet(DrawCommandList& cmds, const FrameContext& fc, const Node& node, const NodeLayoutEntry& entry, float x, float entry_text_top)
{
    if (node.list_ordered()) {
        if (formats_.list_number) {
            const int32_t num = node.list_number();
            const float first_line_h = GetFirstLineHeight(entry, theme_->font_size_body);
            wchar_t num_buf[16];
            const auto fmt_result = std::format_to_n(num_buf, std::size(num_buf), L"{}.", num);
            const size_t num_len = std::min(static_cast<size_t>(fmt_result.size), std::size(num_buf));
            const D2D1_RECT_F num_rect = D2D1::RectF(
                x - theme_->list_bullet_offset - LIST_NUMBER_PAD_RIGHT,
                entry_text_top,
                x - LIST_NUMBER_PAD_LEFT,
                entry_text_top + first_line_h);
            cmds.emplace_back(MakeTextCmd(num_buf, num_len, num_rect, formats_.list_number, theme_->text_color));
        }
    }
    else {
        // 大きい scroll_y を SetTransform で適用すると D2D が小半径の楕円を bounding rect
        // (長方形) に縮退させるため、bullet だけ Identity transform + baked 座標で描画する。
        const float first_line_h = GetFirstLineHeight(entry, theme_->font_size_body);
        // X は Identity 化に伴い md_pane_x を手動で加算。Y は entry_text_top が既にローカル Y。
        const float bullet_x = SnapToPhysicalPixel(fc.md_pane_x + x - theme_->list_bullet_offset * LIST_BULLET_X_FACTOR, fc.dpi_scale);
        const float bullet_y = SnapToPhysicalPixel(entry_text_top + first_line_h * 0.5f, fc.dpi_scale);
        const float r = theme_->list_bullet_radius;
        cmds.emplace_back(SetTransformCmd{ D2D1::Matrix3x2F::Identity() });
        if (node.indent_level <= 1) {
            cmds.emplace_back(FillEllipseCmd{ D2D1::Point2F(bullet_x, bullet_y), r, r, theme_->text_color, BrushId::Text });
        }
        else {
            cmds.emplace_back(DrawEllipseCmd{ D2D1::Point2F(bullet_x, bullet_y), r, r, theme_->text_color, 1.0f, BrushId::Text });
        }
        cmds.emplace_back(SetTransformCmd{ fc.pane_transform });
    }
}

void CommandGenerator::GenBlockQuoteGroupDecorations(DrawCommandList& cmds, const FrameContext& fc, const std::pmr::vector<Node>& nodes, const LayoutCache& cache, int node_count, int first_visible)
{
    const float offset_x = fc.offset_x;
    const float content_width = fc.content_width;
    const float snap = fc.snapped_scroll_y;
    const float local_viewport_bottom = fc.viewport_bottom;
    static constexpr float BAR_EXTEND = 2.0f;
    static constexpr float ALERT_BG_PAD = 4.0f;
    static constexpr float ALERT_BG_CORNER = 4.0f;

    // first_visible がグループ途中の場合、グループ先頭まで遡る
    int i = first_visible;
    if (i < node_count && nodes[i].blockquote_group >= 0) {
        const int group = nodes[i].blockquote_group;
        while (i > 0 && nodes[i - 1].blockquote_group == group) {
            i--;
        }
    }

    while (i < node_count) {
        const int group = nodes[i].blockquote_group;
        if (group < 0) {
            // text_top は単調なので、非引用ノードが下端を超えたら以降のグループも全て可視域外。
            // ここで break しないと引用が可視域以降に無い文書で毎フレーム末尾まで全走査する。
            if (cache[i].text_top - snap > local_viewport_bottom) {
                break;
            }
            i++;
            continue;
        }
        const float group_top = cache[i].text_top - snap;
        if (group_top > local_viewport_bottom) {
            break;
        }

        float group_bottom = group_top + cache[i].height;
        const AlertType alert_type = nodes[i].alert_type;
        const int outer_indent = nodes[i].quote_outer_indent;
        int max_depth = nodes[i].quote_depth;

        int j = i + 1;
        while (j < node_count && nodes[j].blockquote_group == group) {
            const float bottom = cache[j].text_top - snap + cache[j].height;
            if (bottom > group_bottom) {
                group_bottom = bottom;
            }
            if (nodes[j].quote_depth > max_depth) {
                max_depth = nodes[j].quote_depth;
            }
            j++;
            // ビューポート外に大きく超えたグループ末尾は j を進めるだけにする。
            // バー/背景はクリップで切られるため、可視外で max_depth を更新しても
            // 描画コマンド数は変わらず CPU を浪費するだけ。
            if (group_bottom > local_viewport_bottom) {
                while (j < node_count && nodes[j].blockquote_group == group) {
                    j++;
                }
                break;
            }
        }

        if (max_depth <= 0 || outer_indent <= 0) {
            i = j;
            continue;
        }

        const float outer_x_indent = static_cast<float>(outer_indent) * theme_->indent_width;
        const float outer_x = offset_x + outer_x_indent;
        if (alert_type != AlertType::None) {
            const auto idx = AlertColorIndex(alert_type);
            if (idx < ALERT_TYPE_COUNT) {
                const float cw = content_width - outer_x_indent;
                const D2D1_RECT_F bg_rect = D2D1::RectF(outer_x - ALERT_BG_PAD, group_top - ALERT_BG_PAD, outer_x + cw, group_bottom + ALERT_BG_PAD);
                cmds.emplace_back(FillRoundedRectCmd{ bg_rect, ALERT_BG_CORNER, ALERT_BG_CORNER, theme_->alert_bg_color[idx] });
            }
        }

        // GitHub と同様に、外側のバーはネストした範囲を貫通して連続描画する。
        // 各 level ごとに quote_depth >= level の連続区間を求めてバーを発行。
        for (int level = 1; level <= max_depth; ++level) {
            const float bar_indent_x = static_cast<float>(outer_indent + level - 1) * theme_->indent_width;
            const float bar_x = offset_x + bar_indent_x - theme_->indent_width * 0.5f;
            const bool is_alert_bar = (level == 1 && alert_type != AlertType::None);
            const D2D1_COLOR_F bar_color = is_alert_bar ? theme_->alert_color[AlertColorIndex(alert_type)] : theme_->blockquote_bar_color;
            // BrushId::AlertNote..AlertCaution は AlertColorIndex(0..4) で連番に並んでいる。
            const BrushId bar_brush =
                is_alert_bar
                    ? static_cast<BrushId>(std::to_underlying(BrushId::AlertNote) + AlertColorIndex(alert_type))
                    : BrushId::BlockquoteBar;

            const auto emit_bar = [&](float top, float bottom) {
                cmds.emplace_back(DrawLineCmd{
                    D2D1::Point2F(bar_x, top - BAR_EXTEND),
                    D2D1::Point2F(bar_x, bottom + BAR_EXTEND),
                    bar_color, theme_->blockquote_bar_width, bar_brush });
            };

            bool in_region = false;
            float region_top = 0.0f;
            float region_bottom = 0.0f;
            for (int k = i; k < j; ++k) {
                const float local_top_k = cache[k].text_top - snap;
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

        i = j;
    }
}

void CommandGenerator::GenDiagramPlaceholder(DrawCommandList& cmds, float x, float y, float w, float h)
{
    const D2D1_RECT_F bg = D2D1::RectF(x, y, x + w, y + h);
    cmds.emplace_back(FillRoundedRectCmd{ bg, CODE_BLOCK_CORNER, CODE_BLOCK_CORNER, theme_->code_bg_color, BrushId::CodeBg });
    if (formats_.placeholder_text) {
        const auto loading_text = i18n::S().loading;
        cmds.emplace_back(MakeTextCmd(loading_text.data(), loading_text.size(), bg, formats_.placeholder_text, theme_->blockquote_text_color, BrushId::BlockquoteText));
    }
}

void CommandGenerator::EmitHighlightRects(
    DrawCommandList& cmds,
    IDWriteTextLayout* layout,
    uint32_t start,
    uint32_t length,
    float origin_x,
    float origin_y,
    D2D1_COLOR_F color,
    BrushId brush_id)
{
    if (!layout || length == 0) {
        return;
    }
    assert(hit_test_buffer_ && "SetHitTestBuffer must be called before GenerateMdPane");
    auto& buf = *hit_test_buffer_;
    MENDO_COUNT_INC(g_cmd_gen_stats.hittest_range);
    const UINT32 count = FetchHitTestMetrics(layout, start, length, buf);
    for (UINT32 i = 0; i < count; i++) {
        cmds.emplace_back(FillRectCmd{ RectFromHitTest(buf[i], origin_x, origin_y), color, brush_id });
    }
}

void CommandGenerator::GenSelectionHighlight(DrawCommandList& cmds, IDWriteTextLayout* layout, uint32_t start, uint32_t length, float origin_x, float origin_y)
{
    EmitHighlightRects(cmds, layout, start, length, origin_x, origin_y, SELECTION_COLOR, BrushId::Selection);
}

void CommandGenerator::CollectHitTestRects(IDWriteTextLayout* layout, uint32_t start, uint32_t length, std::pmr::vector<D2D1_RECT_F>& out)
{
    assert(hit_test_buffer_ && "SetHitTestBuffer must be called before GenerateMdPane");
    auto& buf = *hit_test_buffer_;
    MENDO_COUNT_INC(g_cmd_gen_stats.hittest_range);
    const UINT32 count = FetchHitTestMetrics(layout, start, length, buf);
    out.reserve(out.size() + count);
    for (UINT32 i = 0; i < count; i++) {
        out.emplace_back(RectFromHitTest(buf[i]));
    }
}

void CommandGenerator::GenSelectionHighlightCached(DrawCommandList& cmds, const Node& node, const NodeLayoutEntry& entry, uint32_t doc_start, uint32_t doc_length, float origin_x, float origin_y)
{
    auto* layout = entry.text_layout.Get();
    if (!layout || doc_length == 0) {
        return;
    }
    auto& cache = entry.ensure_selection_hl_cache();
    // キャッシュキーは UTF-8 byte 単位 (selection 状態と直接対応)。
    // miss 時は WideViewCache 経由で同一ノードの連続 miss でも decode を 1 回に抑える。
    if (cache.layout_ptr != layout || cache.start != doc_start || cache.length != doc_length) {
        MENDO_COUNT_INC(g_cmd_gen_stats.sel_hl_cache_miss);
        cache.rects.clear();
        const auto wr = node_wv_.WideRange(node.GetText(), doc_start, doc_length);
        if (wr.length > 0) {
            CollectHitTestRects(layout, wr.startPosition, wr.length, cache.rects);
        }
        cache.layout_ptr = layout;
        cache.start = doc_start;
        cache.length = doc_length;
    }
    else {
        MENDO_COUNT_INC(g_cmd_gen_stats.sel_hl_cache_hit);
    }
    for (const auto& r : cache.rects) {
        cmds.emplace_back(FillRectCmd{ OffsetRectF(r, origin_x, origin_y), SELECTION_COLOR, BrushId::Selection });
    }
}

void CommandGenerator::GenSearchHighlights(DrawCommandList& cmds, const NodeLayoutEntry& entry, int node_index, float origin_x, float origin_y, int table_row, int table_col)
{
    if (!search_matches_ || search_matches_->empty()) {
        return;
    }

    const std::span<const SearchMatch> matches = *search_matches_;
    const auto range = std::ranges::equal_range(matches, node_index, {}, &SearchMatch::node_index);
    if (range.empty()) {
        return;
    }
    const size_t first_global = static_cast<size_t>(range.begin() - matches.begin());
    const size_t node_match_count = static_cast<size_t>(range.size());

    auto& cache = entry.ensure_search_hl_cache();
    RebuildSearchHlCache(cache, entry, matches, first_global, node_match_count);
    EmitSearchHlCommands(cmds, cache, matches, first_global, origin_x, origin_y, table_row, table_col);
}

void CommandGenerator::RebuildSearchHlCache(
    SearchHlCache& cache, const NodeLayoutEntry& entry,
    std::span<const SearchMatch> matches, size_t first_global, size_t node_match_count)
{
    // キャッシュミス時のみ HitTestTextRange を一括発行。layout 変更時は
    // invalidate_search_hl_cache() でキャッシュ自体が破棄されており、SearchState の
    // generation は 1 から始まるため、cache.gen == search_generation_ のみで
    // キャッシュ有効性を完全判定できる。
    if (cache.gen == search_generation_) {
        return;
    }

    MENDO_COUNT_INC(g_cmd_gen_stats.search_hl_rebuild);
    cache.rects.clear();
    cache.rect_ends.clear();
    cache.rect_ends.reserve(node_match_count);

    for (size_t mi = first_global; mi < first_global + node_match_count; ++mi) {
        const auto& m = matches[mi];
        IDWriteTextLayout* l = nullptr;
        if (m.table_row >= 0 && entry.has_table_layout()) {
            l = entry.table_layout->GetCellLayout(static_cast<size_t>(m.table_row), static_cast<size_t>(m.table_col));
        }
        else if (m.table_row < 0) {
            l = entry.text_layout.Get();
        }
        // m.table_row >= 0 && !has_table_layout() のケースは layout 失効中の
        // 暫定状態で、l は nullptr のまま空 rect として記録する（次回再構築時に修復）。
        if (l && m.length_w > 0) {
            CollectHitTestRects(l, m.start_w, m.length_w, cache.rects);
        }
        else if (!l && m.table_row >= 0) {
            MENDO_COUNT_INC(g_cmd_gen_stats.search_hl_provisional);
        }
        cache.rect_ends.push_back(static_cast<uint32_t>(cache.rects.size()));
    }
    cache.gen = search_generation_;
}

void CommandGenerator::EmitSearchHlCommands(
    DrawCommandList& cmds,
    const SearchHlCache& cache,
    std::span<const SearchMatch> matches,
    size_t first_global,
    float origin_x,
    float origin_y,
    int table_row,
    int table_col)
{
    // 呼び出し側（セル/ノード本体）に属する match のみ描画する。
    const size_t node_match_count = cache.rect_ends.size();

    for (size_t node_mi = 0; node_mi < node_match_count; ++node_mi) {
        const size_t mi = first_global + node_mi;
        const auto& m = matches[mi];
        const bool is_here = (table_row >= 0) ? (m.table_row == table_row && m.table_col == table_col) : (m.table_row < 0);
        if (!is_here) {
            continue;
        }

        const uint32_t rb = (node_mi == 0) ? 0 : cache.rect_ends[node_mi - 1];
        const uint32_t re = cache.rect_ends[node_mi];

        const bool is_current = (static_cast<int>(mi) == current_match_index_);
        const D2D1_COLOR_F color = is_current ? theme_->search_highlight_current_color : theme_->search_highlight_color;
        const BrushId hl_brush = is_current ? BrushId::SearchHighlightCurrent : BrushId::SearchHighlight;
        for (uint32_t k = rb; k < re; ++k) {
            cmds.emplace_back(FillRectCmd{ OffsetRectF(cache.rects[k], origin_x, origin_y), color, hl_brush });
        }
    }
}

void CommandGenerator::GenTableRowBg(DrawCommandList& cmds, const TableRowGeom& g, bool is_header, bool is_even_row)
{
    if (is_header) {
        cmds.emplace_back(FillRectCmd{ D2D1::RectF(g.x, g.y, g.x + g.table_width, g.y + g.row_h + g.border), theme_->code_bg_color, BrushId::CodeBg });
    }
    else if (is_even_row) {
        // cached_stripe_color_ と Renderer の brushes_[TableStripe] は同じ式 (renderer_resources.cpp) で算出する。
        cmds.emplace_back(FillRectCmd{ D2D1::RectF(g.x, g.y, g.x + g.table_width, g.y + g.row_h + g.border), cached_stripe_color_, BrushId::TableStripe });
    }
}

void CommandGenerator::GenTableCellContent(DrawCommandList& cmds, std::string_view cell_text, const CellDrawContext& ctx)
{
    if (ctx.has_selection && ctx.layout) {
        const uint32_t cell_len = static_cast<uint32_t>(cell_text.size());
        const uint32_t ov_start = std::max(ctx.sel_start, ctx.flat_offset);
        const uint32_t ov_end = std::min(ctx.sel_end, ctx.flat_offset + cell_len);
        if (ov_end > ov_start) {
            // sel_start/sel_end は UTF-8 byte offset。HitTestTextRange 用に UTF-16 へ変換する。
            const auto wr = cell_wv_.WideRange(cell_text, ov_start - ctx.flat_offset, ov_end - ov_start);
            if (wr.length > 0) {
                GenSelectionHighlight(cmds, ctx.layout, wr.startPosition, wr.length, ctx.text_x, ctx.text_y);
            }
        }
    }
    if (ctx.layout) {
        const D2D1_COLOR_F cell_color = ctx.is_header ? theme_->heading_color : theme_->text_color;
        const BrushId cell_brush = ctx.is_header ? BrushId::Heading : BrushId::Text;
        cmds.emplace_back(DrawTextLayoutCmd{ D2D1::Point2F(ctx.text_x, ctx.text_y), ctx.layout, cell_color, cell_brush });
    }
}

void CommandGenerator::GenTable(
    DrawCommandList& cmds,
    const FrameContext& fc,
    const Node& node, const NodeLayoutEntry& entry,
    int node_index, float offset_x, float entry_text_top, float h_scroll_x)
{
    const auto* tbl = node.table_data();
    if (!tbl || tbl->row_count == 0 || !entry.has_table_layout() || entry.table_layout->col_widths.empty()) {
        return;
    }

    const float cell_padding = TABLE_CELL_PADDING;
    const float border = TABLE_BORDER_WIDTH;
    const auto& tl = *entry.table_layout;
    const auto& selection = fc.selection;
    const float viewport_top = fc.viewport_top;
    const float viewport_bottom = fc.viewport_bottom;
    const auto row_count = tbl->row_count;
    const auto col_count = static_cast<size_t>(tbl->col_count);

    const float table_width = tl.cached_table_width;

    bool has_selection = selection.active && (node_index >= selection.start_node) && (node_index <= selection.end_node);
    uint32_t sel_start = 0, sel_end = 0;
    if (has_selection) {
        const auto range = selection.ClampedRange(node_index, tbl->concat_text.size());
        sel_start = range.start;
        sel_end = range.end;
        if (sel_end <= sel_start) {
            has_selection = false;
        }
    }

    float y = entry_text_top;
    size_t bg_cursor = 0;

    // 可視行帯を row_cum_y の二分探索で求め、その範囲だけループする
    // (ApplyTableEffects / FindTableRow と同じパターン)。巨大テーブルで
    // 毎フレーム row_count 回の continue ループが走るのを防ぐ。
    size_t r_begin = 0;
    size_t r_end = row_count;
    const bool has_row_geometry = tl.row_cum_y.size() == row_count + 1;
    if (has_row_geometry) {
        const auto [rb, re] = tl.VisibleRowRange(viewport_top - entry_text_top, viewport_bottom - entry_text_top);
        r_begin = rb;
        r_end = re;
        y = entry_text_top + tl.row_cum_y[r_begin];
        // bg リストは追記順が乱れうるため二分探索せず、既存セマンティクス
        // (前進スキップ) で r_begin 直前まで進める。サイズは bg 持ちセル数のみ。
        const uint32_t first_cell = static_cast<uint32_t>(r_begin * tl.col_count);
        while (bg_cursor < tl.cell_inline_code_bgs.size() && tl.cell_inline_code_bgs[bg_cursor].cell_index < first_cell) {
            ++bg_cursor;
        }
    }

    for (size_t r = r_begin; r < r_end; r++) {
        const float row_h = (r < tl.row_heights.size()) ? tl.row_heights[r] : (theme_->font_size_body * TABLE_ROW_HEIGHT_FACTOR);

        const float row_bottom = y + row_h + border;
        if (row_bottom < viewport_top || y > viewport_bottom) {
            y = row_bottom;
            if (!tl.cell_inline_code_bgs.empty() && bg_cursor < tl.cell_inline_code_bgs.size()) {
                const uint32_t next_cell = static_cast<uint32_t>((r + 1) * tl.col_count);
                while (bg_cursor < tl.cell_inline_code_bgs.size() && tl.cell_inline_code_bgs[bg_cursor].cell_index < next_cell) {
                    ++bg_cursor;
                }
            }
            continue;
        }

        const bool is_header_row = tbl->IsHeaderRow(r);
        GenTableRowBg(cmds, TableRowGeom{ offset_x, y, table_width, row_h, border }, is_header_row, r % 2 == 0);

        // 行上部の水平線
        cmds.emplace_back(DrawLineCmd{ D2D1::Point2F(offset_x, y), D2D1::Point2F(offset_x + table_width, y), theme_->hr_color, border, BrushId::Hr });

        // 可視列のみコマンド生成、画面外は cell_text_starts で flat_offset を直接取得。
        // 横スクロール時は画面に映る範囲が h_scroll_x だけ右にずれる。
        const float cull_left = offset_x + fc.viewport_left + h_scroll_x;
        const float cull_right = offset_x + fc.viewport_right + h_scroll_x;
        float cx = offset_x + border;
        const size_t drawn_cols = std::min(col_count, tl.col_widths.size());
        for (size_t c = 0; c < drawn_cols; c++) {
            const float cw = tl.col_widths[c];
            const float col_right = cx + cw + cell_padding * 2.0f;
            const bool col_visible = (col_right >= cull_left) && (cx - border <= cull_right);

            if (col_visible) {
                cmds.emplace_back(DrawLineCmd{ D2D1::Point2F(cx - border, y), D2D1::Point2F(cx - border, y + row_h + border), theme_->hr_color, border, BrushId::Hr });

                const float text_x = cx + cell_padding;
                const float text_y = y + cell_padding;
                IDWriteTextLayout* cell_layout = tl.GetCellLayout(r, c);

                // bg_cursor は cell_index 昇順で進む。
                bg_cursor = GenCellInlineCodeBgs(cmds, tl.cell_inline_code_bgs, bg_cursor, static_cast<uint32_t>(tl.CellIndex(r, c)), text_x, text_y, theme_->code_bg_color);

                GenSearchHighlights(cmds, entry, node_index, text_x, text_y, static_cast<int>(r), static_cast<int>(c));

                const auto cell_text = tbl->GetCellText(r, c);
                const uint32_t cell_flat = tbl->CellTextStart(r, c);
                GenTableCellContent(cmds, cell_text, CellDrawContext{
                    .layout = cell_layout,
                    .text_x = text_x,
                    .text_y = text_y,
                    .flat_offset = cell_flat,
                    .sel_start = sel_start,
                    .sel_end = sel_end,
                    .has_selection = has_selection,
                    .is_header = is_header_row,
                });
            }

            cx += cw + cell_padding * 2.0f + border;
        }
        cmds.emplace_back(DrawLineCmd{ D2D1::Point2F(offset_x + table_width, y), D2D1::Point2F(offset_x + table_width, y + row_h + border), theme_->hr_color, border, BrushId::Hr });
        y += row_h + border;
    }

    // r_end で打ち切った場合も下端線はテーブル全体の底に置く (可視外ならクリップされる)
    const float table_bottom = has_row_geometry ? entry_text_top + tl.row_cum_y[row_count] : y;
    cmds.emplace_back(DrawLineCmd{ D2D1::Point2F(offset_x, table_bottom), D2D1::Point2F(offset_x + table_width, table_bottom), theme_->hr_color, border, BrushId::Hr });
}
