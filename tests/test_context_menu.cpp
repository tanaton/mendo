#include <gtest/gtest.h>
#include "context_menu.h"
#include "resource.h"
#include "theme.h"
#include <d2d1.h>
#include <dwrite.h>
#include <iterator>
#include <span>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

using ItemType = ContextMenu::ItemType;

struct ExpectedItem {
    ItemType type;
    int id;
};

constexpr ExpectedItem kMdPaneItems[] = {
    { ItemType::NavRow, 0 },
    { ItemType::Separator, 0 },
    { ItemType::Text, IDM_EDIT_FILE },
    { ItemType::Text, IDM_COPY },
    { ItemType::Text, IDM_COPY_FORMATTED },
    { ItemType::Separator, 0 },
    { ItemType::Text, IDM_TOGGLE_DARK_MODE },
    { ItemType::Separator, 0 },
    { ItemType::Text, IDM_TOGGLE_FILE_PANE },
    { ItemType::Text, IDM_TOGGLE_TOC_PANE },
};

constexpr ExpectedItem kNonMdPaneItems[] = {
    { ItemType::NavRow, 0 },
    { ItemType::Separator, 0 },
    { ItemType::Text, IDM_TOGGLE_DARK_MODE },
    { ItemType::Separator, 0 },
    { ItemType::Text, IDM_TOGGLE_FILE_PANE },
    { ItemType::Text, IDM_TOGGLE_TOC_PANE },
};

struct Point {
    float x;
    float y;
};

constexpr Point Center(const DipRect& r)
{
    return { (r.left + r.right) / 2.0f, (r.top + r.bottom) / 2.0f };
}

} // namespace

// ============================================================
// BuildItems テスト（DWrite不要）
// ============================================================

class ContextMenuTest : public ::testing::Test {
protected:
    ContextMenu menu_;
    Theme theme_ = GetLightTheme();

    void Build(const ContextMenuParams& params)
    {
        menu_.TestBuildItems(params);
    }

    ContextMenuParams MakeParams(bool show_file = true) const
    {
        ContextMenuParams p;
        p.theme = &theme_;
        p.dpi_scale = 1.0f;
        p.can_go_back = true;
        p.can_go_forward = true;
        p.has_file = true;
        p.has_selection = true;
        p.show_file_items = show_file;
        return p;
    }

    const ContextMenu::Item* FindItem(int id) const
    {
        for (const auto& item : menu_.GetItems()) {
            if (item.id == id) {
                return &item;
            }
        }
        return nullptr;
    }
};

// ─── 項目構築 ───

// MdPane 以外ではファイル操作 (Edit / Copy / CopyFormatted) と直後の区切りが消える。
TEST_F(ContextMenuTest, ItemSequenceDependsOnPane)
{
    struct Case {
        const char* name;
        bool show_file;
        std::span<const ExpectedItem> expected;
    };
    constexpr Case kCases[] = {
        { "MdPane", true, kMdPaneItems },
        { "NonMdPane", false, kNonMdPaneItems },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        Build(MakeParams(c.show_file));
        const auto& items = menu_.GetItems();
        ASSERT_EQ(items.size(), c.expected.size());
        for (size_t i = 0; i < items.size(); ++i) {
            SCOPED_TRACE(i);
            EXPECT_EQ(items[i].type, c.expected[i].type);
            EXPECT_EQ(items[i].id, c.expected[i].id);
        }
    }
}

// ─── 項目の有効/無効・チェック状態 ───

TEST_F(ContextMenuTest, ItemFlagsFollowParams)
{
    struct Case {
        const char* name;
        void (*tweak)(ContextMenuParams&);
        int id;
        bool ContextMenu::Item::*flag;
        bool expected;
    };
    using Item = ContextMenu::Item;
    constexpr Case kCases[] = {
        { "EditFileDisabledWhenNoFile", [](ContextMenuParams& p) { p.has_file = false; }, IDM_EDIT_FILE, &Item::enabled, false },
        { "CopyDisabledWhenNoSelection", [](ContextMenuParams& p) { p.has_selection = false; }, IDM_COPY, &Item::enabled, false },
        { "CopyFormattedDisabledWhenNoSelection", [](ContextMenuParams& p) { p.has_selection = false; }, IDM_COPY_FORMATTED, &Item::enabled, false },
        { "DarkModeChecked", [](ContextMenuParams& p) { p.dark_mode_checked = true; }, IDM_TOGGLE_DARK_MODE, &Item::checked, true },
        { "DarkModeUnchecked", nullptr, IDM_TOGGLE_DARK_MODE, &Item::checked, false },
        { "DarkModeAlwaysEnabled", nullptr, IDM_TOGGLE_DARK_MODE, &Item::enabled, true },
        { "FilePaneChecked", [](ContextMenuParams& p) { p.file_pane_checked = true; }, IDM_TOGGLE_FILE_PANE, &Item::checked, true },
        { "FilePaneUnchecked", [](ContextMenuParams& p) { p.file_pane_checked = false; }, IDM_TOGGLE_FILE_PANE, &Item::checked, false },
        { "TocPaneChecked", [](ContextMenuParams& p) { p.toc_pane_checked = true; }, IDM_TOGGLE_TOC_PANE, &Item::checked, true },
        { "TocPaneUnchecked", [](ContextMenuParams& p) { p.toc_pane_checked = false; }, IDM_TOGGLE_TOC_PANE, &Item::checked, false },
        { "FilePaneAlwaysEnabled", nullptr, IDM_TOGGLE_FILE_PANE, &Item::enabled, true },
        { "TocPaneAlwaysEnabled", nullptr, IDM_TOGGLE_TOC_PANE, &Item::enabled, true },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        auto p = MakeParams(true);
        if (c.tweak) {
            c.tweak(p);
        }
        Build(p);
        const auto* item = FindItem(c.id);
        ASSERT_NE(item, nullptr);
        EXPECT_EQ(item->*c.flag, c.expected);
    }
}

TEST_F(ContextMenuTest, NavEnabledFollowsParams)
{
    for (const bool back : { true, false }) {
        for (const bool fwd : { true, false }) {
            SCOPED_TRACE(::testing::Message() << "back=" << back << " fwd=" << fwd);
            auto p = MakeParams(true);
            p.can_go_back = back;
            p.can_go_forward = fwd;
            Build(p);
            EXPECT_EQ(menu_.GetNavLayout().back_enabled, back);
            EXPECT_EQ(menu_.GetNavLayout().fwd_enabled, fwd);
        }
    }
}

// ============================================================
// レイアウト・ヒットテスト（DWrite必要）
// ============================================================

class ContextMenuLayoutTest : public ContextMenuTest {
protected:
    static ComPtr<IDWriteFactory> dwrite_;
    static ComPtr<ID2D1Factory> d2d_;

    static void SetUpTestSuite()
    {
        HRESULT hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
        ASSERT_TRUE(SUCCEEDED(hr)) << "DWriteCreateFactory failed";
        ASSERT_NE(dwrite_.Get(), nullptr);
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf());
        ASSERT_TRUE(SUCCEEDED(hr)) << "D2D1CreateFactory failed";
        ASSERT_NE(d2d_.Get(), nullptr);
    }

    static void TearDownTestSuite()
    {
        dwrite_.Reset();
        d2d_.Reset();
    }

    void SetUp() override
    {
        menu_.Init(d2d_.Get(), dwrite_.Get());
    }

    void BuildAndLayout(const ContextMenuParams& params)
    {
        menu_.TestBuildItems(params);
        menu_.TestCreateTextFormats(*params.theme);
        menu_.TestComputeLayout();
    }
};

ComPtr<IDWriteFactory> ContextMenuLayoutTest::dwrite_;
ComPtr<ID2D1Factory> ContextMenuLayoutTest::d2d_;

// ─── メニューサイズ ───

TEST_F(ContextMenuLayoutTest, MenuWidthIsPositive)
{
    BuildAndLayout(MakeParams());
    EXPECT_GT(menu_.GetMenuWidth(), 0.0f);
}

TEST_F(ContextMenuLayoutTest, MenuHeightIsPositive)
{
    BuildAndLayout(MakeParams());
    EXPECT_GT(menu_.GetMenuHeight(), 0.0f);
}

TEST_F(ContextMenuLayoutTest, MenuWidthAtLeast160)
{
    BuildAndLayout(MakeParams());
    EXPECT_GE(menu_.GetMenuWidth(), 160.0f);
}

TEST_F(ContextMenuLayoutTest, NonMdPaneMenuIsShorter)
{
    BuildAndLayout(MakeParams(true));
    float full_h = menu_.GetMenuHeight();

    BuildAndLayout(MakeParams(false));
    float short_h = menu_.GetMenuHeight();

    EXPECT_LT(short_h, full_h);
}

// ─── 項目矩形 ───

TEST_F(ContextMenuLayoutTest, AllItemsHavePositiveHeight)
{
    BuildAndLayout(MakeParams());
    for (const auto& item : menu_.GetItems()) {
        EXPECT_LT(item.rect.top, item.rect.bottom)
            << "item id=" << item.id;
    }
}

TEST_F(ContextMenuLayoutTest, ItemsSpanFullWidth)
{
    BuildAndLayout(MakeParams());
    float w = menu_.GetMenuWidth();
    for (const auto& item : menu_.GetItems()) {
        EXPECT_FLOAT_EQ(item.rect.left, 0.0f);
        EXPECT_FLOAT_EQ(item.rect.right, w);
    }
}

TEST_F(ContextMenuLayoutTest, ItemsDoNotOverlapVertically)
{
    BuildAndLayout(MakeParams());
    const auto& items = menu_.GetItems();
    for (size_t i = 1; i < items.size(); ++i) {
        EXPECT_GE(items[i].rect.top, items[i - 1].rect.bottom)
            << "item " << i << " overlaps item " << (i - 1);
    }
}

TEST_F(ContextMenuLayoutTest, ItemsFitWithinMenuHeight)
{
    BuildAndLayout(MakeParams());
    float h = menu_.GetMenuHeight();
    for (const auto& item : menu_.GetItems()) {
        EXPECT_LE(item.rect.bottom, h);
    }
}

// ─── ナビゲーションボタン配置 ───

TEST_F(ContextMenuLayoutTest, NavButtonsHavePositiveSize)
{
    BuildAndLayout(MakeParams());
    auto& nav = menu_.GetNavLayout();
    EXPECT_LT(nav.back_rect.left, nav.back_rect.right);
    EXPECT_LT(nav.back_rect.top, nav.back_rect.bottom);
    EXPECT_LT(nav.fwd_rect.left, nav.fwd_rect.right);
    EXPECT_LT(nav.fwd_rect.top, nav.fwd_rect.bottom);
}

TEST_F(ContextMenuLayoutTest, NavButtonsAreEqualSize)
{
    BuildAndLayout(MakeParams());
    auto& nav = menu_.GetNavLayout();
    float bw = nav.back_rect.right - nav.back_rect.left;
    float bh = nav.back_rect.bottom - nav.back_rect.top;
    float fw = nav.fwd_rect.right - nav.fwd_rect.left;
    float fh = nav.fwd_rect.bottom - nav.fwd_rect.top;
    EXPECT_FLOAT_EQ(bw, fw);
    EXPECT_FLOAT_EQ(bh, fh);
}

TEST_F(ContextMenuLayoutTest, NavButtonsDoNotOverlap)
{
    BuildAndLayout(MakeParams());
    auto& nav = menu_.GetNavLayout();
    EXPECT_LE(nav.back_rect.right, nav.fwd_rect.left);
}

TEST_F(ContextMenuLayoutTest, NavButtonsHaveGap)
{
    BuildAndLayout(MakeParams());
    auto& nav = menu_.GetNavLayout();
    float gap = nav.fwd_rect.left - nav.back_rect.right;
    EXPECT_GT(gap, 0.0f);
}

TEST_F(ContextMenuLayoutTest, NavButtonsCenteredHorizontally)
{
    BuildAndLayout(MakeParams());
    auto& nav = menu_.GetNavLayout();
    float w = menu_.GetMenuWidth();
    float btn_center = (nav.back_rect.left + nav.fwd_rect.right) / 2.0f;
    EXPECT_NEAR(btn_center, w / 2.0f, 1.0f);
}

TEST_F(ContextMenuLayoutTest, NavButtonsAreWithinNavRow)
{
    BuildAndLayout(MakeParams());
    auto& items = menu_.GetItems();
    auto& nav = menu_.GetNavLayout();
    ASSERT_GE(items.size(), 1u);
    auto& row = items[0].rect;
    EXPECT_GE(nav.back_rect.left, row.left);
    EXPECT_LE(nav.fwd_rect.right, row.right);
    EXPECT_GE(nav.back_rect.top, row.top);
    EXPECT_LE(nav.back_rect.bottom, row.bottom);
}

// ─── ヒットテスト ───

// Text 項目の中心は自身の id、NavRow / Separator の中心は 0 を返す。
TEST_F(ContextMenuLayoutTest, HitTestAtItemCenters)
{
    BuildAndLayout(MakeParams());
    const auto& items = menu_.GetItems();
    ASSERT_EQ(items.size(), std::size(kMdPaneItems));
    for (size_t i = 0; i < items.size(); ++i) {
        SCOPED_TRACE(i);
        const int expected = kMdPaneItems[i].type == ItemType::Text ? kMdPaneItems[i].id : 0;
        const auto [cx, cy] = Center(items[i].rect);
        EXPECT_EQ(menu_.HitTest(cx, cy), expected);
    }
}

TEST_F(ContextMenuLayoutTest, HitTestOutsideReturnsZero)
{
    BuildAndLayout(MakeParams());
    EXPECT_EQ(menu_.HitTest(-10.0f, -10.0f), 0);
    EXPECT_EQ(menu_.HitTest(9999.0f, 9999.0f), 0);
}

// ─── ナビゲーションヒットテスト ───

TEST_F(ContextMenuLayoutTest, NavHitTest)
{
    using Nav = ContextMenu::NavRowLayout;
    struct Case {
        const char* name;
        bool can_go_back;
        bool can_go_forward;
        Point (*point)(const Nav&);
        int expected;
    };
    constexpr Case kCases[] = {
        { "Back", true, true, [](const Nav& n) { return Center(n.back_rect); }, IDM_NAV_BACK },
        { "Forward", true, true, [](const Nav& n) { return Center(n.fwd_rect); }, IDM_NAV_FORWARD },
        { "BetweenButtons", true, true, [](const Nav& n) { return Point{ (n.back_rect.right + n.fwd_rect.left) / 2.0f, Center(n.back_rect).y }; }, 0 },
        { "DisabledBack", false, true, [](const Nav& n) { return Center(n.back_rect); }, 0 },
        { "DisabledForward", true, false, [](const Nav& n) { return Center(n.fwd_rect); }, 0 },
        { "Outside", true, true, [](const Nav&) { return Point{ -10.0f, -10.0f }; }, 0 },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        auto p = MakeParams();
        p.can_go_back = c.can_go_back;
        p.can_go_forward = c.can_go_forward;
        BuildAndLayout(p);
        const auto [x, y] = c.point(menu_.GetNavLayout());
        EXPECT_EQ(menu_.NavHitTest(x, y), c.expected);
    }
}
