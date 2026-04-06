#include "app_controller.h"
#include "ui_constants.h"
#include <windows.h>

ActionList AppController::HandleKeyDown(const KeyDownEvent& event) const
{
    ActionList actions;

    // Alt+矢印キー: 戻る/進むナビゲーション
    if (event.alt && !event.ctrl) {
        switch (event.key) {
        case VK_LEFT:  actions.emplace_back(NavigateBackAction{}); break;
        case VK_RIGHT: actions.emplace_back(NavigateForwardAction{}); break;
        }
        return actions;
    }

    if (event.ctrl) {
        switch (event.key) {
        case 'C': actions.emplace_back(CopyClipboardAction{}); break;
        case 'A': actions.emplace_back(SelectAllAction{}); break;
        case 'O': actions.emplace_back(OpenFileAction{}); break;
        case 'F': actions.emplace_back(OpenSearchBarAction{}); break;
        case 'G':
            if (event.shift) {
                actions.emplace_back(SearchPrevAction{});
            }
            else {
                actions.emplace_back(SearchNextAction{});
            }
            break;
        case '1': actions.emplace_back(TogglePaneAction{ true }); break;
        case '2': actions.emplace_back(TogglePaneAction{ false }); break;
        case VK_OEM_PLUS:
        case VK_ADD:
            actions.emplace_back(ZoomAction{ 1 }); break;
        case VK_OEM_MINUS:
        case VK_SUBTRACT:
            actions.emplace_back(ZoomAction{ -1 }); break;
        case '0':
        case VK_NUMPAD0:
            actions.emplace_back(ZoomAction{ 0 }); break;
        }
        return actions;
    }

    switch (event.key) {
    case VK_UP:    actions.emplace_back(KeyScrollAction{ ScrollType::LineUp }); break;
    case VK_DOWN:  actions.emplace_back(KeyScrollAction{ ScrollType::LineDown }); break;
    case VK_PRIOR: actions.emplace_back(KeyScrollAction{ ScrollType::PageUp }); break;
    case VK_NEXT:  actions.emplace_back(KeyScrollAction{ ScrollType::PageDown }); break;
    case VK_HOME:  actions.emplace_back(KeyScrollAction{ ScrollType::Home }); break;
    case VK_END:   actions.emplace_back(KeyScrollAction{ ScrollType::End }); break;
    case VK_F1:    actions.emplace_back(ShowHelpAction{}); break;
    case VK_F3:
        if (event.shift) {
            actions.emplace_back(SearchPrevAction{});
        }
        else {
            actions.emplace_back(SearchNextAction{});
        }
        break;
    case VK_F5:    actions.emplace_back(ReloadFileAction{}); break;
    case VK_ESCAPE: actions.emplace_back(ClearSelectionAction{}); break;
    }

    return actions;
}

ActionList AppController::HandleMouseWheel(const MouseWheelEvent& event) const
{
    ActionList actions;

    if (event.ctrl) {
        actions.emplace_back(ZoomAction{ event.delta > 0 ? 1 : -1 });
        return actions;
    }

    const float scroll_amount = -event.delta * MOUSE_WHEEL_SCROLL_MULTIPLIER;

    switch (event.zone) {
    case PaneZone::FilePane:
        actions.emplace_back(ScrollPaneAction{ PaneZone::FilePane, scroll_amount });
        break;
    case PaneZone::TocPane:
        actions.emplace_back(ScrollPaneAction{ PaneZone::TocPane, scroll_amount });
        break;
    default:
        actions.emplace_back(DirectScrollByAction{ scroll_amount });
        break;
    }

    return actions;
}
