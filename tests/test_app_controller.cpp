#include <gtest/gtest.h>
#include "app_controller.h"
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
    }
    if (const auto* direct = std::get_if<DirectScrollByAction>(&expected)) {
        EXPECT_FLOAT_EQ(std::get<DirectScrollByAction>(actual).delta, direct->delta);
    }
    if (const auto* pane = std::get_if<ScrollPaneAction>(&expected)) {
        const auto& a = std::get<ScrollPaneAction>(actual);
        EXPECT_EQ(a.pane, pane->pane);
        EXPECT_FLOAT_EQ(a.delta, pane->delta);
    }
}

struct KeyCase {
    const char* name;
    KeyDownEvent ev;
    AppAction expected;
};

struct WheelCase {
    const char* name;
    MouseWheelEvent ev;
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

TEST(AppControllerTest, HandleMouseWheel)
{
    const WheelCase kCases[] = {
        { "Up in MdPane", { .delta = 120 }, DirectScrollByAction{ -120.0f * 0.8f } },
        { "Down in MdPane", { .delta = -120 }, DirectScrollByAction{ 120.0f * 0.8f } },
        { "FilePane", { .delta = 120, .zone = PaneZone::FilePane }, ScrollPaneAction{ PaneZone::FilePane, -120.0f * 0.8f } },
        { "TocPane", { .delta = -120, .zone = PaneZone::TocPane }, ScrollPaneAction{ PaneZone::TocPane, 120.0f * 0.8f } },
        { "Splitter scrolls MdPane", { .delta = 120, .zone = PaneZone::Splitter1 }, DirectScrollByAction{ -120.0f * 0.8f } },
        { "Ctrl+Up", { .delta = 120, .ctrl = true }, ZoomAction{ ZoomDirection::In } },
        { "Ctrl+Down", { .delta = -120, .ctrl = true }, ZoomAction{ ZoomDirection::Out } },
        // Ctrl+ホイールはペインに関係なくズーム
        { "Ctrl+Up in FilePane", { .delta = 120, .ctrl = true, .zone = PaneZone::FilePane }, ZoomAction{ ZoomDirection::In } },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        ExpectSameAction(app_controller::HandleMouseWheel(c.ev), c.expected);
    }
}
