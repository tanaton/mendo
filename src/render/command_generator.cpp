#include "command_generator.h"
#include "i18n.h"
#include "layout.h"
#include "ui_constants.h"
#include <algorithm>
#include <format>

// h1/h2 見出し下線を描画するY座標を heading_spacing_below_h1h2 のこの比率で決める。
// 値を小さくすると下線は見出し文字に近づき、下線と次行の間隔が広がる。
static constexpr float HEADING_UNDERLINE_OFFSET_RATIO = 0.25f;

static float GetFirstLineHeight(IDWriteTextLayout* layout, float font_size)
{
    float h = font_size * FALLBACK_LINE_HEIGHT_FACTOR;
    if (layout) {
        DWRITE_LINE_METRICS lm;
        UINT32 lc;
        if (SUCCEEDED(layout->GetLineMetrics(&lm, 1, &lc)) && lc > 0) {
            h = lm.height;
        }
    }
    return h;
}

const DrawCommandList& CommandGenerator::GenerateMdPane(
    const std::pmr::vector<Node>& nodes, const LayoutCache& cache,
    const PaneRect& md_pane_rect, float scroll_y,
    const TextSelection& selection,
    int first_visible,
    int hovered_copy_node,
    int hovered_save_node,
    float dpi_scale)
{
    // 古いコマンドリストを破棄し、monotonic リソースをリセットして再利用する。
    // clear() で要素を破棄してから Reset() を呼び、新しいリストを作成する。
    // monotonic_buffer_resource::deallocate は no-op のため、
    // clear() 後のバッファポインタが残っていても安全。
    cmds_.clear();
    frame_resource_.Reset();
    cmds_ = DrawCommandList{ frame_resource_.resource() };
    auto& cmds = cmds_;

    // MDペインの境界でクリップ
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
    frame_hovered_copy_node_ = hovered_copy_node;
    frame_hovered_save_node_ = hovered_save_node;

    // 最初の可視ノードを二分探索で検索（事前計算済みのインデックスがあればそれを使用）
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
    return cmds_;
}

void CommandGenerator::GenerateNode(DrawCommandList& cmds,
    const Node& node, const NodeLayoutEntry& entry, const DiagramEntry& diagram,
    int node_index)
{
    // ビューポート外のノードをカリング
    // h1/h2は見出し下線がentry.heightの外に描画されるため、カリング境界を拡張する。
    float node_bottom = entry.y_position + entry.height;
    if (node.type == NodeType::Heading && node.heading_level <= 2) {
        node_bottom += theme_->heading_spacing_below_h1h2 * HEADING_UNDERLINE_OFFSET_RATIO + theme_->GetHeadingUnderlineThickness(node.heading_level);
    }
    if (node_bottom < frame_viewport_top_ || entry.y_position > frame_viewport_bottom_) {
        return;
    }

    const float indent = node.indent_level * theme_->indent_width;
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
            const float draw_w = (diagram.height > 0)
                ? diagram.width * (draw_h / diagram.height)
                : diagram.width;
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
                GenSaveButton(cmds, bmp.right, bmp.top, node_index == frame_hovered_save_node_);
            }
            else {
                GenDiagramPlaceholder(cmds, x, entry.y_position, cw, entry.height);
            }
            return;
        }
        GenCodeBlockBg(cmds, entry, x, cw);
        GenCopyButton(cmds, entry, x, cw, node_index == frame_hovered_copy_node_);
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
    default:
        break;
    }

    if (!entry.text_layout) {
        return;
    }

    // ノードタイプに応じたベースカラーを決定
    D2D1_COLOR_F base_color = theme_->text_color;
    if (node.type == NodeType::Heading) {
        base_color = theme_->heading_color;
    }
    else if (node.type == NodeType::BlockQuote) {
        base_color = (node.alert_type != AlertType::None)
            ? theme_->text_color
            : theme_->blockquote_text_color;
    }
    else if (node.type == NodeType::CodeBlock) {
        base_color = theme_->code_text_color;
    }

    // インラインコードの背景
    GenInlineCodeBgs(cmds, entry.inline_code_bgs, text_x, entry.y_position, theme_->code_bg_color);

    // 検索マッチのハイライト（選択より先に描画し、選択が最前面になるようにする）
    GenSearchHighlights(cmds, entry.text_layout.Get(), node_index, text_x, entry.y_position);

    // 選択範囲のハイライト
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
            GenSelectionHighlight(cmds, entry.text_layout.Get(),
                sel_start, sel_end - sel_start, text_x, entry.y_position);
        }
    }

    // メインテキスト
    cmds.emplace_back(DrawTextLayoutCmd{ D2D1::Point2F(text_x, entry.y_position), entry.text_layout.Get(), base_color });

    // タスクリストのチェックボックス
    if (node.type == NodeType::TaskListItem && formats_.icon_font) {
        const wchar_t icon = node.task_checked ? L'\u2611' : L'\u2610'; // ☑ / ☐
        const float icon_size = theme_->font_size_body;
        const float cb_x = x - theme_->list_bullet_offset;
        cmds.emplace_back(MakeTextCmd(
            &icon, 1,
            D2D1::RectF(cb_x, entry.y_position, cb_x + icon_size, entry.y_position + icon_size * TASK_CHECKBOX_HEIGHT_FACTOR),
            formats_.icon_font,
            theme_->text_color
        ));
    }
}

// ---- サブジェネレータ ----

void CommandGenerator::GenHorizontalRule(DrawCommandList& cmds,
    const NodeLayoutEntry& entry, float x, float w)
{
    const float y = entry.y_position + theme_->paragraph_spacing * 0.5f;
    cmds.emplace_back(DrawLineCmd{
        D2D1::Point2F(x, y), D2D1::Point2F(x + w, y),
        theme_->hr_color, theme_->hr_thickness });
}

void CommandGenerator::GenCodeBlockBg(DrawCommandList& cmds,
    const NodeLayoutEntry& entry, float x, float w)
{
    const float pad = theme_->code_block_padding;
    const D2D1_RECT_F bg_rect = D2D1::RectF(
        x,
        entry.y_position - pad,
        x + w,
        entry.y_position + entry.height + pad
    );
    cmds.emplace_back(FillRoundedRectCmd{ bg_rect, CODE_BLOCK_CORNER, CODE_BLOCK_CORNER, theme_->code_bg_color });
}

void CommandGenerator::GenCopyButton(DrawCommandList& cmds,
    const NodeLayoutEntry& entry, float x, float w, bool is_hovered)
{
    if (!formats_.copy_btn_icon) {
        return;
    }
    const float pad = theme_->code_block_padding;
    const D2D1_RECT_F btn = OverlayButtonRect(x + w, entry.y_position - pad);
    GenOverlayButton(cmds, btn, L'\uE8C8', is_hovered);
}

void CommandGenerator::GenSaveButton(DrawCommandList& cmds,
    float bitmap_right, float bitmap_top, bool is_hovered)
{
    if (!formats_.copy_btn_icon) {
        return;
    }
    const D2D1_RECT_F btn = OverlayButtonRect(bitmap_right, bitmap_top);
    GenOverlayButton(cmds, btn, L'\uE896', is_hovered);
}

void CommandGenerator::GenOverlayButton(DrawCommandList& cmds,
    D2D1_RECT_F btn, wchar_t icon, bool is_hovered)
{
    const float bg_alpha = is_hovered
        ? (cached_is_dark_ ? 0.30f : 0.15f)
        : (cached_is_dark_ ? 0.10f : 0.05f);
    const D2D1_COLOR_F bg_color = cached_is_dark_
        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, bg_alpha)
        : D2D1::ColorF(0.0f, 0.0f, 0.0f, bg_alpha);
    cmds.emplace_back(FillRoundedRectCmd{ btn, COPY_BTN_CORNER, COPY_BTN_CORNER, bg_color });

    const float text_alpha = is_hovered
        ? (cached_is_dark_ ? 0.9f : 0.8f)
        : (cached_is_dark_ ? 0.4f : 0.35f);
    const D2D1_COLOR_F icon_color = cached_is_dark_
        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, text_alpha)
        : D2D1::ColorF(0.0f, 0.0f, 0.0f, text_alpha);
    cmds.emplace_back(MakeTextCmd(&icon, 1, btn, formats_.copy_btn_icon, icon_color));
}

void CommandGenerator::GenListBullet(DrawCommandList& cmds,
    const Node& node, const NodeLayoutEntry& entry, float x)
{
    if (node.list_number > 0) {
        // 順序付きリストの番号（1行目に合わせて配置）
        if (formats_.list_number) {
            const float first_line_h = GetFirstLineHeight(entry.text_layout.Get(), theme_->font_size_body);
            wchar_t num_buf[16];
            const auto fmt_result = std::format_to_n(num_buf, std::size(num_buf), L"{}.", node.list_number);
            const size_t num_len = std::min(static_cast<size_t>(fmt_result.size), std::size(num_buf));
            const D2D1_RECT_F num_rect = D2D1::RectF(
                x - theme_->list_bullet_offset - LIST_NUMBER_PAD_RIGHT,
                entry.y_position,
                x - LIST_NUMBER_PAD_LEFT,
                entry.y_position + first_line_h
            );
            cmds.emplace_back(MakeTextCmd(num_buf, num_len, num_rect, formats_.list_number, theme_->text_color));
        }
    }
    else {
        // 順序なしリストの箇条書き記号（1行目の中央に配置）
        const float first_line_h = GetFirstLineHeight(entry.text_layout.Get(), theme_->font_size_body);
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

void CommandGenerator::GenBlockQuoteGroupDecorations(DrawCommandList& cmds,
    const std::pmr::vector<Node>& nodes, const LayoutCache& cache,
    int node_count, int first_visible)
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

        // グループの範囲を特定
        const float group_top = cache[i].y_position;
        float group_bottom = cache[i].y_position + cache[i].height;
        const AlertType alert_type = nodes[i].alert_type;
        int bar_indent = -1;
        if (nodes[i].type == NodeType::BlockQuote) {
            bar_indent = nodes[i].indent_level;
        }

        int j = i + 1;
        while (j < node_count && nodes[j].blockquote_group == group) {
            const float bottom = cache[j].y_position + cache[j].height;
            if (bottom > group_bottom) {
                group_bottom = bottom;
            }
            if (bar_indent < 0 && nodes[j].type == NodeType::BlockQuote) {
                bar_indent = nodes[j].indent_level;
            }
            j++;
            // ビューポート外に大きく超えたグループ末尾の走査を打ち切る。
            // バーや背景はクリップ領域で切られるため、描画結果に影響しない。
            if (group_bottom > viewport_bottom) {
                // グループ末尾までスキップ
                while (j < node_count && nodes[j].blockquote_group == group) {
                    j++;
                }
                break;
            }
        }

        if (bar_indent < 0) {
            // BlockQuote ノードがないグループはスキップ
            i = j;
            continue;
        }

        const float indent = static_cast<float>(bar_indent) * theme_->indent_width;
        const float x = offset_x + indent;
        const float bar_x = x - theme_->indent_width * 0.5f;

        if (alert_type != AlertType::None) {
            const auto idx = AlertColorIndex(alert_type);
            if (idx < ALERT_TYPE_COUNT) {
                const float cw = content_width - indent;
                const D2D1_RECT_F bg_rect = D2D1::RectF(
                    x - ALERT_BG_PAD, group_top - ALERT_BG_PAD,
                    x + cw, group_bottom + ALERT_BG_PAD);
                cmds.emplace_back(FillRoundedRectCmd{ bg_rect, ALERT_BG_CORNER, ALERT_BG_CORNER, theme_->alert_bg_color[idx] });
                cmds.emplace_back(DrawLineCmd{
                    D2D1::Point2F(bar_x, group_top - BAR_EXTEND),
                    D2D1::Point2F(bar_x, group_bottom + BAR_EXTEND),
                    theme_->alert_color[idx], theme_->blockquote_bar_width });
            }
        }
        else {
            cmds.emplace_back(DrawLineCmd{
                D2D1::Point2F(bar_x, group_top - BAR_EXTEND),
                D2D1::Point2F(bar_x, group_bottom + BAR_EXTEND),
                theme_->blockquote_bar_color, theme_->blockquote_bar_width });
        }

        i = j;
    }
}

void CommandGenerator::GenDiagramPlaceholder(DrawCommandList& cmds,
    float x, float y, float w, float h)
{
    const D2D1_RECT_F bg = D2D1::RectF(x, y, x + w, y + h);
    cmds.emplace_back(FillRoundedRectCmd{ bg, CODE_BLOCK_CORNER, CODE_BLOCK_CORNER, theme_->code_bg_color });
    if (formats_.placeholder_text) {
        const auto loading_text = i18n::S().loading;
        cmds.emplace_back(MakeTextCmd(loading_text.data(), loading_text.size(), bg, formats_.placeholder_text, theme_->blockquote_text_color));
    }
}

void CommandGenerator::EmitHighlightRects(DrawCommandList& cmds,
    IDWriteTextLayout* layout, uint32_t start, uint32_t length,
    float origin_x, float origin_y, D2D1_COLOR_F color)
{
    if (!layout || length == 0) {
        return;
    }
    auto& buf = GetHitTestBuffer();
    const UINT32 count = FetchHitTestMetrics(layout, start, length, buf);
    for (UINT32 i = 0; i < count; i++) {
        cmds.emplace_back(FillRectCmd{
            D2D1::RectF(
                origin_x + buf[i].left,
                origin_y + buf[i].top,
                origin_x + buf[i].left + buf[i].width,
                origin_y + buf[i].top + buf[i].height
            ), color });
    }
}

void CommandGenerator::GenSelectionHighlight(DrawCommandList& cmds,
    IDWriteTextLayout* layout, uint32_t start, uint32_t length,
    float origin_x, float origin_y)
{
    EmitHighlightRects(cmds, layout, start, length, origin_x, origin_y, SELECTION_COLOR);
}

void CommandGenerator::GenSearchHighlights(DrawCommandList& cmds, IDWriteTextLayout* layout,
    int node_index, float origin_x, float origin_y,
    int table_row, int table_col)
{
    if (!layout || !search_matches_ || search_matches_->empty()) {
        return;
    }

    const auto& matches = *search_matches_;
    auto it = std::ranges::lower_bound(matches, node_index, {}, &SearchMatch::node_index);
    for (int mi = static_cast<int>(it - matches.begin()); mi < static_cast<int>(matches.size()); mi++) {
        const auto& m = matches[mi];
        if (m.node_index > node_index) {
            break;
        }
        if (table_row >= 0 && (m.table_row != table_row || m.table_col != table_col)) {
            continue;
        }
        if (table_row < 0 && m.table_row >= 0) {
            continue;
        }

        const D2D1_COLOR_F color = (mi == current_match_index_)
            ? theme_->search_highlight_current_color
            : theme_->search_highlight_color;
        EmitHighlightRects(cmds, layout, m.start, m.length, origin_x, origin_y, color);
    }
}
