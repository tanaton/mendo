#include "app.h"
#include "resource.h"
#include "document_utils.h"
#include <shellapi.h>
#include <cwctype>
#include <filesystem>

namespace {

bool IsEditableTextFile(std::wstring_view path)
{
    if (IsMarkdownFile(path)) {
        return true;
    }
    auto ext = std::filesystem::path(path).extension().wstring();
    for (auto& c : ext) {
        c = std::towlower(c);
    }
    return ext == L".txt";
}

} // namespace

void App::OnContextMenu(int screen_x, int screen_y)
{
    POINT client_pt{ screen_x, screen_y };
    ScreenToClient(hwnd_, &client_pt);
    const auto dip = PixelToDip(client_pt.x, client_pt.y);
    const auto zone = PaneAtPoint(dip.x, dip.y);

    ContextMenuParams params;
    params.screen_x = screen_x;
    params.screen_y = screen_y;
    params.dpi_scale = cached_dpi_scale_;
    params.can_go_back = state_.nav_history.CanGoBack();
    params.can_go_forward = state_.nav_history.CanGoForward();
    params.has_file = !state_.doc.GetFilePath().empty();
    params.has_selection = state_.viewport.GetSelection().active && state_.viewport.GetSelection().start_node >= 0;
    params.dark_mode_checked = theme_service_.IsDarkMode();
    params.file_pane_checked = state_.panes.IsFilePaneVisible();
    params.toc_pane_checked = state_.panes.IsTocPaneVisible();
    params.show_file_items = (zone == PaneZone::MdPane);
    params.theme = &renderer_.GetTheme();

    const int cmd = state_.ctx_menu.Show(hwnd_, params);

    switch (cmd) {
    case IDM_NAV_BACK:
        NavigateBack();
        break;
    case IDM_NAV_FORWARD:
        NavigateForward();
        break;
    case IDM_EDIT_FILE: {
        const auto& file_path = state_.doc.GetFilePath();
        if (IsEditableTextFile(file_path)) {
            ShellExecuteW(hwnd_, L"open", file_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        break;
    }
    case IDM_COPY:
        CopySelectionToClipboard();
        break;
    case IDM_TOGGLE_DARK_MODE:
        ToggleDarkMode();
        break;
    case IDM_TOGGLE_FILE_PANE:
        Dispatch(TogglePaneAction{ PaneTarget::File });
        break;
    case IDM_TOGGLE_TOC_PANE:
        Dispatch(TogglePaneAction{ PaneTarget::Toc });
        break;
    default:
        break;
    }
}
