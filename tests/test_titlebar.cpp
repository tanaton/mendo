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
    TitleBarHitZone zone;
};

// 左側ボタン群: Icon | OpenFile | Search | ThemeToggle | Help
constexpr ButtonSpec kLeftButtons[] = {
    { "OpenFile", TitleBarHitZone::OpenFile },
    { "Search", TitleBarHitZone::Search },
    { "ThemeToggle", TitleBarHitZone::ThemeToggle },
    { "Help", TitleBarHitZone::Help },
};

// 右側ボタン群: FileToggle | TocToggle | Minimize | Maximize | Close
constexpr ButtonSpec kRightButtons[] = {
    { "FileToggle", TitleBarHitZone::FileToggle },
    { "TocToggle", TitleBarHitZone::TocToggle },
    { "Minimize", TitleBarHitZone::Minimize },
    { "Maximize", TitleBarHitZone::Maximize },
    { "Close", TitleBarHitZone::Close },
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
    auto& btn = tb_.GetButton(TitleBarHitZone::Close);
    EXPECT_FLOAT_EQ(btn.right, WINDOW_WIDTH);
    EXPECT_FLOAT_EQ(btn.left, WINDOW_WIDTH - TitleBar::CAPTION_BTN_WIDTH);
}

TEST_F(TitleBarTest, OpenFileIsRightOfIcon)
{
    auto& open_file = tb_.GetButton(TitleBarHitZone::OpenFile);
    float expected_left = TitleBar::ICON_LEFT_MARGIN + TitleBar::ICON_SIZE + TitleBar::ICON_RIGHT_GAP;
    EXPECT_FLOAT_EQ(open_file.left, expected_left);
}

TEST_F(TitleBarTest, ButtonGroupsArePackedWithoutGaps)
{
    for (const auto group : kButtonGroups) {
        for (size_t i = 1; i < group.size(); ++i) {
            SCOPED_TRACE(std::string{ group[i - 1].name } + " | " + group[i].name);
            EXPECT_FLOAT_EQ(tb_.GetButton(group[i - 1].zone).right, tb_.GetButton(group[i].zone).left);
        }
    }
}

TEST_F(TitleBarTest, AllButtonsUseFullHeight)
{
    for (const auto group : kButtonGroups) {
        for (const auto& spec : group) {
            const DipRect& btn = tb_.GetButton(spec.zone);
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
    check(tb_.GetButton(TitleBarHitZone::Minimize));
    check(tb_.GetButton(TitleBarHitZone::Maximize));
    check(tb_.GetButton(TitleBarHitZone::Close));
}

TEST_F(TitleBarTest, PaneToggleButtonWidth)
{
    auto check = [](const DipRect& btn) static {
        EXPECT_FLOAT_EQ(btn.right - btn.left, TitleBar::BUTTON_WIDTH);
    };
    check(tb_.GetButton(TitleBarHitZone::OpenFile));
    check(tb_.GetButton(TitleBarHitZone::Help));
    check(tb_.GetButton(TitleBarHitZone::ThemeToggle));
    check(tb_.GetButton(TitleBarHitZone::Search));
    check(tb_.GetButton(TitleBarHitZone::FileToggle));
    check(tb_.GetButton(TitleBarHitZone::TocToggle));
}

TEST_F(TitleBarTest, TitleTextRectStartsAfterLeftButtons)
{
    auto& rect = tb_.GetTitleTextRect();
    auto& help = tb_.GetButton(TitleBarHitZone::Help);
    EXPECT_FLOAT_EQ(rect.left, help.right);
}

TEST_F(TitleBarTest, TitleTextRectEndsAtFileToggleButton)
{
    auto& rect = tb_.GetTitleTextRect();
    auto& file = tb_.GetButton(TitleBarHitZone::FileToggle);
    EXPECT_FLOAT_EQ(rect.right, file.left);
}

TEST_F(TitleBarTest, LayoutUpdatesOnWindowResize)
{
    tb_.UpdateLayout(1200.0f);
    auto& btn = tb_.GetButton(TitleBarHitZone::Close);
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
            const auto& rect = tb_.GetButton(spec.zone);
            const float cx = (rect.left + rect.right) / 2.0f;
            const float cy = (rect.top + rect.bottom) / 2.0f;
            EXPECT_EQ(tb_.HitTest(cx, cy), spec.zone);
        }
    }
}

TEST_F(TitleBarTest, HitTestOpenFileButtonBoundary)
{
    auto& btn = tb_.GetButton(TitleBarHitZone::OpenFile);
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
    auto& btn = tb_.GetButton(TitleBarHitZone::Close);
    EXPECT_EQ(tb_.HitTest(btn.left, TitleBar::BASE_HEIGHT / 2.0f), TitleBarHitZone::Close);
}

TEST_F(TitleBarTest, HitTestButtonBoundaryRight)
{
    // ボタンの右端ちょうどはボタン外（half-open interval）
    auto& btn = tb_.GetButton(TitleBarHitZone::Minimize);
    // right の直前のピクセルはMinimize内
    EXPECT_EQ(tb_.HitTest(btn.right - 0.01f, TitleBar::BASE_HEIGHT / 2.0f), TitleBarHitZone::Minimize);
    // right ちょうどはMaximize（右隣）
    EXPECT_EQ(tb_.HitTest(btn.right, TitleBar::BASE_HEIGHT / 2.0f), TitleBarHitZone::Maximize);
}

// ═══════════════════════════════════════════════
// ボタンが重ならないことの検証
// ═══════════════════════════════════════════════

TEST_F(TitleBarTest, ButtonsDoNotOverlap)
{
    // 全ボタンの矩形を収集して、左端でソートし隣接確認
    struct Rect { float left; float right; };
    Rect rects[] = {
        { tb_.GetButton(TitleBarHitZone::OpenFile).left,      tb_.GetButton(TitleBarHitZone::OpenFile).right },
        { tb_.GetButton(TitleBarHitZone::Help).left,          tb_.GetButton(TitleBarHitZone::Help).right },
        { tb_.GetButton(TitleBarHitZone::ThemeToggle).left,   tb_.GetButton(TitleBarHitZone::ThemeToggle).right },
        { tb_.GetButton(TitleBarHitZone::Search).left,        tb_.GetButton(TitleBarHitZone::Search).right },
        { tb_.GetButton(TitleBarHitZone::FileToggle).left,    tb_.GetButton(TitleBarHitZone::FileToggle).right },
        { tb_.GetButton(TitleBarHitZone::TocToggle).left,     tb_.GetButton(TitleBarHitZone::TocToggle).right },
        { tb_.GetButton(TitleBarHitZone::Minimize).left,      tb_.GetButton(TitleBarHitZone::Minimize).right },
        { tb_.GetButton(TitleBarHitZone::Maximize).left,      tb_.GetButton(TitleBarHitZone::Maximize).right },
        { tb_.GetButton(TitleBarHitZone::Close).left,         tb_.GetButton(TitleBarHitZone::Close).right },
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
    auto& help = tb_.GetButton(TitleBarHitZone::Help);
    auto& file = tb_.GetButton(TitleBarHitZone::FileToggle);
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
