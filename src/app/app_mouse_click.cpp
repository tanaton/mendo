#include "app.h"
#include "app_constants.h"
#include "app_events.h"
#include "app_mouse_helpers.h"
#include "document_utils.h"
#include "i18n.h"
#include "pane_layout.h"
#include "ui_constants.h"

// ============================================================
// MD ペイン — クリック
// ============================================================

void App::HandleMdPaneClick(float dip_x, float dip_y, int px, int py, const PaneLayout& pane_layout)
{
    // 検索バーのクリック処理
    if (state_.search.search_state.IsVisible()) {
        const auto& r = pane_layout.md_rect;
        const auto sbl = ComputeSearchBarLayout(r.x, r.width, r.y + r.height, !state_.search.search_state.GetQuery().empty());
        if (dip_y >= sbl.bar_top) {
            if (PointInRect(dip_x, dip_y, sbl.up_btn)) {
                OnSearchPrev();
                return;
            }
            if (PointInRect(dip_x, dip_y, sbl.down_btn)) {
                OnSearchNext();
                return;
            }
            if (PointInRect(dip_x, dip_y, sbl.case_btn)) {
                OnToggleCaseSensitive();
                return;
            }
            if (PointInRect(dip_x, dip_y, sbl.highlight_btn)) {
                OnToggleHighlight();
                return;
            }
            if (PointInRect(dip_x, dip_y, sbl.close_btn)) {
                OnSearchClose();
                return;
            }
            if (PointInRect(dip_x, dip_y, sbl.input_rect)) {
                const float text_left = sbl.input_rect.left + SEARCH_INPUT_TEXT_PAD_LEFT;
                const float input_w = sbl.input_rect.right - SEARCH_INPUT_TEXT_PAD_RIGHT - text_left;
                const int pos = renderer_.HitTestSearchInput(state_.search.search_state.GetQuery(), dip_x - text_left, input_w);
                Dispatch(SearchInputDragStartedAction{ pos });
                return;
            }
            PostMessage(hwnd_, app_msg::SEARCH_FOCUS, app_param::SEARCH_FOCUS_SELECT_ALL, 0);
            return;
        }
    }

    const auto nav_hit = state_.hit_test.NavButtonHitTest(dip_x, dip_y, pane_layout.md_rect);
    if (nav_hit == NavButtonHover::Back) {
        Dispatch(NavigateBackAction{});
        return;
    }
    if (nav_hit == NavButtonHover::Forward) {
        Dispatch(NavigateForwardAction{});
        return;
    }
    // コピーボタンのクリック判定（クリック位置で再判定）
    const float content_width = renderer_.GetTheme().ContentWidth(pane_layout.md_rect.width);
    const MdPaneHitContext hit_ctx{
        state_.document.doc.GetNodes(), state_.document.layout_cache, renderer_.GetTheme(),
        state_.view.viewport.GetScrollY(), pane_layout.md_rect.x,
        state_.window.cached_dpi_scale, px, py,
        content_width, pane_layout.md_rect.height
    };
    const auto copy_node = state_.hit_test.CopyButtonHitTest(hit_ctx);
    if (copy_node >= 0) {
        CopyCodeBlockToClipboard(copy_node);
        return;
    }
    // 保存ボタンのクリック判定
    const auto save_node = state_.hit_test.SaveButtonHitTest(hit_ctx);
    if (save_node >= 0) {
        SaveDiagramAsPng(save_node);
        return;
    }
    // MDペインスクロールバーのクリック判定
    if (IsOverMdScrollbar(dip_x, dip_y, pane_layout)) {
        Dispatch(MdScrollbarDragStartedAction{ dip_y, layout_service_->GetTotalHeight() });
        return;
    }

    const auto hit = HitTest(px, py);
    Dispatch(TextSelectionStartedAction{ hit.node_index, hit.text_pos, px, py });
}

// ============================================================
// サイドペイン共通 — ペインスクロールバーの当たり判定
// ============================================================

bool App::IsOverPaneScrollbar(float dip_x, const PaneRect& rect,
    float total_content, const PaneScrollInfo& scroll_info) noexcept
{
    const float local_x = dip_x - rect.x;
    const float hit_left = rect.width - PANE_SCROLLBAR_WIDTH - PANE_SCROLLBAR_MARGIN - PANE_SCROLLBAR_HIT_PADDING;
    return local_x >= hit_left && total_content > scroll_info.content_height;
}

// ============================================================
// ファイルペイン — クリック
// ============================================================

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

    if (ProcessSidePaneHeaderClick(dip_x, dip_y, layout.file_rect, theme.pane_header_height, true,
        [this]() { state_.view.panes.ToggleFilePane(); RefreshPaneLayout(); },
        [this]() { RefreshFilePane(); })) {
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
        if (file_entry.is_directory) {
            Dispatch(FilePaneDirectoryClickedAction{ file_entry.full_path });
        }
        else if (!file_entry.is_current) {
            if (GetFileAttributesW(file_entry.full_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                RefreshFilePane();
                ShowToast(i18n::S().toast_file_not_found);
                return;
            }
            Dispatch(FilePaneFileClickedAction{ file_entry.full_path });
        }
    }
}

// ============================================================
// 目次ペイン — クリック
// ============================================================

void App::HandleTocPaneClick(float dip_x, float dip_y, const PaneLayout& layout)
{
    using mendo::app_mouse::ProcessSidePaneHeaderClick;

    const auto& theme = renderer_.GetTheme();

    if (ProcessSidePaneHeaderClick(dip_x, dip_y, layout.toc_rect, theme.pane_header_height, false,
        [this]() { state_.view.panes.ToggleTocPane(); RefreshPaneLayout(); },
        []() {})) {
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
        Dispatch(TocItemClickedAction{ std::pmr::wstring(anchor) });
    }
}
