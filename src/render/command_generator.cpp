#include "command_generator.h"
#include "i18n.h"
#include "layout.h"
#include "ui_constants.h"
#include <algorithm>
#include <format>
#include <ranges>
#include <utility>

// h1/h2 見出し下線を描画するY座標を heading_spacing_below_h1h2 のこの比率で決める。
// 値を小さくすると下線は見出し文字に近づき、下線と次行の間隔が広がる。
static constexpr float HEADING_UNDERLINE_OFFSET_RATIO = 0.25f;

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
    float dpi_scale)
{
    // 古いコマンドを destruct してから resource をリセットする。
    // 順序が逆だと cmds_ 内部ストレージが Reset 済みバッファを指してしまう。
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
    // フレーム間変動によるテキストのガタつきを防止する。
    // viewport bounds にはスナップ前の scroll_y を使い、ヒットテストとの座標一致を保つ。
    const float snapped_y = SnapScrollToPixel(scroll_y, dpi_scale);
    cmds.emplace_back(SetTransformCmd{ D2D1::Matrix3x2F::Translation(md_pane_rect.x, -snapped_y) });

    frame_offset_x_ = theme_->margin_left;
    frame_viewport_top_ = scroll_y;
    frame_viewport_bottom_ = scroll_y + md_pane_rect.height;
    // 水平カリング範囲: ペイン内ローカル座標で margin_left を起点とした相対値
    frame_viewport_left_ = -theme_->margin_left;
    frame_viewport_right_ = md_pane_rect.width - theme_->margin_left;
    frame_content_width_ = theme_->ContentWidth(md_pane_rect.width);
    frame_selection_ = &selection;
    frame_hovered_ = hovered;

    // 前フレームの選択範囲のうち、今回の範囲外に出たノードの selection_hl_cache を破棄する。
    // 「解除」「縮小」「別範囲への移動」をまとめて拾い、長時間利用での漸増を防ぐ。
    {
        const int new_start = selection.active ? selection.start_node : -1;
        const int new_end = selection.active ? selection.end_node : -1;
        if (prev_sel_start_node_ >= 0) {
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

    const int node_count = static_cast<int>(nodes.size());
    if (first_visible < 0) {
        first_visible = FindFirstVisibleNodeIndex(cache, nodes.size(), frame_viewport_top_);
    }

    // 同一 blockquote_group のノードをまとめてバー/背景を描画する。
    // ノード単体ごとではなく一括で描画することで、複数行の引用ブロックが連続した見た目になる。
    GenBlockQuoteGroupDecorations(cmds, nodes, cache, node_count, first_visible);

    for (int i = first_visible; i < node_count; i++) {
        if (cache[i].y_position > frame_viewport_bottom_) {
            break;
        }
        GenerateNode(cmds, nodes[i], cache[i], cache.GetDiagram(i), i);
    }

    cmds.emplace_back(SetTransformCmd{ D2D1::Matrix3x2F::Identity() });
    cmds.emplace_back(PopClipCmd{});
    frame_selection_ = nullptr;
    last_cmds_size_ = cmds_.size();
    return cmds_;
}

void CommandGenerator::GenerateNode(DrawCommandList& cmds,
                                    const Node& node, const NodeLayoutEntry& entry, const DiagramEntry& diagram,
                                    int node_index)
{
    // h1/h2は見出し下線がentry.heightの外に描画されるため、カリング境界を拡張する。
    float node_bottom = entry.y_position + entry.height;
    if (node.type == NodeType::Heading && node.heading_level <= 2) {
        node_bottom += theme_->heading_spacing_below_h1h2 * HEADING_UNDERLINE_OFFSET_RATIO + theme_->GetHeadingUnderlineThickness(node.heading_level);
    }
    if (node_bottom < frame_viewport_top_ || entry.y_position > frame_viewport_bottom_) {
        return;
    }

    const float indent = NodeIndent(node, *theme_);
    const float x = frame_offset_x_ + indent;
    const float cw = frame_content_width_ - indent;
    const float text_x = x + NodeTextXOffset(node, *theme_);

    switch (node.type) {
    case NodeType::HorizontalRule:
        GenHorizontalRule(cmds, entry, x, cw);
        return;

    case NodeType::Image:
        if (diagram.bitmap) {
            const float draw_h = entry.height;
            const float draw_w = (diagram.height > 0) ? diagram.width * (draw_h / diagram.height) : diagram.width;
            const float dx = x;
            cmds.emplace_back(DrawBitmapCmd{
                diagram.bitmap.Get(),
                D2D1::RectF(dx, entry.y_position, dx + draw_w, entry.y_position + draw_h) });
        }
        else {
            GenDiagramPlaceholder(cmds, x, entry.y_position, cw, entry.height);
        }
        return;

    case NodeType::Table:
        GenTable(cmds, node, entry, node_index, x);
        return;

    case NodeType::CodeBlock:
        if (IsDiagramLanguage(node.code_language)) {
            if (diagram.bitmap) {
                const auto bmp = MermaidBitmapRect(diagram.width, diagram.height, x, cw, entry.y_position);
                cmds.emplace_back(DrawBitmapCmd{ diagram.bitmap.Get(), bmp });
                GenSaveButton(cmds, bmp.right, bmp.top, node_index == frame_hovered_.save);
                if (IsSvgExportable(node.code_language)) {
                    GenSvgCopyButton(cmds, bmp.right, bmp.top, node_index == frame_hovered_.svg_copy);
                }
            }
            else {
                GenDiagramPlaceholder(cmds, x, entry.y_position, cw, entry.height);
            }
            return;
        }
        GenCodeBlockBg(cmds, entry, x, cw);
        GenCopyButton(cmds, entry, x, cw, node_index == frame_hovered_.copy);
        break;

    case NodeType::ListItem:
        GenListBullet(cmds, node, entry, x);
        break;

    case NodeType::TaskListItem:
        break;

    case NodeType::BlockQuote:
        // バーと背景はグループ単位で GenBlockQuoteGroupDecorations から描画済み
        break;

    case NodeType::Heading:
        if (node.heading_level <= 2) {
            const float line_y = entry.y_position + entry.height + theme_->heading_spacing_below_h1h2 * HEADING_UNDERLINE_OFFSET_RATIO;
            cmds.emplace_back(DrawLineCmd{
                D2D1::Point2F(x, line_y), D2D1::Point2F(x + cw, line_y),
                theme_->hr_color, theme_->GetHeadingUnderlineThickness(node.heading_level) });
        }
        break;

    case NodeType::Paragraph:
        break;

    default:
        std::unreachable();
    }

    GenNodeTextDecorations(cmds, node, entry, node_index, x, text_x);
}

D2D1_COLOR_F CommandGenerator::GetNodeBaseColor(const Node& node) const noexcept
{
    switch (node.type) {
    case NodeType::Heading:
        return theme_->heading_color;
    case NodeType::BlockQuote:
        return (node.alert_type != AlertType::None) ? theme_->text_color : theme_->blockquote_text_color;
    case NodeType::CodeBlock:
        return theme_->code_text_color;
    case NodeType::Paragraph:
    case NodeType::HorizontalRule:
    case NodeType::ListItem:
    case NodeType::Table:
    case NodeType::TaskListItem:
    case NodeType::Image:
        return theme_->text_color;
    }
    std::unreachable();
}

void CommandGenerator::GenNodeTextDecorations(DrawCommandList& cmds, const Node& node, const NodeLayoutEntry& entry, int node_index, float x, float text_x)
{
    if (!entry.text_layout) {
        return;
    }

    const D2D1_COLOR_F base_color = GetNodeBaseColor(node);

    GenInlineCodeBgs(cmds, entry.view_inline_code_bgs(), text_x, entry.y_position, theme_->code_bg_color);

    // 検索マッチのハイライト（選択より先に描画し、選択が最前面になるようにする）
    GenSearchHighlights(cmds, entry, node_index, text_x, entry.y_position);

    const auto& selection = *frame_selection_;
    if (selection.active && node_index >= selection.start_node && node_index <= selection.end_node) {
        uint32_t sel_start = 0;
        uint32_t sel_end = static_cast<uint32_t>(node.GetText().size());
        if (node_index == selection.start_node) {
            sel_start = selection.start_pos;
        }
        if (node_index == selection.end_node) {
            sel_end = selection.end_pos;
        }
        if (sel_end > sel_start) {
            GenSelectionHighlightCached(cmds, entry, sel_start, sel_end - sel_start, text_x, entry.y_position);
        }
    }

    cmds.emplace_back(DrawTextLayoutCmd{ D2D1::Point2F(text_x, entry.y_position), entry.text_layout.Get(), base_color });

    if (node.type == NodeType::TaskListItem && formats_.icon_font) {
        const wchar_t icon = node.task_checked ? L'\u2611' : L'\u2610'; // ☑ / ☐
        const float icon_size = theme_->font_size_body;
        const float cb_x = x - theme_->list_bullet_offset;
        cmds.emplace_back(MakeTextCmd(
            &icon, 1,
            D2D1::RectF(cb_x, entry.y_position, cb_x + icon_size, entry.y_position + icon_size * TASK_CHECKBOX_HEIGHT_FACTOR),
            formats_.icon_font,
            theme_->text_color));
    }
}

void CommandGenerator::GenHorizontalRule(DrawCommandList& cmds, const NodeLayoutEntry& entry, float x, float w)
{
    const float y = entry.y_position + theme_->paragraph_spacing * 0.5f;
    cmds.emplace_back(DrawLineCmd{
        D2D1::Point2F(x, y), D2D1::Point2F(x + w, y),
        theme_->hr_color, theme_->hr_thickness });
}

void CommandGenerator::GenCodeBlockBg(DrawCommandList& cmds, const NodeLayoutEntry& entry, float x, float w)
{
    const float pad = theme_->code_block_padding;
    const D2D1_RECT_F bg_rect = D2D1::RectF(
        x,
        entry.y_position - pad,
        x + w,
        entry.y_position + entry.height + pad);
    cmds.emplace_back(FillRoundedRectCmd{ bg_rect, CODE_BLOCK_CORNER, CODE_BLOCK_CORNER, theme_->code_bg_color });
}

void CommandGenerator::GenCopyButton(DrawCommandList& cmds, const NodeLayoutEntry& entry, float x, float w, bool is_hovered)
{
    if (!formats_.copy_btn_icon) {
        return;
    }

    const float pad = theme_->code_block_padding;
    const D2D1_RECT_F btn = OverlayButtonRect(x + w, entry.y_position - pad);
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

void CommandGenerator::GenOverlayButton(DrawCommandList& cmds, D2D1_RECT_F btn, wchar_t icon, bool is_hovered)
{
    const float bg_alpha = is_hovered ? (cached_is_dark_ ? 0.30f : 0.15f) : (cached_is_dark_ ? 0.10f : 0.05f);
    const D2D1_COLOR_F bg_color = cached_is_dark_ ? D2D1::ColorF(1.0f, 1.0f, 1.0f, bg_alpha) : D2D1::ColorF(0.0f, 0.0f, 0.0f, bg_alpha);
    cmds.emplace_back(FillRoundedRectCmd{ btn, COPY_BTN_CORNER, COPY_BTN_CORNER, bg_color });

    const float text_alpha = is_hovered ? (cached_is_dark_ ? 0.9f : 0.8f) : (cached_is_dark_ ? 0.4f : 0.35f);
    const D2D1_COLOR_F icon_color = cached_is_dark_ ? D2D1::ColorF(1.0f, 1.0f, 1.0f, text_alpha) : D2D1::ColorF(0.0f, 0.0f, 0.0f, text_alpha);
    cmds.emplace_back(MakeTextCmd(&icon, 1, btn, formats_.copy_btn_icon, icon_color));
}

void CommandGenerator::GenListBullet(DrawCommandList& cmds, const Node& node, const NodeLayoutEntry& entry, float x)
{
    if (node.list_number > 0) {
        if (formats_.list_number) {
            const float first_line_h = GetFirstLineHeight(entry, theme_->font_size_body);
            wchar_t num_buf[16];
            const auto fmt_result = std::format_to_n(num_buf, std::size(num_buf), L"{}.", node.list_number);
            const size_t num_len = std::min(static_cast<size_t>(fmt_result.size), std::size(num_buf));
            const D2D1_RECT_F num_rect = D2D1::RectF(
                x - theme_->list_bullet_offset - LIST_NUMBER_PAD_RIGHT,
                entry.y_position,
                x - LIST_NUMBER_PAD_LEFT,
                entry.y_position + first_line_h);
            cmds.emplace_back(MakeTextCmd(num_buf, num_len, num_rect, formats_.list_number, theme_->text_color));
        }
    }
    else {
        // 1行目の中央に配置
        const float first_line_h = GetFirstLineHeight(entry, theme_->font_size_body);
        const float bullet_y = entry.y_position + first_line_h * 0.5f;
        const float bullet_x = x - theme_->list_bullet_offset * LIST_BULLET_X_FACTOR;
        const float r = LIST_BULLET_RADIUS;
        if (node.indent_level <= 1) {
            cmds.emplace_back(FillEllipseCmd{ D2D1::Point2F(bullet_x, bullet_y), r, r, theme_->text_color });
        }
        else {
            cmds.emplace_back(DrawEllipseCmd{ D2D1::Point2F(bullet_x, bullet_y), r, r, theme_->text_color, 1.0f });
        }
    }
}

void CommandGenerator::GenBlockQuoteGroupDecorations(DrawCommandList& cmds, const std::pmr::vector<Node>& nodes, const LayoutCache& cache, int node_count, int first_visible)
{
    const float offset_x = frame_offset_x_;
    const float content_width = frame_content_width_;
    const float viewport_bottom = frame_viewport_bottom_;
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
            i++;
            continue;
        }
        if (cache[i].y_position > viewport_bottom) {
            break;
        }

        const float group_top = cache[i].y_position;
        float group_bottom = cache[i].y_position + cache[i].height;
        const AlertType alert_type = nodes[i].alert_type;
        const int outer_indent = nodes[i].quote_outer_indent;
        int max_depth = nodes[i].quote_depth;

        int j = i + 1;
        while (j < node_count && nodes[j].blockquote_group == group) {
            const float bottom = cache[j].y_position + cache[j].height;
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
            if (group_bottom > viewport_bottom) {
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
                const D2D1_RECT_F bg_rect = D2D1::RectF(
                    outer_x - ALERT_BG_PAD, group_top - ALERT_BG_PAD,
                    outer_x + cw, group_bottom + ALERT_BG_PAD);
                cmds.emplace_back(FillRoundedRectCmd{ bg_rect, ALERT_BG_CORNER, ALERT_BG_CORNER, theme_->alert_bg_color[idx] });
            }
        }

        // GitHub と同様に、外側のバーはネストした範囲を貫通して連続描画する。
        // 各 level ごとに quote_depth >= level の連続区間を求めてバーを発行。
        for (int level = 1; level <= max_depth; ++level) {
            const float bar_indent_x = static_cast<float>(outer_indent + level - 1) * theme_->indent_width;
            const float bar_x = offset_x + bar_indent_x - theme_->indent_width * 0.5f;
            const D2D1_COLOR_F bar_color = (level == 1 && alert_type != AlertType::None) ? theme_->alert_color[AlertColorIndex(alert_type)] : theme_->blockquote_bar_color;

            const auto emit_bar = [&](float top, float bottom) {
                cmds.emplace_back(DrawLineCmd{
                    D2D1::Point2F(bar_x, top - BAR_EXTEND),
                    D2D1::Point2F(bar_x, bottom + BAR_EXTEND),
                    bar_color, theme_->blockquote_bar_width });
            };

            bool in_region = false;
            float region_top = 0.0f;
            float region_bottom = 0.0f;
            for (int k = i; k < j; ++k) {
                if (nodes[k].quote_depth >= level) {
                    if (!in_region) {
                        in_region = true;
                        region_top = cache[k].y_position;
                    }
                    region_bottom = cache[k].y_position + cache[k].height;
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
    cmds.emplace_back(FillRoundedRectCmd{ bg, CODE_BLOCK_CORNER, CODE_BLOCK_CORNER, theme_->code_bg_color });
    if (formats_.placeholder_text) {
        const auto loading_text = i18n::S().loading;
        cmds.emplace_back(MakeTextCmd(loading_text.data(), loading_text.size(), bg, formats_.placeholder_text, theme_->blockquote_text_color));
    }
}

void CommandGenerator::EmitHighlightRects(
    DrawCommandList& cmds,
    IDWriteTextLayout* layout,
    uint32_t start,
    uint32_t length,
    float origin_x,
    float origin_y,
    D2D1_COLOR_F color)
{
    if (!layout || length == 0) {
        return;
    }
    auto& buf = GetHitTestBuffer();
    const UINT32 count = FetchHitTestMetrics(layout, start, length, buf);
    for (UINT32 i = 0; i < count; i++) {
        cmds.emplace_back(FillRectCmd{ RectFromHitTest(buf[i], origin_x, origin_y), color });
    }
}

void CommandGenerator::GenSelectionHighlight(DrawCommandList& cmds, IDWriteTextLayout* layout, uint32_t start, uint32_t length, float origin_x, float origin_y)
{
    EmitHighlightRects(cmds, layout, start, length, origin_x, origin_y, SELECTION_COLOR);
}

void CommandGenerator::CollectHitTestRects(IDWriteTextLayout* layout, uint32_t start, uint32_t length, std::pmr::vector<D2D1_RECT_F>& out)
{
    auto& buf = GetHitTestBuffer();
    const UINT32 count = FetchHitTestMetrics(layout, start, length, buf);
    out.reserve(out.size() + count);
    for (UINT32 i = 0; i < count; i++) {
        out.emplace_back(RectFromHitTest(buf[i]));
    }
}

void CommandGenerator::GenSelectionHighlightCached(DrawCommandList& cmds, const NodeLayoutEntry& entry, uint32_t start, uint32_t length, float origin_x, float origin_y)
{
    auto* layout = entry.text_layout.Get();
    if (!layout || length == 0) {
        return;
    }
    auto& cache = entry.ensure_selection_hl_cache();
    if (cache.layout_ptr != layout || cache.start != start || cache.length != length) {
        cache.rects.clear();
        CollectHitTestRects(layout, start, length, cache.rects);
        cache.layout_ptr = layout;
        cache.start = start;
        cache.length = length;
    }
    for (const auto& r : cache.rects) {
        cmds.emplace_back(FillRectCmd{
            D2D1::RectF(
                origin_x + r.left,
                origin_y + r.top,
                origin_x + r.right,
                origin_y + r.bottom),
            SELECTION_COLOR });
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

void CommandGenerator::RebuildSearchHlCache(SearchHlCache& cache, const NodeLayoutEntry& entry,
                                            std::span<const SearchMatch> matches, size_t first_global, size_t node_match_count)
{
    // キャッシュミス時のみ HitTestTextRange を一括発行。layout 変更時は
    // invalidate_search_hl_cache() でキャッシュ自体が破棄されており、SearchState の
    // generation は 1 から始まるため、cache.gen == search_generation_ のみで
    // キャッシュ有効性を完全判定できる。
    if (cache.gen == search_generation_) {
        return;
    }

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
        if (l && m.length > 0) {
            CollectHitTestRects(l, m.start, m.length, cache.rects);
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
        const bool is_here = (table_row >= 0)
                                 ? (m.table_row == table_row && m.table_col == table_col)
                                 : (m.table_row < 0);
        if (!is_here) {
            continue;
        }

        const uint32_t rb = (node_mi == 0) ? 0 : cache.rect_ends[node_mi - 1];
        const uint32_t re = cache.rect_ends[node_mi];

        const D2D1_COLOR_F color = (static_cast<int>(mi) == current_match_index_)
                                       ? theme_->search_highlight_current_color
                                       : theme_->search_highlight_color;
        for (uint32_t k = rb; k < re; ++k) {
            const auto& r = cache.rects[k];
            cmds.emplace_back(FillRectCmd{
                D2D1::RectF(origin_x + r.left, origin_y + r.top,
                            origin_x + r.right, origin_y + r.bottom),
                color });
        }
    }
}

void CommandGenerator::GenTableRowBg(DrawCommandList& cmds, bool is_header, bool is_even_row, float x, float y, float table_width, float row_h, float border)
{
    if (is_header) {
        cmds.emplace_back(FillRectCmd{
            D2D1::RectF(x, y, x + table_width, y + row_h + border),
            theme_->code_bg_color });
    }
    else if (is_even_row) {
        cmds.emplace_back(FillRectCmd{
            D2D1::RectF(x, y, x + table_width, y + row_h + border),
            cached_stripe_color_ });
    }
}

void CommandGenerator::GenTableCellContent(
    DrawCommandList& cmds,
    const TableCell& cell,
    IDWriteTextLayout* cell_layout,
    float text_x,
    float text_y,
    bool has_selection,
    uint32_t sel_start,
    uint32_t sel_end,
    uint32_t flat_offset)
{
    if (has_selection && cell_layout) {
        const uint32_t cell_len = static_cast<uint32_t>(cell.text.size());
        const uint32_t ov_start = std::max(sel_start, flat_offset);
        const uint32_t ov_end = std::min(sel_end, flat_offset + cell_len);
        if (ov_end > ov_start) {
            GenSelectionHighlight(cmds, cell_layout, ov_start - flat_offset, ov_end - ov_start, text_x, text_y);
        }
    }
    if (cell_layout) {
        const D2D1_COLOR_F cell_color = cell.is_header ? theme_->heading_color : theme_->text_color;
        cmds.emplace_back(DrawTextLayoutCmd{ D2D1::Point2F(text_x, text_y), cell_layout, cell_color });
    }
}

void CommandGenerator::GenTable(DrawCommandList& cmds,
                                const Node& node, const NodeLayoutEntry& entry,
                                int node_index, float offset_x)
{
    if (node.table_rows().empty() || !entry.has_table_layout() || entry.table_layout->col_widths.empty()) {
        return;
    }

    const float cell_padding = TABLE_CELL_PADDING;
    const float border = TABLE_BORDER_WIDTH;
    const auto& tl = *entry.table_layout;
    const auto& selection = *frame_selection_;
    const float viewport_top = frame_viewport_top_;
    const float viewport_bottom = frame_viewport_bottom_;

    const float table_width = tl.cached_table_width;

    bool has_selection = selection.active && (node_index >= selection.start_node) && (node_index <= selection.end_node);
    uint32_t sel_start = 0, sel_end = static_cast<uint32_t>(node.GetText().size());
    if (has_selection) {
        if (node_index == selection.start_node) {
            sel_start = selection.start_pos;
        }
        if (node_index == selection.end_node) {
            sel_end = selection.end_pos;
        }
        if (sel_end <= sel_start) {
            has_selection = false;
        }
    }

    float y = entry.y_position;
    uint32_t flat_offset = 0;
    size_t bg_cursor = 0;

    for (size_t r = 0; r < node.table_rows().size(); r++) {
        const auto& row = node.table_rows()[r];
        const float row_h = (r < tl.row_heights.size()) ? tl.row_heights[r] : (theme_->font_size_body * TABLE_ROW_HEIGHT_FACTOR);

        const float row_bottom = y + row_h + border;
        if (row_bottom < viewport_top || y > viewport_bottom) {
            // プリコンピュート済みの行オフセットを使い、O(cells) の走査を O(1) に削減
            if (r + 1 < node.table_rows().size() && r + 1 < tl.row_flat_offsets.size()) {
                flat_offset = tl.row_flat_offsets[r + 1];
            }
            else {
                TableLayoutData::AdvanceFlatOffsetInRow(row, 0, row.cells.size(), flat_offset);
                if (r + 1 < node.table_rows().size()) {
                    flat_offset++;
                }
            }
            y = row_bottom;
            continue;
        }

        const bool is_header_row = (!row.cells.empty() && row.cells[0].is_header);
        GenTableRowBg(cmds, is_header_row, r % 2 == 0, offset_x, y, table_width, row_h, border);

        // 行上部の水平線
        cmds.emplace_back(DrawLineCmd{
            D2D1::Point2F(offset_x, y), D2D1::Point2F(offset_x + table_width, y),
            theme_->hr_color, border });

        // 可視列のみコマンド生成、画面外は flat_offset のみ進める
        const float cull_left = offset_x + frame_viewport_left_;
        const float cull_right = offset_x + frame_viewport_right_;
        float cx = offset_x + border;
        const size_t drawn_cols = std::min(row.cells.size(), tl.col_widths.size());
        for (size_t c = 0; c < drawn_cols; c++) {
            const auto& cell = row.cells[c];
            const float cw = tl.col_widths[c];
            const float col_right = cx + cw + cell_padding * 2.0f;
            const bool col_visible = (col_right >= cull_left) && (cx - border <= cull_right);

            if (col_visible) {
                cmds.emplace_back(DrawLineCmd{
                    D2D1::Point2F(cx - border, y), D2D1::Point2F(cx - border, y + row_h + border),
                    theme_->hr_color, border });

                const float text_x = cx + cell_padding;
                const float text_y = y + cell_padding;

                IDWriteTextLayout* cell_layout = tl.GetCellLayout(r, c);

                // bg_cursor は cell_index 昇順で進む。
                bg_cursor = GenCellInlineCodeBgs(cmds, tl.cell_inline_code_bgs, bg_cursor, static_cast<uint32_t>(tl.CellIndex(r, c)), text_x, text_y, theme_->code_bg_color);

                GenSearchHighlights(cmds, entry, node_index, text_x, text_y, static_cast<int>(r), static_cast<int>(c));

                GenTableCellContent(cmds, cell, cell_layout, text_x, text_y, has_selection, sel_start, sel_end, flat_offset);
            }

            flat_offset += static_cast<uint32_t>(cell.text.size());
            if (c + 1 < row.cells.size()) {
                flat_offset++;
            }
            cx += cw + cell_padding * 2.0f + border;
        }

        TableLayoutData::AdvanceFlatOffsetInRow(row, drawn_cols, row.cells.size(), flat_offset);

        cmds.emplace_back(DrawLineCmd{
            D2D1::Point2F(offset_x + table_width, y),
            D2D1::Point2F(offset_x + table_width, y + row_h + border),
            theme_->hr_color, border });

        y += row_h + border;
        if (r + 1 < node.table_rows().size()) {
            flat_offset++;
        }
    }

    cmds.emplace_back(DrawLineCmd{
        D2D1::Point2F(offset_x, y), D2D1::Point2F(offset_x + table_width, y),
        theme_->hr_color, border });
}
