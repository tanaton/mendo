#include <gtest/gtest.h>
#include "app_controller.h"
#include <windows.h>

class AppControllerTest : public ::testing::Test {
};

// ─── ヘルパー: 期待する型のアクションを取得 ───

template <typename T>
const T& GetAction(const AppAction& action)
{
    return std::get<T>(action);
}

// ═══════════════════════════════════════════════
// HandleKeyDown — ナビゲーションキー
// ═══════════════════════════════════════════════

TEST_F(AppControllerTest, ArrowUpReturnsLineUp)
{
    auto a = app_controller::HandleKeyDown({ VK_UP, false, false });
    auto& s = GetAction<KeyScrollAction>(a);
    EXPECT_EQ(s.type, ScrollType::LineUp);
}

TEST_F(AppControllerTest, ArrowDownReturnsLineDown)
{
    auto a = app_controller::HandleKeyDown({ VK_DOWN, false, false });
    auto& s = GetAction<KeyScrollAction>(a);
    EXPECT_EQ(s.type, ScrollType::LineDown);
}

TEST_F(AppControllerTest, PageUpReturnsPageUp)
{
    auto a = app_controller::HandleKeyDown({ VK_PRIOR, false, false });
    auto& s = GetAction<KeyScrollAction>(a);
    EXPECT_EQ(s.type, ScrollType::PageUp);
}

TEST_F(AppControllerTest, PageDownReturnsPageDown)
{
    auto a = app_controller::HandleKeyDown({ VK_NEXT, false, false });
    auto& s = GetAction<KeyScrollAction>(a);
    EXPECT_EQ(s.type, ScrollType::PageDown);
}

TEST_F(AppControllerTest, HomeReturnsHome)
{
    auto a = app_controller::HandleKeyDown({ VK_HOME, false, false });
    auto& s = GetAction<KeyScrollAction>(a);
    EXPECT_EQ(s.type, ScrollType::Home);
}

TEST_F(AppControllerTest, EndReturnsEnd)
{
    auto a = app_controller::HandleKeyDown({ VK_END, false, false });
    auto& s = GetAction<KeyScrollAction>(a);
    EXPECT_EQ(s.type, ScrollType::End);
}

// ═══════════════════════════════════════════════
// HandleKeyDown — ファンクション／特殊キー
// ═══════════════════════════════════════════════

TEST_F(AppControllerTest, F1ReturnsShowHelp)
{
    auto a = app_controller::HandleKeyDown({ VK_F1, false, false });
    EXPECT_TRUE(std::holds_alternative<ShowHelpAction>(a));
}

TEST_F(AppControllerTest, F5ReturnsReload)
{
    auto a = app_controller::HandleKeyDown({ VK_F5, false, false });
    EXPECT_TRUE(std::holds_alternative<ReloadFileAction>(a));
}

TEST_F(AppControllerTest, EscapeReturnsClearSelection)
{
    auto a = app_controller::HandleKeyDown({ VK_ESCAPE, false, false });
    EXPECT_TRUE(std::holds_alternative<ClearSelectionAction>(a));
}

// ═══════════════════════════════════════════════
// HandleKeyDown — Ctrlショートカット
// ═══════════════════════════════════════════════

TEST_F(AppControllerTest, CtrlCReturnsCopy)
{
    auto a = app_controller::HandleKeyDown({ 'C', true, false });
    EXPECT_TRUE(std::holds_alternative<CopyClipboardAction>(a));
}

TEST_F(AppControllerTest, CtrlShiftCReturnsCopyFormatted)
{
    auto a = app_controller::HandleKeyDown({ 'C', true, true });
    EXPECT_TRUE(std::holds_alternative<CopyFormattedClipboardAction>(a));
}

TEST_F(AppControllerTest, CtrlAReturnsSelectAll)
{
    auto a = app_controller::HandleKeyDown({ 'A', true, false });
    EXPECT_TRUE(std::holds_alternative<SelectAllAction>(a));
}

TEST_F(AppControllerTest, CtrlOReturnsOpenFile)
{
    auto a = app_controller::HandleKeyDown({ 'O', true, false });
    EXPECT_TRUE(std::holds_alternative<OpenFileAction>(a));
}

TEST_F(AppControllerTest, Ctrl1ReturnsToggleFilePane)
{
    auto a = app_controller::HandleKeyDown({ '1', true, false });
    auto& t = GetAction<TogglePaneAction>(a);
    EXPECT_EQ(t.target, PaneTarget::File);
}

TEST_F(AppControllerTest, Ctrl2ReturnsToggleTocPane)
{
    auto a = app_controller::HandleKeyDown({ '2', true, false });
    auto& t = GetAction<TogglePaneAction>(a);
    EXPECT_EQ(t.target, PaneTarget::Toc);
}

TEST_F(AppControllerTest, CtrlPlusReturnsZoomIn)
{
    auto a = app_controller::HandleKeyDown({ VK_OEM_PLUS, true, false });
    auto& z = GetAction<ZoomAction>(a);
    EXPECT_EQ(z.direction, ZoomDirection::In);
}

TEST_F(AppControllerTest, CtrlNumpadPlusReturnsZoomIn)
{
    auto a = app_controller::HandleKeyDown({ VK_ADD, true, false });
    auto& z = GetAction<ZoomAction>(a);
    EXPECT_EQ(z.direction, ZoomDirection::In);
}

TEST_F(AppControllerTest, CtrlMinusReturnsZoomOut)
{
    auto a = app_controller::HandleKeyDown({ VK_OEM_MINUS, true, false });
    auto& z = GetAction<ZoomAction>(a);
    EXPECT_EQ(z.direction, ZoomDirection::Out);
}

TEST_F(AppControllerTest, CtrlNumpadMinusReturnsZoomOut)
{
    auto a = app_controller::HandleKeyDown({ VK_SUBTRACT, true, false });
    auto& z = GetAction<ZoomAction>(a);
    EXPECT_EQ(z.direction, ZoomDirection::Out);
}

TEST_F(AppControllerTest, Ctrl0ReturnsZoomReset)
{
    auto a = app_controller::HandleKeyDown({ '0', true, false });
    auto& z = GetAction<ZoomAction>(a);
    EXPECT_EQ(z.direction, ZoomDirection::Reset);
}

TEST_F(AppControllerTest, CtrlNumpad0ReturnsZoomReset)
{
    auto a = app_controller::HandleKeyDown({ VK_NUMPAD0, true, false });
    auto& z = GetAction<ZoomAction>(a);
    EXPECT_EQ(z.direction, ZoomDirection::Reset);
}

// ═══════════════════════════════════════════════
// HandleKeyDown — アクションなしのケース
// ═══════════════════════════════════════════════

TEST_F(AppControllerTest, UnknownKeyProducesNoAction)
{
    auto a = app_controller::HandleKeyDown({ 'Z', false, false });
    EXPECT_TRUE(std::holds_alternative<NoOpAction>(a));
}

TEST_F(AppControllerTest, CWithoutCtrlProducesNoAction)
{
    auto a = app_controller::HandleKeyDown({ 'C', false, false });
    EXPECT_TRUE(std::holds_alternative<NoOpAction>(a));
}

TEST_F(AppControllerTest, UnknownCtrlKeyProducesNoAction)
{
    auto a = app_controller::HandleKeyDown({ 'Z', true, false });
    EXPECT_TRUE(std::holds_alternative<NoOpAction>(a));
}

// ═══════════════════════════════════════════════
// HandleMouseWheel — MDペインのスクロール
// ═══════════════════════════════════════════════

TEST_F(AppControllerTest, WheelUpInMdPaneDirectScrolls)
{
    auto a = app_controller::HandleMouseWheel({ 120, false, PaneZone::MdPane });
    auto& s = GetAction<DirectScrollByAction>(a);
    EXPECT_FLOAT_EQ(s.delta, -120.0f * 0.8f);
}

TEST_F(AppControllerTest, WheelDownInMdPaneDirectScrolls)
{
    auto a = app_controller::HandleMouseWheel({ -120, false, PaneZone::MdPane });
    auto& s = GetAction<DirectScrollByAction>(a);
    EXPECT_FLOAT_EQ(s.delta, 120.0f * 0.8f);
}

// ═══════════════════════════════════════════════
// HandleMouseWheel — ペインスクロール
// ═══════════════════════════════════════════════

TEST_F(AppControllerTest, WheelInFilePaneScrollsFilePane)
{
    auto a = app_controller::HandleMouseWheel({ 120, false, PaneZone::FilePane });
    auto& s = GetAction<ScrollPaneAction>(a);
    EXPECT_EQ(s.pane, PaneZone::FilePane);
    EXPECT_FLOAT_EQ(s.delta, -120.0f * 0.8f);
}

TEST_F(AppControllerTest, WheelInTocPaneScrollsTocPane)
{
    auto a = app_controller::HandleMouseWheel({ -120, false, PaneZone::TocPane });
    auto& s = GetAction<ScrollPaneAction>(a);
    EXPECT_EQ(s.pane, PaneZone::TocPane);
    EXPECT_FLOAT_EQ(s.delta, 120.0f * 0.8f);
}

TEST_F(AppControllerTest, WheelOnSplitterScrollsMdPane)
{
    auto a = app_controller::HandleMouseWheel({ 120, false, PaneZone::Splitter1 });
    EXPECT_TRUE(std::holds_alternative<DirectScrollByAction>(a));
}

// ═══════════════════════════════════════════════
// HandleMouseWheel — Ctrl+ホイールによるズーム
// ═══════════════════════════════════════════════

TEST_F(AppControllerTest, CtrlWheelUpZoomsIn)
{
    auto a = app_controller::HandleMouseWheel({ 120, true, PaneZone::MdPane });
    auto& z = GetAction<ZoomAction>(a);
    EXPECT_EQ(z.direction, ZoomDirection::In);
}

TEST_F(AppControllerTest, CtrlWheelDownZoomsOut)
{
    auto a = app_controller::HandleMouseWheel({ -120, true, PaneZone::MdPane });
    auto& z = GetAction<ZoomAction>(a);
    EXPECT_EQ(z.direction, ZoomDirection::Out);
}

TEST_F(AppControllerTest, CtrlWheelIgnoresZone)
{
    // Ctrl+ホイールはどのペインでもズームする
    auto a = app_controller::HandleMouseWheel({ 120, true, PaneZone::FilePane });
    EXPECT_TRUE(std::holds_alternative<ZoomAction>(a));
}

// ═══════════════════════════════════════════════
// HandleKeyDown — Alt+矢印キーによるナビゲーション
// ═══════════════════════════════════════════════

TEST_F(AppControllerTest, AltLeftReturnsNavigateBack)
{
    auto a = app_controller::HandleKeyDown({ VK_LEFT, false, false, true });
    EXPECT_TRUE(std::holds_alternative<NavigateBackAction>(a));
}

TEST_F(AppControllerTest, AltRightReturnsNavigateForward)
{
    auto a = app_controller::HandleKeyDown({ VK_RIGHT, false, false, true });
    EXPECT_TRUE(std::holds_alternative<NavigateForwardAction>(a));
}

TEST_F(AppControllerTest, AltUpProducesNoAction)
{
    auto a = app_controller::HandleKeyDown({ VK_UP, false, false, true });
    EXPECT_TRUE(std::holds_alternative<NoOpAction>(a));
}

TEST_F(AppControllerTest, CtrlAltLeftProducesNoAction)
{
    // Ctrl+Altではナビゲーションが発動しないこと
    auto a = app_controller::HandleKeyDown({ VK_LEFT, true, false, true });
    EXPECT_TRUE(std::holds_alternative<NoOpAction>(a));
}

// ═══════════════════════════════════════════════
// HandleKeyDown — 検索ショートカット
// ═══════════════════════════════════════════════

TEST_F(AppControllerTest, F3ReturnsSearchNext)
{
    auto a = app_controller::HandleKeyDown({ VK_F3, false, false });
    EXPECT_TRUE(std::holds_alternative<SearchNextAction>(a));
}

TEST_F(AppControllerTest, ShiftF3ReturnsSearchPrev)
{
    auto a = app_controller::HandleKeyDown({ VK_F3, false, true });
    EXPECT_TRUE(std::holds_alternative<SearchPrevAction>(a));
}

TEST_F(AppControllerTest, CtrlFReturnsOpenSearchBar)
{
    auto a = app_controller::HandleKeyDown({ 'F', true, false });
    EXPECT_TRUE(std::holds_alternative<OpenSearchBarAction>(a));
}

TEST_F(AppControllerTest, CtrlGReturnsSearchNext)
{
    auto a = app_controller::HandleKeyDown({ 'G', true, false });
    EXPECT_TRUE(std::holds_alternative<SearchNextAction>(a));
}

TEST_F(AppControllerTest, CtrlShiftGReturnsSearchPrev)
{
    auto a = app_controller::HandleKeyDown({ 'G', true, true });
    EXPECT_TRUE(std::holds_alternative<SearchPrevAction>(a));
}
