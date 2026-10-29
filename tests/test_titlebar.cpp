#include <gtest/gtest.h>
#include <algorithm>
#include <span>
#include <string>
#include <vector>
#include "titlebar.h"

namespace {

struct ButtonSpec {
    const char* name;
    TitleBarHitZone zone;
    float width;
};

// 左側ボタン群: Icon | OpenFile | Search | ThemeToggle | Settings | Help
constexpr ButtonSpec kLeftButtons[] = {
    { "OpenFile", TitleBarHitZone::OpenFile, TitleBar::BUTTON_WIDTH },
    { "Search", TitleBarHitZone::Search, TitleBar::BUTTON_WIDTH },
    { "ThemeToggle", TitleBarHitZone::ThemeToggle, TitleBar::BUTTON_WIDTH },
    { "Settings", TitleBarHitZone::Settings, TitleBar::BUTTON_WIDTH },
    { "Help", TitleBarHitZone::Help, TitleBar::BUTTON_WIDTH },
};

// 右側ボタン群: FileToggle | TocToggle | Minimize | Maximize | Close
constexpr ButtonSpec kRightButtons[] = {
    { "FileToggle", TitleBarHitZone::FileToggle, TitleBar::BUTTON_WIDTH },
    { "TocToggle", TitleBarHitZone::TocToggle, TitleBar::BUTTON_WIDTH },
    { "Minimize", TitleBarHitZone::Minimize, TitleBar::CAPTION_BTN_WIDTH },
    { "Maximize", TitleBarHitZone::Maximize, TitleBar::CAPTION_BTN_WIDTH },
    { "Close", TitleBarHitZone::Close, TitleBar::CAPTION_BTN_WIDTH },
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

// キャプションボタン（最小化/最大化/閉じる）だけ幅が広く、全ボタンがタイトルバーの全高を使う。
TEST_F(TitleBarTest, ButtonSizes)
{
    for (const auto group : kButtonGroups) {
        for (const auto& spec : group) {
            SCOPED_TRACE(spec.name);
            const DipRect& btn = tb_.GetButton(spec.zone);
            EXPECT_FLOAT_EQ(btn.top, 0.0f);
            EXPECT_FLOAT_EQ(btn.bottom, TitleBar::BASE_HEIGHT);
            EXPECT_FLOAT_EQ(btn.right - btn.left, spec.width);
        }
    }
}

TEST_F(TitleBarTest, TitleTextRectSpansBetweenButtonGroups)
{
    const auto& rect = tb_.GetTitleTextRect();
    EXPECT_FLOAT_EQ(rect.left, tb_.GetButton(TitleBarHitZone::Help).right);
    EXPECT_FLOAT_EQ(rect.right, tb_.GetButton(TitleBarHitZone::FileToggle).left);
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

TEST_F(TitleBarTest, HitTestCaptionArea)
{
    // タイトルテキスト領域の中央 — どのボタンにも属さない
    auto& rect = tb_.GetTitleTextRect();
    float cx = (rect.left + rect.right) / 2.0f;
    float cy = TitleBar::BASE_HEIGHT / 2.0f;
    EXPECT_EQ(tb_.HitTest(cx, cy), TitleBarHitZone::Caption);
}

// ボタンは左端を含み右端を含まない半開区間。右端ちょうどは右隣のボタンに当たる。
TEST_F(TitleBarTest, HitTestButtonEdgesAreHalfOpen)
{
    constexpr float cy = TitleBar::BASE_HEIGHT / 2.0f;
    for (const auto group : kButtonGroups) {
        for (const auto& spec : group) {
            SCOPED_TRACE(spec.name);
            const auto& btn = tb_.GetButton(spec.zone);
            EXPECT_EQ(tb_.HitTest(btn.left, cy), spec.zone);
            EXPECT_EQ(tb_.HitTest(btn.right - 0.01f, cy), spec.zone);
        }
    }
    EXPECT_EQ(tb_.HitTest(tb_.GetButton(TitleBarHitZone::Minimize).right, cy), TitleBarHitZone::Maximize);
}

// ═══════════════════════════════════════════════
// ボタンが重ならないことの検証
// ═══════════════════════════════════════════════

TEST_F(TitleBarTest, ButtonsDoNotOverlap)
{
    std::vector<DipRect> rects;
    for (const auto zone : { TitleBarHitZone::OpenFile, TitleBarHitZone::Settings, TitleBarHitZone::Help, TitleBarHitZone::ThemeToggle,
                             TitleBarHitZone::Search, TitleBarHitZone::FileToggle, TitleBarHitZone::TocToggle, TitleBarHitZone::Minimize,
                             TitleBarHitZone::Maximize, TitleBarHitZone::Close }) {
        rects.push_back(tb_.GetButton(zone));
    }
    ASSERT_EQ(rects.size(), kTitleBarButtonCount);
    std::ranges::sort(rects, {}, &DipRect::left);
    for (size_t i = 1; i < rects.size(); ++i) {
        EXPECT_LE(rects[i - 1].right, rects[i].left)
            << "Button " << i - 1 << " overlaps button " << i;
    }
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

TEST(BuildTitleString, PrefixesFilenameToAppName)
{
    struct Case {
        std::wstring_view path;
        std::wstring_view expected;
    };
    constexpr Case kCases[] = {
        { L"", L"mendo" },
        { L"C:\\dir\\test.md", L"test.md - mendo" },
        { L"readme.md", L"readme.md - mendo" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ c.path }));
        EXPECT_EQ(BuildTitleString(c.path, 0), c.expected);
    }
}
