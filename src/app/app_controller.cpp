#include "app_events.h"
#include "ui_constants.h"
#include <cstdlib>
#include <windows.h>

namespace app_controller {

AppAction HandleKeyDown(const KeyDownEvent& event)
{
    if (event.alt && !event.ctrl) {
        switch (event.key) {
        case VK_LEFT:
            return NavigateBackAction{};
        case VK_RIGHT:
            return NavigateForwardAction{};
        }
        return NoOpAction{};
    }

    if (event.ctrl) {
        switch (event.key) {
        case 'C':
            if (event.shift) {
                return CopyFormattedClipboardAction{};
            }
            return CopyClipboardAction{};
        case 'A':
            return SelectAllAction{};
        case 'O':
            return OpenFileAction{};
        case 'F':
            return OpenSearchBarAction{};
        case 'G':
            if (event.shift) {
                return SearchPrevAction{};
            }
            return SearchNextAction{};
        case '1':
            return TogglePaneAction{ PaneTarget::File };
        case '2':
            return TogglePaneAction{ PaneTarget::Toc };
        case VK_OEM_PLUS:
        case VK_ADD:
            return ZoomAction{ ZoomDirection::In };
        case VK_OEM_MINUS:
        case VK_SUBTRACT:
            return ZoomAction{ ZoomDirection::Out };
        case '0':
        case VK_NUMPAD0:
            return ZoomAction{ ZoomDirection::Reset };
        }
        return NoOpAction{};
    }

    switch (event.key) {
    case VK_UP:
        return KeyScrollAction{ ScrollType::LineUp };
    case VK_DOWN:
        return KeyScrollAction{ ScrollType::LineDown };
    case VK_PRIOR:
        return KeyScrollAction{ ScrollType::PageUp };
    case VK_NEXT:
        return KeyScrollAction{ ScrollType::PageDown };
    case VK_HOME:
        return KeyScrollAction{ ScrollType::Home };
    case VK_END:
        return KeyScrollAction{ ScrollType::End };
    case VK_F1:
        return ShowHelpAction{};
    case VK_F3:
        if (event.shift) {
            return SearchPrevAction{};
        }
        return SearchNextAction{};
    case VK_F5:
        return ReloadFileAction{};
    case VK_ESCAPE:
        return ClearSelectionAction{};
    }

    return NoOpAction{};
}

AppAction HandleMouseWheel(const MouseWheelEvent& event, int& zoom_accum)
{
    if (event.ctrl) {
        // 向きが反転したら逆向きの端数は持ち越さない。
        if ((zoom_accum < 0) != (event.delta < 0)) {
            zoom_accum = 0;
        }
        zoom_accum += event.delta;
        const int steps = std::abs(zoom_accum) / WHEEL_DELTA;
        if (steps == 0) {
            return NoOpAction{};
        }
        zoom_accum = 0;
        // ポインタの下の領域だけをズームする。
        return ZoomAction{ event.delta > 0 ? ZoomDirection::In : ZoomDirection::Out, steps, ToPaneTarget(event.zone) };
    }

    const float scroll_amount = -event.delta * MOUSE_WHEEL_SCROLL_MULTIPLIER;

    if (const auto pane = ToPaneTarget(event.zone)) {
        return ScrollPaneAction{ *pane, scroll_amount, event.tick };
    }
    return DirectScrollByAction{ scroll_amount, event.tick };
}

} // namespace app_controller
