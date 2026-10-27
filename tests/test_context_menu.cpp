#include <gtest/gtest.h>
#include "context_menu_impl.h"
#include "i18n.h"
#include "resource.h"
#include "theme.h"
#include <algorithm>
#include <dwrite.h>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

using ItemType = ContextMenu::Impl::ItemType;

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
    ContextMenu::Impl menu_;
    Theme theme_ = GetLightTheme();

    void Build(const ContextMenuParams& params)
    {
        menu_.BuildItems(params);
    }

    ContextMenuParams MakeParams(bool show_file = true) const
    {
        ContextMenuParams p;
        p.env.theme = &theme_;
        p.env.dpi_scale = 1.0f;
        p.can_go_back = true;
        p.can_go_forward = true;
        p.can_edit_file = true;
        p.has_selection = true;
        p.show_file_items = show_file;
        return p;
    }

    const ContextMenu::Impl::Item* FindItem(int id) const
    {
        const auto& items = menu_.items;
        const auto it = std::ranges::find(items, id, &ContextMenu::Impl::Item::id);
        return it != items.end() ? &*it : nullptr;
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
        const auto& items = menu_.items;
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
        bool ContextMenu::Impl::Item::* flag;
        bool expected;
    };
    using Item = ContextMenu::Impl::Item;
    constexpr Case kCases[] = {
        { "EditFileDisabledWhenNoFile", [](ContextMenuParams& p) static { p.can_edit_file = false; }, IDM_EDIT_FILE, &Item::enabled, false },
        { "CopyDisabledWhenNoSelection", [](ContextMenuParams& p) static { p.has_selection = false; }, IDM_COPY, &Item::enabled, false },
        { "CopyFormattedDisabledWhenNoSelection", [](ContextMenuParams& p) static { p.has_selection = false; }, IDM_COPY_FORMATTED, &Item::enabled, false },
        { "DarkModeChecked", [](ContextMenuParams& p) static { p.dark_mode_checked = true; }, IDM_TOGGLE_DARK_MODE, &Item::checked, true },
        { "DarkModeUnchecked", nullptr, IDM_TOGGLE_DARK_MODE, &Item::checked, false },
        { "DarkModeAlwaysEnabled", nullptr, IDM_TOGGLE_DARK_MODE, &Item::enabled, true },
        { "FilePaneChecked", [](ContextMenuParams& p) static { p.file_pane_checked = true; }, IDM_TOGGLE_FILE_PANE, &Item::checked, true },
        { "FilePaneUnchecked", [](ContextMenuParams& p) static { p.file_pane_checked = false; }, IDM_TOGGLE_FILE_PANE, &Item::checked, false },
        { "TocPaneChecked", [](ContextMenuParams& p) static { p.toc_pane_checked = true; }, IDM_TOGGLE_TOC_PANE, &Item::checked, true },
        { "TocPaneUnchecked", [](ContextMenuParams& p) static { p.toc_pane_checked = false; }, IDM_TOGGLE_TOC_PANE, &Item::checked, false },
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

// ズームの行は倍率を渡した領域でだけ出し、直後に区切りを置く。
TEST_F(ContextMenuTest, ZoomRowOnlyWithZoomLevel)
{
    const auto has_zoom_row = [this] {
        return std::ranges::contains(menu_.items, ItemType::ZoomRow, &ContextMenu::Impl::Item::type);
    };
    Build(MakeParams(false));
    EXPECT_FALSE(has_zoom_row());

    ZoomLevel zoom;
    auto p = MakeParams(false);
    p.zoom = &zoom;
    Build(p);
    const auto& items = menu_.items;
    const auto it = std::ranges::find(items, ItemType::ZoomRow, &ContextMenu::Impl::Item::type);
    ASSERT_NE(it, items.end());
    ASSERT_NE(std::next(it), items.end());
    EXPECT_EQ(std::next(it)->type, ItemType::Separator);
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
            EXPECT_EQ(menu_.nav_layout.back_enabled, back);
            EXPECT_EQ(menu_.nav_layout.fwd_enabled, fwd);
        }
    }
}

// ============================================================
// レイアウト・ヒットテスト（DWrite必要）
// ============================================================

// CreateTextFormats / ComputeLayout は DWrite だけを使う。D2D は Show で描くときにしか要らない。
class ContextMenuLayoutTest : public ContextMenuTest {
protected:
    static ComPtr<IDWriteFactory> dwrite_;

    static void SetUpTestSuite()
    {
        const HRESULT hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
        ASSERT_TRUE(SUCCEEDED(hr)) << "DWriteCreateFactory failed";
        ASSERT_NE(dwrite_.Get(), nullptr);
    }

    static void TearDownTestSuite()
    {
        dwrite_.Reset();
    }

    void SetUp() override
    {
        menu_.dwrite_factory = dwrite_.Get();
    }

    void BuildAndLayout(const ContextMenuParams& params)
    {
        menu_.theme = params.env.theme;
        menu_.BuildItems(params);
        menu_.CreateBaseTextFormats(context_menu_constants::ICON_FONT_SIZE);
        menu_.ComputeLayout();
    }
};

ComPtr<IDWriteFactory> ContextMenuLayoutTest::dwrite_;

// ─── メニューサイズ ───

TEST_F(ContextMenuLayoutTest, MenuHasMinimumWidthAndPositiveHeight)
{
    BuildAndLayout(MakeParams());
    EXPECT_GE(menu_.width, 160.0f);
    EXPECT_GT(menu_.height, 0.0f);
}

// 文字幅は覚えて開くたびに測らないが、書式を作り直したら測り直す。
TEST_F(ContextMenuLayoutTest, TextWidthIsRemeasuredWhenFontChanges)
{
    auto params = MakeParams();
    BuildAndLayout(params);
    const float width = menu_.width;
    BuildAndLayout(params);
    EXPECT_FLOAT_EQ(menu_.width, width);

    Theme big = *params.env.theme;
    big.pane_font_size *= 4.0f;
    params.env.theme = &big;
    BuildAndLayout(params);
    EXPECT_GT(menu_.width, width);
}

// 項目は UI の文言なので、言語を切り替えたら書式もその言語のロケールで作り直す。
TEST_F(ContextMenuLayoutTest, TextFormatFollowsUiLanguage)
{
    const auto params = MakeParams();
    for (const i18n::Strings* language : { &i18n::kZhHant, &i18n::kKo, &i18n::kJa }) {
        SCOPED_TRACE(std::wstring(language->lang_key));
        i18n::SetLanguage(language);
        BuildAndLayout(params);
        wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
        ASSERT_TRUE(SUCCEEDED(menu_.fmt_text->GetLocaleName(locale, LOCALE_NAME_MAX_LENGTH)));
        EXPECT_STREQ(locale, language->lang_locale);
    }
}

TEST_F(ContextMenuLayoutTest, NonMdPaneMenuIsShorter)
{
    BuildAndLayout(MakeParams(true));
    const float full_h = menu_.height;

    BuildAndLayout(MakeParams(false));
    const float short_h = menu_.height;

    EXPECT_LT(short_h, full_h);
}

// ─── 項目矩形 ───

// 項目は全幅で、重ならずに上から積まれ、メニューの高さに収まる。
TEST_F(ContextMenuLayoutTest, ItemsStackVerticallyAtFullWidth)
{
    BuildAndLayout(MakeParams());
    const float w = menu_.width;
    const float h = menu_.height;
    const auto& items = menu_.items;
    for (size_t i = 0; i < items.size(); ++i) {
        SCOPED_TRACE(i);
        const auto& rect = items[i].rect;
        EXPECT_LT(rect.top, rect.bottom);
        EXPECT_FLOAT_EQ(rect.left, 0.0f);
        EXPECT_FLOAT_EQ(rect.right, w);
        EXPECT_LE(rect.bottom, h);
        if (i > 0) {
            EXPECT_GE(rect.top, items[i - 1].rect.bottom);
        }
    }
}

// ─── ナビゲーションボタン配置 ───

// 同じ大きさの 2 ボタンが間を空けて並び、ナビ行の中で左右中央に置かれる。
TEST_F(ContextMenuLayoutTest, NavButtonsAreCenteredPairInNavRow)
{
    BuildAndLayout(MakeParams());
    const auto& nav = menu_.nav_layout;
    const float bw = nav.back_rect.right - nav.back_rect.left;
    const float bh = nav.back_rect.bottom - nav.back_rect.top;
    EXPECT_GT(bw, 0.0f);
    EXPECT_GT(bh, 0.0f);
    EXPECT_FLOAT_EQ(nav.fwd_rect.right - nav.fwd_rect.left, bw);
    EXPECT_FLOAT_EQ(nav.fwd_rect.bottom - nav.fwd_rect.top, bh);
    EXPECT_GT(nav.fwd_rect.left - nav.back_rect.right, 0.0f);

    const float btn_center = (nav.back_rect.left + nav.fwd_rect.right) / 2.0f;
    EXPECT_NEAR(btn_center, menu_.width / 2.0f, 1.0f);

    const auto& items = menu_.items;
    ASSERT_FALSE(items.empty());
    ASSERT_EQ(items[0].type, ItemType::NavRow);
    const auto& row = items[0].rect;
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
    const auto& items = menu_.items;
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

// ─── ズームの行 ───

// 縮小・倍率・拡大・元に戻すが左から重ならずに並び、見出しの右・メニューの内側に収まる。
TEST_F(ContextMenuLayoutTest, ZoomRowButtonsLineUpInsideRow)
{
    ZoomLevel zoom;
    auto p = MakeParams(false);
    p.zoom = &zoom;
    BuildAndLayout(p);
    const auto it = std::ranges::find(menu_.items, ItemType::ZoomRow, &ContextMenu::Impl::Item::type);
    ASSERT_NE(it, menu_.items.end());
    const auto& row = it->rect;
    const auto& z = menu_.zoom_layout;
    EXPECT_GT(z.out_rect.left, menu_.Measure(it->text, menu_.fmt_text.Get()));
    EXPECT_LE(z.out_rect.right, z.percent_rect.left);
    EXPECT_LE(z.percent_rect.right, z.in_rect.left);
    EXPECT_LT(z.in_rect.right, z.reset_rect.left);
    EXPECT_LE(z.reset_rect.right, menu_.width);
    for (const auto* r : { &z.out_rect, &z.percent_rect, &z.in_rect, &z.reset_rect }) {
        EXPECT_GE(r->top, row.top);
        EXPECT_LE(r->bottom, row.bottom);
    }
}

// 端に着いた向きと、等倍での元に戻すは押せない。倍率の数字は押せない。
TEST_F(ContextMenuLayoutTest, ZoomHitTestFollowsZoomLevel)
{
    struct Case {
        const char* name;
        int index;
        bool out, in, reset;
    };
    constexpr Case kCases[] = {
        { "Default", ZOOM_DEFAULT_INDEX, true, true, false },
        { "Max", ZOOM_STEP_COUNT - 1, true, false, true },
        { "Min", 0, false, true, true },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        ZoomLevel zoom;
        zoom.SetIndex(c.index);
        auto p = MakeParams(false);
        p.zoom = &zoom;
        BuildAndLayout(p);
        const auto& z = menu_.zoom_layout;
        const auto hit = [this](const DipRect& r) {
            const auto [cx, cy] = Center(r);
            return menu_.ZoomHitTest(cx, cy);
        };
        const auto expect = [](bool enabled, ZoomDirection d) { return enabled ? std::optional{ d } : std::nullopt; };
        EXPECT_EQ(hit(z.out_rect), expect(c.out, ZoomDirection::Out));
        EXPECT_EQ(hit(z.in_rect), expect(c.in, ZoomDirection::In));
        EXPECT_EQ(hit(z.reset_rect), expect(c.reset, ZoomDirection::Reset));
        EXPECT_EQ(hit(z.percent_rect), std::nullopt);
    }
}

// ─── ナビゲーションヒットテスト ───

TEST_F(ContextMenuLayoutTest, NavHitTest)
{
    using Nav = ContextMenu::Impl::NavRowLayout;
    struct Case {
        const char* name;
        bool can_go_back;
        bool can_go_forward;
        Point (*point)(const Nav&);
        int expected;
    };
    constexpr Case kCases[] = {
        { "Back", true, true, [](const Nav& n) static { return Center(n.back_rect); }, IDM_NAV_BACK },
        { "Forward", true, true, [](const Nav& n) static { return Center(n.fwd_rect); }, IDM_NAV_FORWARD },
        { "BetweenButtons", true, true, [](const Nav& n) static { return Point{ (n.back_rect.right + n.fwd_rect.left) / 2.0f, Center(n.back_rect).y }; }, 0 },
        { "DisabledBack", false, true, [](const Nav& n) static { return Center(n.back_rect); }, 0 },
        { "DisabledForward", true, false, [](const Nav& n) static { return Center(n.fwd_rect); }, 0 },
        { "Outside", true, true, [](const Nav&) static { return Point{ -10.0f, -10.0f }; }, 0 },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        auto p = MakeParams();
        p.can_go_back = c.can_go_back;
        p.can_go_forward = c.can_go_forward;
        BuildAndLayout(p);
        const auto [x, y] = c.point(menu_.nav_layout);
        EXPECT_EQ(menu_.NavHitTest(x, y), c.expected);
    }
}
