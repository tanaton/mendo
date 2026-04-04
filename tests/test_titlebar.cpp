#include <gtest/gtest.h>
#include <algorithm>
#include <iterator>
#include "titlebar.h"

class TitleBarTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        tb_.UpdateLayout(WINDOW_WIDTH);
    }
    static constexpr float WINDOW_WIDTH = 800.0f;
    TitleBar tb_;
};

// ═══════════════════════════════════════════════
// 基本定数
// ═══════════════════════════════════════════════

TEST_F(TitleBarTest, HeightIsBaseHeight)
{
    EXPECT_FLOAT_EQ(tb_.GetHeight(), TitleBar::BASE_HEIGHT);
}

// ═══════════════════════════════════════════════
// UpdateLayout — ボタン配置
// ═══════════════════════════════════════════════

TEST_F(TitleBarTest, CloseButtonIsAtRightEdge)
{
    auto& btn = tb_.GetCloseButton();
    EXPECT_FLOAT_EQ(btn.rect.right, WINDOW_WIDTH);
    EXPECT_FLOAT_EQ(btn.rect.left, WINDOW_WIDTH - TitleBar::CAPTION_BTN_WIDTH);
}

TEST_F(TitleBarTest, MaximizeButtonIsLeftOfClose)
{
    auto& close = tb_.GetCloseButton();
    auto& maximize = tb_.GetMaximizeButton();
    EXPECT_FLOAT_EQ(maximize.rect.right, close.rect.left);
}

TEST_F(TitleBarTest, MinimizeButtonIsLeftOfMaximize)
{
    auto& maximize = tb_.GetMaximizeButton();
    auto& minimize = tb_.GetMinimizeButton();
    EXPECT_FLOAT_EQ(minimize.rect.right, maximize.rect.left);
}

TEST_F(TitleBarTest, HelpButtonIsLeftOfMinimize)
{
    auto& minimize = tb_.GetMinimizeButton();
    auto& help = tb_.GetHelpButton();
    EXPECT_FLOAT_EQ(help.rect.right, minimize.rect.left);
}

TEST_F(TitleBarTest, ThemeToggleIsLeftOfHelp)
{
    auto& help = tb_.GetHelpButton();
    auto& theme = tb_.GetThemeToggleButton();
    EXPECT_FLOAT_EQ(theme.rect.right, help.rect.left);
}

TEST_F(TitleBarTest, SearchIsLeftOfThemeToggle)
{
    auto& theme = tb_.GetThemeToggleButton();
    auto& search = tb_.GetSearchButton();
    EXPECT_FLOAT_EQ(search.rect.right, theme.rect.left);
}

TEST_F(TitleBarTest, TocToggleIsLeftOfSearch)
{
    auto& search = tb_.GetSearchButton();
    auto& toc = tb_.GetTocToggleButton();
    EXPECT_FLOAT_EQ(toc.rect.right, search.rect.left);
}

TEST_F(TitleBarTest, FileToggleIsLeftOfTocToggle)
{
    auto& toc = tb_.GetTocToggleButton();
    auto& file = tb_.GetFileToggleButton();
    EXPECT_FLOAT_EQ(file.rect.right, toc.rect.left);
}

TEST_F(TitleBarTest, AllButtonsUseFullHeight)
{
    auto check = [](const TitleBarButton& btn) static  {
        EXPECT_FLOAT_EQ(btn.rect.top, 0.0f);
        EXPECT_FLOAT_EQ(btn.rect.bottom, TitleBar::BASE_HEIGHT);
    };
    check(tb_.GetHelpButton());
    check(tb_.GetThemeToggleButton());
    check(tb_.GetSearchButton());
    check(tb_.GetFileToggleButton());
    check(tb_.GetTocToggleButton());
    check(tb_.GetMinimizeButton());
    check(tb_.GetMaximizeButton());
    check(tb_.GetCloseButton());
}

TEST_F(TitleBarTest, CaptionButtonWidth)
{
    auto check = [](const TitleBarButton& btn) static {
        EXPECT_FLOAT_EQ(btn.rect.right - btn.rect.left, TitleBar::CAPTION_BTN_WIDTH);
    };
    check(tb_.GetMinimizeButton());
    check(tb_.GetMaximizeButton());
    check(tb_.GetCloseButton());
}

TEST_F(TitleBarTest, PaneToggleButtonWidth)
{
    auto check = [](const TitleBarButton& btn) static {
        EXPECT_FLOAT_EQ(btn.rect.right - btn.rect.left, TitleBar::BUTTON_WIDTH);
    };
    check(tb_.GetHelpButton());
    check(tb_.GetThemeToggleButton());
    check(tb_.GetSearchButton());
    check(tb_.GetFileToggleButton());
    check(tb_.GetTocToggleButton());
}

TEST_F(TitleBarTest, TitleTextRectStartsAfterIcon)
{
    auto& rect = tb_.GetTitleTextRect();
    float expected = TitleBar::ICON_LEFT_MARGIN + TitleBar::ICON_SIZE + TitleBar::ICON_RIGHT_GAP;
    EXPECT_FLOAT_EQ(rect.left, expected);
}

TEST_F(TitleBarTest, TitleTextRectEndsAtFileToggleButton)
{
    auto& rect = tb_.GetTitleTextRect();
    auto& file = tb_.GetFileToggleButton();
    EXPECT_FLOAT_EQ(rect.right, file.rect.left);
}

TEST_F(TitleBarTest, LayoutUpdatesOnWindowResize)
{
    tb_.UpdateLayout(1200.0f);
    auto& btn = tb_.GetCloseButton();
    EXPECT_FLOAT_EQ(btn.rect.right, 1200.0f);
}

// ═══════════════════════════════════════════════
// HitTest
// ═══════════════════════════════════════════════

TEST_F(TitleBarTest, HitTestOutsideTitleBar)
{
    EXPECT_EQ(tb_.HitTest(400.0f, -1.0f), TitleBarHitZone::None);
    EXPECT_EQ(tb_.HitTest(400.0f, TitleBar::BASE_HEIGHT), TitleBarHitZone::None);
    EXPECT_EQ(tb_.HitTest(400.0f, 100.0f), TitleBarHitZone::None);
}

TEST_F(TitleBarTest, HitTestCloseButton)
{
    auto& btn = tb_.GetCloseButton();
    float cx = (btn.rect.left + btn.rect.right) / 2.0f;
    float cy = (btn.rect.top + btn.rect.bottom) / 2.0f;
    EXPECT_EQ(tb_.HitTest(cx, cy), TitleBarHitZone::Close);
}

TEST_F(TitleBarTest, HitTestMaximizeButton)
{
    auto& btn = tb_.GetMaximizeButton();
    float cx = (btn.rect.left + btn.rect.right) / 2.0f;
    float cy = (btn.rect.top + btn.rect.bottom) / 2.0f;
    EXPECT_EQ(tb_.HitTest(cx, cy), TitleBarHitZone::Maximize);
}

TEST_F(TitleBarTest, HitTestMinimizeButton)
{
    auto& btn = tb_.GetMinimizeButton();
    float cx = (btn.rect.left + btn.rect.right) / 2.0f;
    float cy = (btn.rect.top + btn.rect.bottom) / 2.0f;
    EXPECT_EQ(tb_.HitTest(cx, cy), TitleBarHitZone::Minimize);
}

TEST_F(TitleBarTest, HitTestFileToggle)
{
    auto& btn = tb_.GetFileToggleButton();
    float cx = (btn.rect.left + btn.rect.right) / 2.0f;
    float cy = (btn.rect.top + btn.rect.bottom) / 2.0f;
    EXPECT_EQ(tb_.HitTest(cx, cy), TitleBarHitZone::FileToggle);
}

TEST_F(TitleBarTest, HitTestTocToggle)
{
    auto& btn = tb_.GetTocToggleButton();
    float cx = (btn.rect.left + btn.rect.right) / 2.0f;
    float cy = (btn.rect.top + btn.rect.bottom) / 2.0f;
    EXPECT_EQ(tb_.HitTest(cx, cy), TitleBarHitZone::TocToggle);
}

TEST_F(TitleBarTest, HitTestThemeToggle)
{
    auto& btn = tb_.GetThemeToggleButton();
    float cx = (btn.rect.left + btn.rect.right) / 2.0f;
    float cy = (btn.rect.top + btn.rect.bottom) / 2.0f;
    EXPECT_EQ(tb_.HitTest(cx, cy), TitleBarHitZone::ThemeToggle);
}

TEST_F(TitleBarTest, HitTestSearchButton)
{
    auto& btn = tb_.GetSearchButton();
    float cx = (btn.rect.left + btn.rect.right) / 2.0f;
    float cy = (btn.rect.top + btn.rect.bottom) / 2.0f;
    EXPECT_EQ(tb_.HitTest(cx, cy), TitleBarHitZone::Search);
}

TEST_F(TitleBarTest, HitTestHelpButton)
{
    auto& btn = tb_.GetHelpButton();
    float cx = (btn.rect.left + btn.rect.right) / 2.0f;
    float cy = (btn.rect.top + btn.rect.bottom) / 2.0f;
    EXPECT_EQ(tb_.HitTest(cx, cy), TitleBarHitZone::Help);
}

TEST_F(TitleBarTest, HitTestCaptionArea)
{
    // タイトルテキスト領域の中央 — どのボタンにも属さない
    auto& rect = tb_.GetTitleTextRect();
    float cx = (rect.left + rect.right) / 2.0f;
    float cy = TitleBar::BASE_HEIGHT / 2.0f;
    EXPECT_EQ(tb_.HitTest(cx, cy), TitleBarHitZone::Caption);
}

TEST_F(TitleBarTest, HitTestButtonBoundaryLeft)
{
    // ボタンの左端ちょうどはボタン内
    auto& btn = tb_.GetCloseButton();
    EXPECT_EQ(tb_.HitTest(btn.rect.left, TitleBar::BASE_HEIGHT / 2.0f), TitleBarHitZone::Close);
}

TEST_F(TitleBarTest, HitTestButtonBoundaryRight)
{
    // ボタンの右端ちょうどはボタン外（half-open interval）
    auto& btn = tb_.GetMinimizeButton();
    // right の直前のピクセルはMinimize内
    EXPECT_EQ(tb_.HitTest(btn.rect.right - 0.01f, TitleBar::BASE_HEIGHT / 2.0f), TitleBarHitZone::Minimize);
    // right ちょうどはMaximize（右隣）
    EXPECT_EQ(tb_.HitTest(btn.rect.right, TitleBar::BASE_HEIGHT / 2.0f), TitleBarHitZone::Maximize);
}

// ═══════════════════════════════════════════════
// SetHovered
// ═══════════════════════════════════════════════

TEST_F(TitleBarTest, InitialHoverIsNone)
{
    EXPECT_EQ(tb_.GetHovered(), TitleBarHitZone::None);
    EXPECT_FALSE(tb_.GetFileToggleButton().hovered);
    EXPECT_FALSE(tb_.GetCloseButton().hovered);
}

TEST_F(TitleBarTest, SetHoveredReturnsTrueOnChange)
{
    EXPECT_TRUE(tb_.SetHovered(TitleBarHitZone::Close));
    EXPECT_EQ(tb_.GetHovered(), TitleBarHitZone::Close);
    EXPECT_TRUE(tb_.GetCloseButton().hovered);
}

TEST_F(TitleBarTest, SetHoveredReturnsFalseOnSame)
{
    tb_.SetHovered(TitleBarHitZone::Close);
    EXPECT_FALSE(tb_.SetHovered(TitleBarHitZone::Close));
}

TEST_F(TitleBarTest, SetHoveredClearsPrevious)
{
    tb_.SetHovered(TitleBarHitZone::Close);
    tb_.SetHovered(TitleBarHitZone::Minimize);
    EXPECT_FALSE(tb_.GetCloseButton().hovered);
    EXPECT_TRUE(tb_.GetMinimizeButton().hovered);
    EXPECT_EQ(tb_.GetHovered(), TitleBarHitZone::Minimize);
}

TEST_F(TitleBarTest, SetHoveredNoneClearsAll)
{
    tb_.SetHovered(TitleBarHitZone::FileToggle);
    tb_.SetHovered(TitleBarHitZone::None);
    EXPECT_FALSE(tb_.GetHelpButton().hovered);
    EXPECT_FALSE(tb_.GetThemeToggleButton().hovered);
    EXPECT_FALSE(tb_.GetSearchButton().hovered);
    EXPECT_FALSE(tb_.GetFileToggleButton().hovered);
    EXPECT_FALSE(tb_.GetTocToggleButton().hovered);
    EXPECT_FALSE(tb_.GetMinimizeButton().hovered);
    EXPECT_FALSE(tb_.GetMaximizeButton().hovered);
    EXPECT_FALSE(tb_.GetCloseButton().hovered);
}

TEST_F(TitleBarTest, SetHoveredSetsExactlyOneButton)
{
    auto countHovered = [&]() {
        int count = 0;
        if (tb_.GetHelpButton().hovered) { ++count; }
        if (tb_.GetThemeToggleButton().hovered) { ++count; }
        if (tb_.GetSearchButton().hovered) { ++count; }
        if (tb_.GetFileToggleButton().hovered) { ++count; }
        if (tb_.GetTocToggleButton().hovered) { ++count; }
        if (tb_.GetMinimizeButton().hovered) { ++count; }
        if (tb_.GetMaximizeButton().hovered) { ++count; }
        if (tb_.GetCloseButton().hovered) { ++count; }
        return count;
    };

    TitleBarHitZone zones[] = {
        TitleBarHitZone::Help, TitleBarHitZone::ThemeToggle,
        TitleBarHitZone::Search,
        TitleBarHitZone::FileToggle, TitleBarHitZone::TocToggle,
        TitleBarHitZone::Minimize, TitleBarHitZone::Maximize,
        TitleBarHitZone::Close,
    };
    for (auto zone : zones) {
        tb_.SetHovered(zone);
        EXPECT_EQ(countHovered(), 1) << "zone=" << static_cast<int>(zone);
    }
}

// ═══════════════════════════════════════════════
// ボタンが重ならないことの検証
// ═══════════════════════════════════════════════

TEST_F(TitleBarTest, ButtonsDoNotOverlap)
{
    // 全ボタンの矩形を収集して、左端でソートし隣接確認
    struct Rect { float left; float right; };
    Rect rects[] = {
        { tb_.GetHelpButton().rect.left,          tb_.GetHelpButton().rect.right },
        { tb_.GetThemeToggleButton().rect.left,   tb_.GetThemeToggleButton().rect.right },
        { tb_.GetSearchButton().rect.left,        tb_.GetSearchButton().rect.right },
        { tb_.GetFileToggleButton().rect.left,    tb_.GetFileToggleButton().rect.right },
        { tb_.GetTocToggleButton().rect.left,     tb_.GetTocToggleButton().rect.right },
        { tb_.GetMinimizeButton().rect.left,      tb_.GetMinimizeButton().rect.right },
        { tb_.GetMaximizeButton().rect.left,      tb_.GetMaximizeButton().rect.right },
        { tb_.GetCloseButton().rect.left,         tb_.GetCloseButton().rect.right },
    };
    // leftでソート
    std::sort(std::begin(rects), std::end(rects),
        [](const Rect& a, const Rect& b) static noexcept { return a.left < b.left; });

    for (size_t i = 1; i < std::size(rects); ++i) {
        EXPECT_LE(rects[i - 1].right, rects[i].left)
            << "Button " << i - 1 << " overlaps button " << i;
    }
}

TEST_F(TitleBarTest, TitleTextDoesNotOverlapButtons)
{
    auto& title = tb_.GetTitleTextRect();
    auto& file = tb_.GetFileToggleButton().rect;
    EXPECT_LE(title.right, file.left);
    EXPECT_GE(title.left, 0.0f);
}

// ═══════════════════════════════════════════════
// 狭いウィンドウでのレイアウト安全性
// ═══════════════════════════════════════════════

TEST_F(TitleBarTest, NarrowWindowTitleTextDoesNotGoNegativeWidth)
{
    // 全ボタンが収まらないほど狭いウィンドウ
    tb_.UpdateLayout(100.0f);
    auto& rect = tb_.GetTitleTextRect();
    // タイトルテキスト幅は0以上であるべき
    EXPECT_GE(rect.right, rect.left);
}
