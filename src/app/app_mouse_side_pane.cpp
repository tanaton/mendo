// サイドペイン(File/TOC)のクリック・ペインスクロールバーのクリック処理。
#include "app.h"
#include "app_constants.h"
#include "app_mouse_helpers.h"
#include "document_utils.h"
#include "i18n.h"
#include "pane_layout.h"
#include "ui_constants.h"

void App::RefreshFilePane()
{
    state_.file_explorer.Refresh();
    if (!state_.document.doc.GetFilePath().empty()) {
        state_.file_explorer.SetCurrentFile(state_.document.doc.GetFilePath());
    }
    renderer_.InvalidateFilePaneCache();
    Invalidate();
}

bool App::TryHandlePaneScrollbarClick(float dip_x, float dip_y, const PaneRect& rect,
    PaneController::DragTarget target,
    const PaneScrollInfo& scroll_info,
    float total_content, ScrollState& scroll,
    void (Renderer::* invalidate)())
{
    const float local_x = dip_x - rect.x;

    if ((local_x >= rect.width - PANE_SCROLLBAR_WIDTH - 4.0f) && (total_content > scroll_info.content_height)) {
        SetCapture(hwnd_);
        state_.view.panes.StartDrag(target);
        bool dirty = false;
        HandleScrollbarClick(dip_y, scroll_info, scroll, dirty);
        if (dirty) {
            (renderer_.*invalidate)();
        }
        return true;
    }
    return false;
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

    if (TryHandlePaneScrollbarClick(dip_x, dip_y, layout.file_rect,
        PaneController::DragTarget::FileScrollbar,
        scroll_info, total_content, state_.view.panes.FileScroll(),
        &Renderer::InvalidateFilePaneCache)) {
        return;
    }
    const float local_y = dip_y - scroll_info.content_top + state_.view.panes.FileScroll().scroll_y;
    const int idx = state_.file_explorer.HitTest(local_y, theme.pane_item_height);
    if (idx >= 0 && idx < static_cast<int>(state_.file_explorer.GetEntries().size())) {
        const auto& file_entry = state_.file_explorer.GetEntries()[idx];
        if (file_entry.is_directory) {
            state_.file_explorer.SetDirectory(file_entry.full_path);
            if (!state_.document.doc.GetFilePath().empty()) {
                state_.file_explorer.SetCurrentFile(state_.document.doc.GetFilePath());
            }
            state_.view.panes.FileScroll() = {};
            renderer_.InvalidateFilePaneCache();
            Invalidate();
        }
        else if (!file_entry.is_current) {
            if (GetFileAttributesW(file_entry.full_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                RefreshFilePane();
                ShowToast(i18n::S().toast_file_not_found);
                return;
            }
            PushNavHistory();
            LoadMarkdownFile(file_entry.full_path);
        }
    }
}

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

    if (TryHandlePaneScrollbarClick(dip_x, dip_y, layout.toc_rect,
        PaneController::DragTarget::TocScrollbar,
        scroll_info, total_content, state_.view.panes.TocScroll(),
        &Renderer::InvalidateTocPaneCache)) {
        return;
    }
    const float local_y = dip_y - scroll_info.content_top + state_.view.panes.TocScroll().scroll_y;
    const int idx = state_.document.doc.GetToc().HitTest(local_y, theme.pane_item_height);
    if (idx >= 0 && idx < static_cast<int>(state_.document.doc.GetToc().GetEntries().size())) {
        PushNavHistory();
        const auto& toc_entry = state_.document.doc.GetToc().GetEntries()[idx];
        NavigateToAnchor(state_.document.doc.GetNodes()[toc_entry.node_index].anchor_id());
    }
}
