#include "app.h"
#include "app_constants.h"
#include "app_events.h"
#include "app_mouse_helpers.h"
#include "block_h_scroll.h"
#include "document_utils.h"
#include "i18n.h"
#include "layout_computer.h"
#include "pane_layout.h"
#include "ui_constants.h"

bool App::HandleSearchBarClick(float dip_x, float dip_y, const PaneLayout& pane_layout, bool is_double_click)
{
    if (!state_.search.search_state.IsVisible()) {
        return false;
    }
    const auto& r = pane_layout.md_rect;
    const auto sbl = ComputeSearchBarLayout(r.x, r.width, r.y + r.height, !state_.search.search_state.GetQuery().empty());
    if (dip_y < sbl.bar_top) {
        return false;
    }
    switch (HitTestSearchBar(sbl, dip_x, dip_y)) {
    case SearchBarHitZone::None:
        if (!is_double_click) {
            EmitEffect(effect::SearchFocus{});
        }
        break;
    case SearchBarHitZone::Up:
        OnSearchPrev();
        break;
    case SearchBarHitZone::Down:
        OnSearchNext();
        break;
    case SearchBarHitZone::CaseSensitive:
        OnToggleCaseSensitive();
        break;
    case SearchBarHitZone::Highlight:
        OnToggleHighlight();
        break;
    case SearchBarHitZone::Close:
        OnSearchClose();
        break;
    case SearchBarHitZone::Input: {
        const float text_left = sbl.input_rect.left + SEARCH_INPUT_TEXT_PAD_LEFT;
        const float input_w = sbl.input_rect.right - SEARCH_INPUT_TEXT_PAD_RIGHT - text_left;
        const auto& query_wide = state_.search.search_bar_ctrl.GetQueryWide();
        const int pos = renderer_.HitTestSearchInput(query_wide, dip_x - text_left, input_w);
        if (is_double_click) {
            // 検索 EDIT は非表示で WM_LBUTTONDBLCLK を直接受けないため、
            // 自前で単語境界を計算して EM_SETSEL を発行する。
            const auto wb = FindWordBoundaries(std::wstring_view{ query_wide }, static_cast<uint32_t>(pos));
            if (wb.found) {
                EmitEffect(effect::SearchFocus{
                    effect::SearchFocus::Mode::SetSelection,
                    static_cast<int>(wb.start),
                    static_cast<int>(wb.end),
                });
            }
        }
        else {
            Dispatch(SearchInputDragStartedAction{ pos });
        }
        break;
    }
    default:
        std::unreachable();
    }
    return true;
}

void App::HandleMdPaneClick(float dip_x, float dip_y, int px, int py, const PaneLayout& pane_layout)
{
    if (HandleSearchBarClick(dip_x, dip_y, pane_layout, false)) {
        return;
    }

    const auto nav_hit = hit_test_.NavButtonHitTest(dip_x, dip_y, pane_layout.md_rect);
    if (nav_hit == NavButtonHover::Back) {
        Dispatch(NavigateBackAction{});
        return;
    }
    if (nav_hit == NavButtonHover::Forward) {
        Dispatch(NavigateForwardAction{});
        return;
    }
    // コピー/SVG コピー/保存ボタンを 1 回の可視ノード走査でまとめて判定する。
    const auto hit_ctx = BuildMdPaneHitContext(px, py, pane_layout);
    const auto btn_hit = hit_test_.CodeBlockButtonsHitTest(hit_ctx);
    if (btn_hit.copy_node >= 0) {
        const bool dark = renderer_.GetTheme().IsDark();
        clipboard_manager_.CopyCodeBlock(state_.document.doc, btn_hit.copy_node, dark);
        return;
    }
    if (btn_hit.svg_copy_node >= 0 || btn_hit.save_node >= 0) {
        const float md_width = renderer_.GetTheme().ContentWidth(GetMarkdownPaneWidth());
        const bool dark = renderer_.GetTheme().IsDark();
        if (btn_hit.svg_copy_node >= 0) {
            clipboard_manager_.CopyDiagramAsSvg(state_.document.doc, btn_hit.svg_copy_node, md_width, dark);
            return;
        }
        clipboard_manager_.SaveDiagramAsPng(state_.document.doc, btn_hit.save_node, md_width, dark);
        return;
    }
    if (IsOverMdScrollbar(dip_x, dip_y, pane_layout)) {
        Dispatch(MdScrollbarDragStartedAction{ dip_y, layout_service_->GetTotalHeight() });
        return;
    }

    // ホバー中ブロックの水平スクロールバー上ならドラッグ開始 (テキスト選択より優先)。
    if (state_.view.hovered_h_block >= 0) {
        const int hover = state_.view.hovered_h_block;
        const auto& nodes = state_.document.doc.GetNodes();
        const auto& cache = state_.document.layout_cache;
        if (hover < static_cast<int>(nodes.size()) && hover < static_cast<int>(cache.size())) {
            const auto& node = nodes[hover];
            const auto& entry = cache[hover];
            const auto& theme = renderer_.GetTheme();
            const auto geom = GetBlockHScrollGeometry(node, entry, theme, pane_layout.md_rect.width);
            if (geom.can_scroll()) {
                const float pad = IsScrollableCodeBlock(node) ? theme.code_block_padding : 0.0f;
                const float bar_y_local = BlockHScrollbarBarY(entry.text_top, entry.height, pad);
                // 描画 transform は Translation(md_x, -scroll_y) で md_rect.y は加算しない規約。
                const float bar_y_screen = bar_y_local - state_.view.viewport.GetScrollY();
                const float block_x_screen = pane_layout.md_rect.x + theme.margin_left + mendo::layout::NodeIndent(node, theme);
                if (PointInRect(dip_x, dip_y, BlockHScrollbarHitRect(block_x_screen, geom.visible_width, bar_y_screen))) {
                    Dispatch(BlockHScrollDragStartedAction{ hover, dip_x });
                    return;
                }
            }
        }
    }

    const auto hit = HitTest(px, py);
    Dispatch(TextSelectionStartedAction{ hit.node_index, hit.text_pos, px, py });
}

bool App::IsOverPaneScrollbar(float dip_x, const PaneRect& rect, float total_content, const PaneScrollInfo& scroll_info) noexcept
{
    const float local_x = dip_x - rect.x;
    const float hit_left = rect.width - PANE_SCROLLBAR_WIDTH - PANE_SCROLLBAR_MARGIN - PANE_SCROLLBAR_HIT_PADDING;
    return local_x >= hit_left && total_content > scroll_info.content_height;
}

void App::RefreshFilePane()
{
    state_.file_explorer.Refresh();
    if (!state_.document.doc.GetFilePath().empty()) {
        state_.file_explorer.SetCurrentFile(state_.document.doc.GetFilePath());
    }
    renderer_.InvalidateSidePaneCache(PaneTarget::File);
    Invalidate();
}

void App::HandleSidePaneClick(PaneTarget target, float dip_x, float dip_y, const PaneLayout& layout)
{
    using mendo::app_mouse::ProcessSidePaneHeaderClick;

    const auto& theme = renderer_.GetTheme();
    const bool is_file = target == PaneTarget::File;
    const PaneRect& rect = layout.Get(target);

    if (ProcessSidePaneHeaderClick(dip_x, dip_y, rect, theme.pane_header_height, is_file, [this, target]() {
        state_.view.panes.ToggleSidePane(target);
        RefreshPaneLayout();
    }, [this]() {
        RefreshFilePane();
    })) {
        return;
    }

    const int item_count = is_file
        ? static_cast<int>(state_.file_explorer.GetEntries().size())
        : static_cast<int>(state_.document.doc.GetToc().GetEntries().size());
    const float total_content = static_cast<float>(item_count) * theme.pane_item_height;
    const auto scroll_info = ComputePaneScrollInfo(rect, total_content);

    if (IsOverPaneScrollbar(dip_x, rect, total_content, scroll_info)) {
        Dispatch(PaneScrollbarDragStartedAction{ target, dip_y });
        return;
    }
    const float local_y = dip_y - scroll_info.content_top + state_.view.panes.SidePaneScroll(target).scroll_y;
    const int idx = is_file
        ? state_.file_explorer.HitTest(local_y, theme.pane_item_height)
        : state_.document.doc.GetToc().HitTest(local_y, theme.pane_item_height);
    if (idx < 0 || idx >= item_count) {
        return;
    }
    if (is_file) {
        const auto& file_entry = state_.file_explorer.GetEntries()[idx];
        if (file_entry.is_directory()) {
            Dispatch(FilePaneDirectoryClickedAction{ file_entry.full_path });
        }
        else if (!file_entry.is_current()) {
            if (GetFileAttributesW(file_entry.full_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                RefreshFilePane();
                ShowToast(i18n::S().toast_file_not_found);
                return;
            }
            Dispatch(FilePaneFileClickedAction{ file_entry.full_path });
        }
    }
    else {
        const auto& toc_entry = state_.document.doc.GetToc().GetEntries()[idx];
        const auto anchor = state_.document.doc.GetNodes()[toc_entry.node_index].anchor_id();
        Dispatch(TocItemClickedAction{ std::pmr::string(anchor) });
    }
}
