#include "app.h"
#include "ascii_util.h"
#include "document_utils.h"
#include "resource.h"

namespace {

bool IsEditableTextFile(std::wstring_view path)
{
    return IsMarkdownFile(path) || ascii_util::iequal(ExtensionView(path), L".txt");
}

} // namespace

void App::OnContextMenu(int screen_x, int screen_y)
{
    POINT client_pt{ screen_x, screen_y };
    ScreenToClient(hwnd_, &client_pt);
    const auto dip = PixelToDip(client_pt.x, client_pt.y);

    const auto& selection = state_.view.viewport.GetSelection();
    const auto& panes = state_.view.panes;
    const ContextMenuParams params{
        .screen_x = screen_x,
        .screen_y = screen_y,
        .dpi_scale = state_.window.cached_dpi_scale,
        .can_go_back = state_.view.nav_history.CanGoBack(),
        .can_go_forward = state_.view.nav_history.CanGoForward(),
        .has_file = !state_.document.doc.GetFilePath().empty(),
        .has_selection = selection.active && selection.start_node >= 0,
        .dark_mode_checked = theme_service_.IsDarkMode(),
        .file_pane_checked = panes.IsSidePaneVisible(PaneTarget::File),
        .toc_pane_checked = panes.IsSidePaneVisible(PaneTarget::Toc),
        .show_file_items = PaneAtPoint(dip.x) == PaneZone::MdPane,
        .theme = &renderer_.GetTheme(),
    };

    switch (state_.ctx_menu.Show(hwnd_, params)) {
    case IDM_NAV_BACK:
        Dispatch(NavigateBackAction{});
        break;
    case IDM_NAV_FORWARD:
        Dispatch(NavigateForwardAction{});
        break;
    case IDM_EDIT_FILE: {
        const auto& file_path = state_.document.doc.GetFilePath();
        if (IsEditableTextFile(file_path)) {
            win32_host_.ShellOpen(file_path);
        }
        break;
    }
    case IDM_COPY:
        Dispatch(CopyClipboardAction{});
        break;
    case IDM_COPY_FORMATTED:
        Dispatch(CopyFormattedClipboardAction{});
        break;
    case IDM_TOGGLE_DARK_MODE:
        Dispatch(ToggleDarkModeAction{});
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
