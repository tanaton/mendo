#include "app_controller.h"
#include "ui_constants.h"
#include <windows.h>

AppAction AppController::HandleKeyDown(const KeyDownEvent& event) const
{
    // Alt+矢印キー: 戻る/進むナビゲーション
    if (event.alt && !event.ctrl) {
        switch (event.key) {
        case VK_LEFT:  return NavigateBackAction{};
        case VK_RIGHT: return NavigateForwardAction{};
        }
        return NoOpAction{};
    }

    if (event.ctrl) {
        switch (event.key) {
        case 'C': return CopyClipboardAction{};
        case 'A': return SelectAllAction{};
        case 'O': return OpenFileAction{};
        case 'F': return OpenSearchBarAction{};
        case 'G':
            if (event.shift) {
                return SearchPrevAction{};
            }
            else {
                return SearchNextAction{};
            }
        case '1': return TogglePaneAction{ true };
        case '2': return TogglePaneAction{ false };
        case VK_OEM_PLUS:
        case VK_ADD:
            return ZoomAction{ 1 };
        case VK_OEM_MINUS:
        case VK_SUBTRACT:
            return ZoomAction{ -1 };
        case '0':
        case VK_NUMPAD0:
            return ZoomAction{ 0 };
        }
        return NoOpAction{};
    }

    switch (event.key) {
    case VK_UP:    return KeyScrollAction{ ScrollType::LineUp };
    case VK_DOWN:  return KeyScrollAction{ ScrollType::LineDown };
    case VK_PRIOR: return KeyScrollAction{ ScrollType::PageUp };
    case VK_NEXT:  return KeyScrollAction{ ScrollType::PageDown };
    case VK_HOME:  return KeyScrollAction{ ScrollType::Home };
    case VK_END:   return KeyScrollAction{ ScrollType::End };
    case VK_F1:    return ShowHelpAction{};
    case VK_F3:
        if (event.shift) {
            return SearchPrevAction{};
        }
        else {
            return SearchNextAction{};
        }
    case VK_F5:    return ReloadFileAction{};
    case VK_ESCAPE: return ClearSelectionAction{};
    }

    return NoOpAction{};
}

AppAction AppController::HandleMouseWheel(const MouseWheelEvent& event) const
{
    if (event.ctrl) {
        return ZoomAction{ event.delta > 0 ? 1 : -1 };
    }

    const float scroll_amount = -event.delta * MOUSE_WHEEL_SCROLL_MULTIPLIER;

    switch (event.zone) {
    case PaneZone::FilePane:
        return ScrollPaneAction{ PaneZone::FilePane, scroll_amount };
    case PaneZone::TocPane:
        return ScrollPaneAction{ PaneZone::TocPane, scroll_amount };
    default:
        return DirectScrollByAction{ scroll_amount };
    }
}
