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
    params.can_go_back = nav_history_.CanGoBack();
    params.can_go_forward = nav_history_.CanGoForward();
    params.has_file = !doc_.GetFilePath().empty();
    params.has_selection = viewport_.GetSelection().active && viewport_.GetSelection().start_node >= 0;
    params.dark_mode_checked = theme_service_.IsDarkMode();
    params.file_pane_checked = panes_.IsFilePaneVisible();
    params.toc_pane_checked = panes_.IsTocPaneVisible();
    params.show_file_items = (zone == PaneZone::MdPane);
    params.theme = &renderer_.GetTheme();

    const int cmd = ctx_menu_.Show(hwnd_, params);

    if (cmd == IDM_NAV_BACK) {
        NavigateBack();
    }
    else if (cmd == IDM_NAV_FORWARD) {
        NavigateForward();
    }
    else if (cmd == IDM_EDIT_FILE) {
        const auto& file_path = doc_.GetFilePath();
        if (IsEditableTextFile(file_path)) {
            ShellExecuteW(hwnd_, L"open", file_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    else if (cmd == IDM_COPY) {
        CopySelectionToClipboard();
    }
    else if (cmd == IDM_TOGGLE_DARK_MODE) {
        ToggleDarkMode();
    }
    else if (cmd == IDM_TOGGLE_FILE_PANE) {
        panes_.ToggleFilePane();
        RefreshPaneLayout();
    }
    else if (cmd == IDM_TOGGLE_TOC_PANE) {
        panes_.ToggleTocPane();
        RefreshPaneLayout();
    }
}
