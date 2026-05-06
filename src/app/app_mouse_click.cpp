#include "app.h"
#include "app_constants.h"
#include "app_events.h"
#include "app_mouse_helpers.h"
#include "document_utils.h"
#include "i18n.h"
#include "pane_layout.h"
#include "string_convert.h"
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
            EmitEffect(effect::PostWindowMessage{ app_msg::SEARCH_FOCUS, app_param::SEARCH_FOCUS_SELECT_ALL, 0 });
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
        std::pmr::wstring query_wide;
        string_convert::Utf8ToWide(state_.search.search_state.GetQuery(), query_wide);
        const int pos = renderer_.HitTestSearchInput(query_wide, dip_x - text_left, input_w);
        if (is_double_click) {
            // 検索 EDIT は非表示で WM_LBUTTONDBLCLK を直接受けないため、
            // 自前で単語境界を計算して EM_SETSEL を発行する。
            const auto wb = FindWordBoundaries(std::wstring_view{ query_wide }, static_cast<uint32_t>(pos));
            if (wb.found) {
                EmitEffect(effect::PostWindowMessage{
                    app_msg::SEARCH_FOCUS,
                    app_param::SEARCH_FOCUS_SET_SELECTION,
                    MAKELPARAM(static_cast<int>(wb.start), static_cast<int>(wb.end)) });
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
        CopyCodeBlockToClipboard(btn_hit.copy_node);
        return;
    }
    if (btn_hit.svg_copy_node >= 0) {
        CopyDiagramAsSvg(btn_hit.svg_copy_node);
        return;
    }
    if (btn_hit.save_node >= 0) {
        SaveDiagramAsPng(btn_hit.save_node);
        return;
    }
    if (IsOverMdScrollbar(dip_x, dip_y, pane_layout)) {
        Dispatch(MdScrollbarDragStartedAction{ dip_y, layout_service_->GetTotalHeight() });
        return;
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
    renderer_.InvalidateFilePaneCache();
    Invalidate();
}

void App::HandleFilePaneClick(float dip_x, float dip_y, const PaneLayout& layout)
{
    using mendo::app_mouse::ProcessSidePaneHeaderClick;

    const auto& theme = renderer_.GetTheme();

    if (ProcessSidePaneHeaderClick(dip_x, dip_y, layout.file_rect, theme.pane_header_height, true, [this]() {
        state_.view.panes.ToggleFilePane();
        RefreshPaneLayout();
    }, [this]() {
        RefreshFilePane();
    })) {
        return;
    }

    const float total_content = static_cast<float>(state_.file_explorer.GetEntries().size()) * theme.pane_item_height;
    const auto scroll_info = ComputePaneScrollInfo(layout.file_rect, total_content);

    if (IsOverPaneScrollbar(dip_x, layout.file_rect, total_content, scroll_info)) {
        Dispatch(PaneScrollbarDragStartedAction{ PaneTarget::File, dip_y });
        return;
    }
    const float local_y = dip_y - scroll_info.content_top + state_.view.panes.FileScroll().scroll_y;
    const int idx = state_.file_explorer.HitTest(local_y, theme.pane_item_height);
    if (idx >= 0 && idx < static_cast<int>(state_.file_explorer.GetEntries().size())) {
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
}

void App::HandleTocPaneClick(float dip_x, float dip_y, const PaneLayout& layout)
{
    using mendo::app_mouse::ProcessSidePaneHeaderClick;

    const auto& theme = renderer_.GetTheme();

    if (ProcessSidePaneHeaderClick(dip_x, dip_y, layout.toc_rect, theme.pane_header_height, false, [this]() {
        state_.view.panes.ToggleTocPane();
        RefreshPaneLayout();
    }, []() {})) {
        return;
    }

    const float total_content = static_cast<float>(state_.document.doc.GetToc().GetEntries().size()) * theme.pane_item_height;
    const auto scroll_info = ComputePaneScrollInfo(layout.toc_rect, total_content);

    if (IsOverPaneScrollbar(dip_x, layout.toc_rect, total_content, scroll_info)) {
        Dispatch(PaneScrollbarDragStartedAction{ PaneTarget::Toc, dip_y });
        return;
    }
    const float local_y = dip_y - scroll_info.content_top + state_.view.panes.TocScroll().scroll_y;
    const int idx = state_.document.doc.GetToc().HitTest(local_y, theme.pane_item_height);
    if (idx >= 0 && idx < static_cast<int>(state_.document.doc.GetToc().GetEntries().size())) {
        const auto& toc_entry = state_.document.doc.GetToc().GetEntries()[idx];
        const auto anchor = state_.document.doc.GetNodes()[toc_entry.node_index].anchor_id();
        Dispatch(TocItemClickedAction{ std::pmr::string(anchor) });
    }
}
