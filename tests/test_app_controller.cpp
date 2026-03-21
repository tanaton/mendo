#include <gtest/gtest.h>
#include "app_controller.h"
#include <windows.h>

class AppControllerTest : public ::testing::Test {
protected:
    AppController ctrl_;
};

// ─── Helper: extract single action of expected type ───

template <typename T>
const T& GetSingleAction(const ActionList& actions) {
    EXPECT_EQ(actions.size(), 1u);
    return std::get<T>(actions.at(0));
}

// ═══════════════════════════════════════════════
// HandleKeyDown — navigation keys
// ═══════════════════════════════════════════════

TEST_F(AppControllerTest, ArrowUpReturnsLineUp) {
    auto a = ctrl_.HandleKeyDown({VK_UP, false, false});
    auto& s = GetSingleAction<KeyScrollAction>(a);
    EXPECT_EQ(s.type, ScrollType::LineUp);
}

TEST_F(AppControllerTest, ArrowDownReturnsLineDown) {
    auto a = ctrl_.HandleKeyDown({VK_DOWN, false, false});
    auto& s = GetSingleAction<KeyScrollAction>(a);
    EXPECT_EQ(s.type, ScrollType::LineDown);
}

TEST_F(AppControllerTest, PageUpReturnsPageUp) {
    auto a = ctrl_.HandleKeyDown({VK_PRIOR, false, false});
    auto& s = GetSingleAction<KeyScrollAction>(a);
    EXPECT_EQ(s.type, ScrollType::PageUp);
}

TEST_F(AppControllerTest, PageDownReturnsPageDown) {
    auto a = ctrl_.HandleKeyDown({VK_NEXT, false, false});
    auto& s = GetSingleAction<KeyScrollAction>(a);
    EXPECT_EQ(s.type, ScrollType::PageDown);
}

TEST_F(AppControllerTest, HomeReturnsHome) {
    auto a = ctrl_.HandleKeyDown({VK_HOME, false, false});
    auto& s = GetSingleAction<KeyScrollAction>(a);
    EXPECT_EQ(s.type, ScrollType::Home);
}

TEST_F(AppControllerTest, EndReturnsEnd) {
    auto a = ctrl_.HandleKeyDown({VK_END, false, false});
    auto& s = GetSingleAction<KeyScrollAction>(a);
    EXPECT_EQ(s.type, ScrollType::End);
}

// ═══════════════════════════════════════════════
// HandleKeyDown — function / special keys
// ═══════════════════════════════════════════════

TEST_F(AppControllerTest, F5ReturnsReload) {
    auto a = ctrl_.HandleKeyDown({VK_F5, false, false});
    ASSERT_EQ(a.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<ReloadFileAction>(a[0]));
}

TEST_F(AppControllerTest, EscapeReturnsClearSelection) {
    auto a = ctrl_.HandleKeyDown({VK_ESCAPE, false, false});
    ASSERT_EQ(a.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<ClearSelectionAction>(a[0]));
}

// ═══════════════════════════════════════════════
// HandleKeyDown — Ctrl shortcuts
// ═══════════════════════════════════════════════

TEST_F(AppControllerTest, CtrlCReturnsCopy) {
    auto a = ctrl_.HandleKeyDown({'C', true, false});
    ASSERT_EQ(a.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<CopyClipboardAction>(a[0]));
}

TEST_F(AppControllerTest, CtrlAReturnsSelectAll) {
    auto a = ctrl_.HandleKeyDown({'A', true, false});
    ASSERT_EQ(a.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<SelectAllAction>(a[0]));
}

TEST_F(AppControllerTest, CtrlOReturnsOpenFile) {
    auto a = ctrl_.HandleKeyDown({'O', true, false});
    ASSERT_EQ(a.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OpenFileAction>(a[0]));
}

TEST_F(AppControllerTest, Ctrl1ReturnsToggleFilePane) {
    auto a = ctrl_.HandleKeyDown({'1', true, false});
    auto& t = GetSingleAction<TogglePaneAction>(a);
    EXPECT_TRUE(t.file_pane);
}

TEST_F(AppControllerTest, Ctrl2ReturnsToggleTocPane) {
    auto a = ctrl_.HandleKeyDown({'2', true, false});
    auto& t = GetSingleAction<TogglePaneAction>(a);
    EXPECT_FALSE(t.file_pane);
}

TEST_F(AppControllerTest, CtrlPlusReturnsZoomIn) {
    auto a = ctrl_.HandleKeyDown({VK_OEM_PLUS, true, false});
    auto& z = GetSingleAction<ZoomAction>(a);
    EXPECT_EQ(z.direction, 1);
}

TEST_F(AppControllerTest, CtrlNumpadPlusReturnsZoomIn) {
    auto a = ctrl_.HandleKeyDown({VK_ADD, true, false});
    auto& z = GetSingleAction<ZoomAction>(a);
    EXPECT_EQ(z.direction, 1);
}

TEST_F(AppControllerTest, CtrlMinusReturnsZoomOut) {
    auto a = ctrl_.HandleKeyDown({VK_OEM_MINUS, true, false});
    auto& z = GetSingleAction<ZoomAction>(a);
    EXPECT_EQ(z.direction, -1);
}

TEST_F(AppControllerTest, CtrlNumpadMinusReturnsZoomOut) {
    auto a = ctrl_.HandleKeyDown({VK_SUBTRACT, true, false});
    auto& z = GetSingleAction<ZoomAction>(a);
    EXPECT_EQ(z.direction, -1);
}

TEST_F(AppControllerTest, Ctrl0ReturnsZoomReset) {
    auto a = ctrl_.HandleKeyDown({'0', true, false});
    auto& z = GetSingleAction<ZoomAction>(a);
    EXPECT_EQ(z.direction, 0);
}

TEST_F(AppControllerTest, CtrlNumpad0ReturnsZoomReset) {
    auto a = ctrl_.HandleKeyDown({VK_NUMPAD0, true, false});
    auto& z = GetSingleAction<ZoomAction>(a);
    EXPECT_EQ(z.direction, 0);
}

// ═══════════════════════════════════════════════
// HandleKeyDown — no action cases
// ═══════════════════════════════════════════════

TEST_F(AppControllerTest, UnknownKeyProducesNoAction) {
    auto a = ctrl_.HandleKeyDown({'Z', false, false});
    EXPECT_TRUE(a.empty());
}

TEST_F(AppControllerTest, CWithoutCtrlProducesNoAction) {
    auto a = ctrl_.HandleKeyDown({'C', false, false});
    EXPECT_TRUE(a.empty());
}

TEST_F(AppControllerTest, UnknownCtrlKeyProducesNoAction) {
    auto a = ctrl_.HandleKeyDown({'Z', true, false});
    EXPECT_TRUE(a.empty());
}

// ═══════════════════════════════════════════════
// HandleMouseWheel — MD pane scroll
// ═══════════════════════════════════════════════

TEST_F(AppControllerTest, WheelUpInMdPaneSmoothScrolls) {
    auto a = ctrl_.HandleMouseWheel({120, false, PaneZone::MdPane});
    auto& s = GetSingleAction<SmoothScrollByAction>(a);
    EXPECT_FLOAT_EQ(s.delta, -120.0f * 0.8f);
}

TEST_F(AppControllerTest, WheelDownInMdPaneSmoothScrolls) {
    auto a = ctrl_.HandleMouseWheel({-120, false, PaneZone::MdPane});
    auto& s = GetSingleAction<SmoothScrollByAction>(a);
    EXPECT_FLOAT_EQ(s.delta, 120.0f * 0.8f);
}

// ═══════════════════════════════════════════════
// HandleMouseWheel — pane scroll
// ═══════════════════════════════════════════════

TEST_F(AppControllerTest, WheelInFilePaneScrollsFilePane) {
    auto a = ctrl_.HandleMouseWheel({120, false, PaneZone::FilePane});
    auto& s = GetSingleAction<ScrollPaneAction>(a);
    EXPECT_EQ(s.pane, PaneZone::FilePane);
    EXPECT_FLOAT_EQ(s.delta, -120.0f * 0.8f);
}

TEST_F(AppControllerTest, WheelInTocPaneScrollsTocPane) {
    auto a = ctrl_.HandleMouseWheel({-120, false, PaneZone::TocPane});
    auto& s = GetSingleAction<ScrollPaneAction>(a);
    EXPECT_EQ(s.pane, PaneZone::TocPane);
    EXPECT_FLOAT_EQ(s.delta, 120.0f * 0.8f);
}

TEST_F(AppControllerTest, WheelOnSplitterScrollsMdPane) {
    auto a = ctrl_.HandleMouseWheel({120, false, PaneZone::Splitter1});
    ASSERT_EQ(a.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<SmoothScrollByAction>(a[0]));
}

// ═══════════════════════════════════════════════
// HandleMouseWheel — Ctrl+wheel zoom
// ═══════════════════════════════════════════════

TEST_F(AppControllerTest, CtrlWheelUpZoomsIn) {
    auto a = ctrl_.HandleMouseWheel({120, true, PaneZone::MdPane});
    auto& z = GetSingleAction<ZoomAction>(a);
    EXPECT_EQ(z.direction, 1);
}

TEST_F(AppControllerTest, CtrlWheelDownZoomsOut) {
    auto a = ctrl_.HandleMouseWheel({-120, true, PaneZone::MdPane});
    auto& z = GetSingleAction<ZoomAction>(a);
    EXPECT_EQ(z.direction, -1);
}

TEST_F(AppControllerTest, CtrlWheelIgnoresZone) {
    // Ctrl+wheel zooms regardless of which pane
    auto a = ctrl_.HandleMouseWheel({120, true, PaneZone::FilePane});
    ASSERT_EQ(a.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<ZoomAction>(a[0]));
}

// ═══════════════════════════════════════════════
// HandleKeyDown — Alt+Arrow navigation
// ═══════════════════════════════════════════════

TEST_F(AppControllerTest, AltLeftReturnsNavigateBack) {
    auto a = ctrl_.HandleKeyDown({VK_LEFT, false, false, true});
    ASSERT_EQ(a.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<NavigateBackAction>(a[0]));
}

TEST_F(AppControllerTest, AltRightReturnsNavigateForward) {
    auto a = ctrl_.HandleKeyDown({VK_RIGHT, false, false, true});
    ASSERT_EQ(a.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<NavigateForwardAction>(a[0]));
}

TEST_F(AppControllerTest, AltUpProducesNoAction) {
    auto a = ctrl_.HandleKeyDown({VK_UP, false, false, true});
    EXPECT_TRUE(a.empty());
}

TEST_F(AppControllerTest, CtrlAltLeftProducesNoAction) {
    // Ctrl+Alt should not trigger navigation
    auto a = ctrl_.HandleKeyDown({VK_LEFT, true, false, true});
    EXPECT_TRUE(a.empty());
}
