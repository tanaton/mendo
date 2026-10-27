#include <gtest/gtest.h>
#include "app_events.h"
#include <windows.h>

namespace {

// 同じ alternative であることに加え、payload を持つものは中身まで比較する。
void ExpectSameAction(const AppAction& actual, const AppAction& expected)
{
    ASSERT_EQ(actual.index(), expected.index());
    if (const auto* scroll = std::get_if<KeyScrollAction>(&expected)) {
        EXPECT_EQ(std::get<KeyScrollAction>(actual).type, scroll->type);
    }
    if (const auto* toggle = std::get_if<TogglePaneAction>(&expected)) {
        EXPECT_EQ(std::get<TogglePaneAction>(actual).target, toggle->target);
    }
    if (const auto* zoom = std::get_if<ZoomAction>(&expected)) {
        EXPECT_EQ(std::get<ZoomAction>(actual).direction, zoom->direction);
        EXPECT_EQ(std::get<ZoomAction>(actual).steps, zoom->steps);
        EXPECT_EQ(std::get<ZoomAction>(actual).pane, zoom->pane);
    }
}

struct KeyCase {
    const char* name;
    KeyDownEvent ev;
    AppAction expected;
};

} // namespace

TEST(AppControllerTest, HandleKeyDown)
{
    const KeyCase kCases[] = {
        { "Up", { .key = VK_UP }, KeyScrollAction{ ScrollType::LineUp } },
        { "Down", { .key = VK_DOWN }, KeyScrollAction{ ScrollType::LineDown } },
        { "PageUp", { .key = VK_PRIOR }, KeyScrollAction{ ScrollType::PageUp } },
        { "PageDown", { .key = VK_NEXT }, KeyScrollAction{ ScrollType::PageDown } },
        { "Home", { .key = VK_HOME }, KeyScrollAction{ ScrollType::Home } },
        { "End", { .key = VK_END }, KeyScrollAction{ ScrollType::End } },

        { "F1", { .key = VK_F1 }, ShowHelpAction{} },
        { "F5", { .key = VK_F5 }, ReloadFileAction{} },
        { "Escape", { .key = VK_ESCAPE }, ClearSelectionAction{} },

        { "Ctrl+C", { .key = 'C', .ctrl = true }, CopyClipboardAction{} },
        { "Ctrl+Shift+C", { .key = 'C', .ctrl = true, .shift = true }, CopyFormattedClipboardAction{} },
        { "Ctrl+A", { .key = 'A', .ctrl = true }, SelectAllAction{} },
        { "Ctrl+O", { .key = 'O', .ctrl = true }, OpenFileAction{} },
        { "Ctrl+1", { .key = '1', .ctrl = true }, TogglePaneAction{ PaneTarget::File } },
        { "Ctrl+2", { .key = '2', .ctrl = true }, TogglePaneAction{ PaneTarget::Toc } },
        { "Ctrl+Plus", { .key = VK_OEM_PLUS, .ctrl = true }, ZoomAction{ ZoomDirection::In } },
        { "Ctrl+NumpadPlus", { .key = VK_ADD, .ctrl = true }, ZoomAction{ ZoomDirection::In } },
        { "Ctrl+Minus", { .key = VK_OEM_MINUS, .ctrl = true }, ZoomAction{ ZoomDirection::Out } },
        { "Ctrl+NumpadMinus", { .key = VK_SUBTRACT, .ctrl = true }, ZoomAction{ ZoomDirection::Out } },
        { "Ctrl+0", { .key = '0', .ctrl = true }, ZoomAction{ ZoomDirection::Reset } },
        { "Ctrl+Numpad0", { .key = VK_NUMPAD0, .ctrl = true }, ZoomAction{ ZoomDirection::Reset } },

        { "Z", { .key = 'Z' }, NoOpAction{} },
        { "C without Ctrl", { .key = 'C' }, NoOpAction{} },
        { "Ctrl+Z", { .key = 'Z', .ctrl = true }, NoOpAction{} },

        { "Alt+Left", { .key = VK_LEFT, .alt = true }, NavigateBackAction{} },
        { "Alt+Right", { .key = VK_RIGHT, .alt = true }, NavigateForwardAction{} },
        { "Alt+Up", { .key = VK_UP, .alt = true }, NoOpAction{} },
        // Ctrl+Alt ではナビゲーションを発動しない
        { "Ctrl+Alt+Left", { .key = VK_LEFT, .ctrl = true, .alt = true }, NoOpAction{} },

        { "F3", { .key = VK_F3 }, SearchNextAction{} },
        { "Shift+F3", { .key = VK_F3, .shift = true }, SearchPrevAction{} },
        { "Ctrl+F", { .key = 'F', .ctrl = true }, OpenSearchBarAction{} },
        { "Ctrl+G", { .key = 'G', .ctrl = true }, SearchNextAction{} },
        { "Ctrl+Shift+G", { .key = 'G', .ctrl = true, .shift = true }, SearchPrevAction{} },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        ExpectSameAction(app_controller::HandleKeyDown(c.ev), c.expected);
    }
}
