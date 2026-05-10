#include "persistence_controller.h"
#include "app_state.h"
#include "config_service.h"
#include "document_utils.h"
#include "pane_controller.h"

void PersistenceController::SaveLastFilePath()
{
    if (!IsHelpPath(state_.document.doc.GetFilePath())) {
        session_.SaveLastFilePath(state_.document.doc.GetFilePath());
    }
}

void PersistenceController::SavePaneState()
{
    const auto& panes = state_.view.panes;
    SessionService::PaneState s{
        .show_file = panes.IsSidePaneVisible(PaneTarget::File),
        .show_toc = panes.IsSidePaneVisible(PaneTarget::Toc),
        .file_width = panes.GetSidePaneWidth(PaneTarget::File),
        .toc_width = panes.GetSidePaneWidth(PaneTarget::Toc),
    };
    session_.SavePaneState(s);
}

void PersistenceController::LoadPaneState(HWND hwnd)
{
    float client_width = 0.0f;
    if (hwnd) {
        RECT rc{};
        if (GetClientRect(hwnd, &rc)) {
            client_width = static_cast<float>(rc.right - rc.left);
        }
    }
    const auto s = session_.LoadPaneState(client_width, PaneController::PANE_MIN_WIDTH, PaneController::PANE_DEFAULT_WIDTH);
    auto& panes = state_.view.panes;
    panes.SetSidePaneVisible(PaneTarget::File, s.show_file);
    panes.SetSidePaneVisible(PaneTarget::Toc, s.show_toc);
    panes.SetSidePaneWidth(PaneTarget::File, s.file_width);
    panes.SetSidePaneWidth(PaneTarget::Toc, s.toc_width);
}

void PersistenceController::SaveScrollPosition()
{
    const int node = state_.view.viewport.FindFirstVisibleNode(state_.document.layout_cache, state_.document.doc.GetNodes().size());
    if (node < 0) {
        return;
    }
    // 復元側 (NodeOffsetToScrollY) と同じ cache[node].text_top フィールドを読む。
    // Fenwick PrefixSum 経由 (TextTopOf) は float 加算順が違うためノード数が増えると誤差が累積する。
    const float text_top = state_.document.layout_cache[node].text_top;
    session_.SaveScrollPosition(node, state_.view.viewport.GetScrollY(), text_top);
}
