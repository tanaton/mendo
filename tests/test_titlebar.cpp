#include <gtest/gtest.h>
#include <algorithm>
#include <iterator>
#include <span>
#include <string>
#include <utility>
#include "titlebar.h"

namespace {

struct ButtonSpec {
    const char* name;
    const DipRect& (TitleBar::*get)() const noexcept;
    TitleBarHitZone zone;
};

// 左側ボタン群: Icon | OpenFile | Search | ThemeToggle | Help
constexpr ButtonSpec kLeftButtons[] = {
    { "OpenFile", &TitleBar::GetOpenFileButton, TitleBarHitZone::OpenFile },
    { "Search", &TitleBar::GetSearchButton, TitleBarHitZone::Search },
    { "ThemeToggle", &TitleBar::GetThemeToggleButton, TitleBarHitZone::ThemeToggle },
    { "Help", &TitleBar::GetHelpButton, TitleBarHitZone::Help },
};

// 右側ボタン群: FileToggle | TocToggle | Minimize | Maximize | Close
constexpr ButtonSpec kRightButtons[] = {
    { "FileToggle", &TitleBar::GetFileToggleButton, TitleBarHitZone::FileToggle },
    { "TocToggle", &TitleBar::GetTocToggleButton, TitleBarHitZone::TocToggle },
    { "Minimize", &TitleBar::GetMinimizeButton, TitleBarHitZone::Minimize },
    { "Maximize", &TitleBar::GetMaximizeButton, TitleBarHitZone::Maximize },
    { "Close", &TitleBar::GetCloseButton, TitleBarHitZone::Close },
};

constexpr std::span<const ButtonSpec> kButtonGroups[] = { kLeftButtons, kRightButtons };

} // namespace

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
    EXPECT_FLOAT_EQ(btn.right, WINDOW_WIDTH);
    EXPECT_FLOAT_EQ(btn.left, WINDOW_WIDTH - TitleBar::CAPTION_BTN_WIDTH);
}

TEST_F(TitleBarTest, OpenFileIsRightOfIcon)
{
    auto& open_file = tb_.GetOpenFileButton();
    float expected_left = TitleBar::ICON_LEFT_MARGIN + TitleBar::ICON_SIZE + TitleBar::ICON_RIGHT_GAP;
    EXPECT_FLOAT_EQ(open_file.left, expected_left);
}

TEST_F(TitleBarTest, ButtonGroupsArePackedWithoutGaps)
{
    for (const auto group : kButtonGroups) {
        for (size_t i = 1; i < group.size(); ++i) {
            SCOPED_TRACE(std::string{ group[i - 1].name } + " | " + group[i].name);
            EXPECT_FLOAT_EQ((tb_.*group[i - 1].get)().right, (tb_.*group[i].get)().left);
        }
    }
}

TEST_F(TitleBarTest, AllButtonsUseFullHeight)
{
    for (const auto group : kButtonGroups) {
        for (const auto& spec : group) {
            const DipRect& btn = (tb_.*spec.get)();
            EXPECT_FLOAT_EQ(btn.top, 0.0f) << spec.name;
            EXPECT_FLOAT_EQ(btn.bottom, TitleBar::BASE_HEIGHT) << spec.name;
        }
    }
}

TEST_F(TitleBarTest, CaptionButtonWidth)
{
    auto check = [](const DipRect& btn) static {
        EXPECT_FLOAT_EQ(btn.right - btn.left, TitleBar::CAPTION_BTN_WIDTH);
    };
    check(tb_.GetMinimizeButton());
    check(tb_.GetMaximizeButton());
    check(tb_.GetCloseButton());
}

TEST_F(TitleBarTest, PaneToggleButtonWidth)
{
    auto check = [](const DipRect& btn) static {
        EXPECT_FLOAT_EQ(btn.right - btn.left, TitleBar::BUTTON_WIDTH);
    };
    check(tb_.GetOpenFileButton());
    check(tb_.GetHelpButton());
    check(tb_.GetThemeToggleButton());
    check(tb_.GetSearchButton());
    check(tb_.GetFileToggleButton());
    check(tb_.GetTocToggleButton());
}

TEST_F(TitleBarTest, TitleTextRectStartsAfterLeftButtons)
{
    auto& rect = tb_.GetTitleTextRect();
    auto& help = tb_.GetHelpButton();
    EXPECT_FLOAT_EQ(rect.left, help.right);
}

TEST_F(TitleBarTest, TitleTextRectEndsAtFileToggleButton)
{
    auto& rect = tb_.GetTitleTextRect();
    auto& file = tb_.GetFileToggleButton();
    EXPECT_FLOAT_EQ(rect.right, file.left);
}

TEST_F(TitleBarTest, LayoutUpdatesOnWindowResize)
{
    tb_.UpdateLayout(1200.0f);
    auto& btn = tb_.GetCloseButton();
    EXPECT_FLOAT_EQ(btn.right, 1200.0f);
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

TEST_F(TitleBarTest, HitTestAtButtonCenters)
{
    for (const auto group : kButtonGroups) {
        for (const auto& spec : group) {
            SCOPED_TRACE(spec.name);
            const auto& rect = (tb_.*spec.get)();
            const float cx = (rect.left + rect.right) / 2.0f;
            const float cy = (rect.top + rect.bottom) / 2.0f;
            EXPECT_EQ(tb_.HitTest(cx, cy), spec.zone);
        }
    }
}

TEST_F(TitleBarTest, HitTestOpenFileButtonBoundary)
{
    auto& btn = tb_.GetOpenFileButton();
    float cy = (btn.top + btn.bottom) / 2.0f;
    // 左端ちょうどはボタン内
    EXPECT_EQ(tb_.HitTest(btn.left, cy), TitleBarHitZone::OpenFile);
    // 右端の直前はボタン内
    EXPECT_EQ(tb_.HitTest(btn.right - 0.01f, cy), TitleBarHitZone::OpenFile);
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
    EXPECT_EQ(tb_.HitTest(btn.left, TitleBar::BASE_HEIGHT / 2.0f), TitleBarHitZone::Close);
}

TEST_F(TitleBarTest, HitTestButtonBoundaryRight)
{
    // ボタンの右端ちょうどはボタン外（half-open interval）
    auto& btn = tb_.GetMinimizeButton();
    // right の直前のピクセルはMinimize内
    EXPECT_EQ(tb_.HitTest(btn.right - 0.01f, TitleBar::BASE_HEIGHT / 2.0f), TitleBarHitZone::Minimize);
    // right ちょうどはMaximize（右隣）
    EXPECT_EQ(tb_.HitTest(btn.right, TitleBar::BASE_HEIGHT / 2.0f), TitleBarHitZone::Maximize);
}

// ═══════════════════════════════════════════════
// SetHovered
// ═══════════════════════════════════════════════

TEST_F(TitleBarTest, InitialHoverIsNone)
{
    EXPECT_EQ(tb_.GetHovered(), TitleBarHitZone::None);
}

TEST_F(TitleBarTest, SetHoveredReturnsTrueOnChange)
{
    EXPECT_TRUE(tb_.SetHovered(TitleBarHitZone::Close));
    EXPECT_EQ(tb_.GetHovered(), TitleBarHitZone::Close);
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
    EXPECT_EQ(tb_.GetHovered(), TitleBarHitZone::Minimize);
}

TEST_F(TitleBarTest, SetHoveredNoneClearsAll)
{
    tb_.SetHovered(TitleBarHitZone::FileToggle);
    tb_.SetHovered(TitleBarHitZone::None);
    EXPECT_EQ(tb_.GetHovered(), TitleBarHitZone::None);
}

// ═══════════════════════════════════════════════
// ボタンが重ならないことの検証
// ═══════════════════════════════════════════════

TEST_F(TitleBarTest, ButtonsDoNotOverlap)
{
    // 全ボタンの矩形を収集して、左端でソートし隣接確認
    struct Rect { float left; float right; };
    Rect rects[] = {
        { tb_.GetOpenFileButton().left,      tb_.GetOpenFileButton().right },
        { tb_.GetHelpButton().left,          tb_.GetHelpButton().right },
        { tb_.GetThemeToggleButton().left,   tb_.GetThemeToggleButton().right },
        { tb_.GetSearchButton().left,        tb_.GetSearchButton().right },
        { tb_.GetFileToggleButton().left,    tb_.GetFileToggleButton().right },
        { tb_.GetTocToggleButton().left,     tb_.GetTocToggleButton().right },
        { tb_.GetMinimizeButton().left,      tb_.GetMinimizeButton().right },
        { tb_.GetMaximizeButton().left,      tb_.GetMaximizeButton().right },
        { tb_.GetCloseButton().left,         tb_.GetCloseButton().right },
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
    auto& help = tb_.GetHelpButton();
    auto& file = tb_.GetFileToggleButton();
    EXPECT_GE(title.left, help.right);
    EXPECT_LE(title.right, file.left);
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
