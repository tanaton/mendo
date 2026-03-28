#include <gtest/gtest.h>
#include "context_menu.h"
#include "resource.h"
#include <dwrite.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// ============================================================
// BuildItems テスト（DWrite不要）
// ============================================================

class ContextMenuTest : public ::testing::Test {
protected:
    ContextMenu menu_;
    Theme theme_;

    void SetUp() override {
        theme_ = GetLightTheme();
    }

    void Build(const ContextMenuParams& params) {
        menu_.TestBuildItems(params);
    }

    ContextMenuParams MakeParams(bool show_file = true) {
        ContextMenuParams p;
        p.theme = &theme_;
        p.dpi_scale = 1.0f;
        p.can_go_back = true;
        p.can_go_forward = true;
        p.has_file = true;
        p.has_selection = true;
        p.dark_mode_checked = false;
        p.show_file_items = show_file;
        return p;
    }
};

// ─── 項目構築: MdPaneの場合 ───

TEST_F(ContextMenuTest, MdPaneItemCount) {
    Build(MakeParams(true));
    // NavRow, Sep, EditFile, Copy, Sep, DarkMode = 6項目
    EXPECT_EQ(menu_.GetItems().size(), 6u);
}

TEST_F(ContextMenuTest, FirstItemIsNavRow) {
    Build(MakeParams(true));
    EXPECT_EQ(menu_.GetItems()[0].type, ContextMenu::ItemType::NavRow);
}

TEST_F(ContextMenuTest, SecondItemIsSeparator) {
    Build(MakeParams(true));
    EXPECT_EQ(menu_.GetItems()[1].type, ContextMenu::ItemType::Separator);
}

TEST_F(ContextMenuTest, EditFileItemHasCorrectId) {
    Build(MakeParams(true));
    EXPECT_EQ(menu_.GetItems()[2].id, IDM_EDIT_FILE);
}

TEST_F(ContextMenuTest, CopyItemHasCorrectId) {
    Build(MakeParams(true));
    EXPECT_EQ(menu_.GetItems()[3].id, IDM_COPY);
}

TEST_F(ContextMenuTest, DarkModeItemHasCorrectId) {
    Build(MakeParams(true));
    EXPECT_EQ(menu_.GetItems()[5].id, IDM_TOGGLE_DARK_MODE);
}

// ─── 項目構築: 非MdPaneの場合 ───

TEST_F(ContextMenuTest, NonMdPaneItemCount) {
    Build(MakeParams(false));
    // NavRow, Sep, DarkMode = 3項目
    EXPECT_EQ(menu_.GetItems().size(), 3u);
}

TEST_F(ContextMenuTest, NonMdPaneHasNoCopyOrEdit) {
    Build(MakeParams(false));
    for (const auto& item : menu_.GetItems()) {
        EXPECT_NE(item.id, IDM_EDIT_FILE);
        EXPECT_NE(item.id, IDM_COPY);
    }
}

// ─── 項目の有効/無効状態 ───

TEST_F(ContextMenuTest, EditFileDisabledWhenNoFile) {
    auto p = MakeParams(true);
    p.has_file = false;
    Build(p);
    EXPECT_FALSE(menu_.GetItems()[2].enabled);
}

TEST_F(ContextMenuTest, CopyDisabledWhenNoSelection) {
    auto p = MakeParams(true);
    p.has_selection = false;
    Build(p);
    EXPECT_FALSE(menu_.GetItems()[3].enabled);
}

TEST_F(ContextMenuTest, DarkModeCheckedState) {
    auto p = MakeParams(true);
    p.dark_mode_checked = true;
    Build(p);
    EXPECT_TRUE(menu_.GetItems()[5].checked);
}

TEST_F(ContextMenuTest, DarkModeUncheckedState) {
    Build(MakeParams(true));
    EXPECT_FALSE(menu_.GetItems()[5].checked);
}

TEST_F(ContextMenuTest, NavBackEnabled) {
    auto p = MakeParams(true);
    p.can_go_back = true;
    Build(p);
    EXPECT_TRUE(menu_.GetNavLayout().back_enabled);
}

TEST_F(ContextMenuTest, NavBackDisabled) {
    auto p = MakeParams(true);
    p.can_go_back = false;
    Build(p);
    EXPECT_FALSE(menu_.GetNavLayout().back_enabled);
}

TEST_F(ContextMenuTest, NavForwardEnabled) {
    auto p = MakeParams(true);
    p.can_go_forward = true;
    Build(p);
    EXPECT_TRUE(menu_.GetNavLayout().fwd_enabled);
}

TEST_F(ContextMenuTest, NavForwardDisabled) {
    auto p = MakeParams(true);
    p.can_go_forward = false;
    Build(p);
    EXPECT_FALSE(menu_.GetNavLayout().fwd_enabled);
}

TEST_F(ContextMenuTest, DarkModeItemAlwaysEnabled) {
    Build(MakeParams(true));
    // DarkModeは常にenabled
    for (const auto& item : menu_.GetItems()) {
        if (item.id == IDM_TOGGLE_DARK_MODE) {
            EXPECT_TRUE(item.enabled);
        }
    }
}

// ============================================================
// レイアウト・ヒットテスト（DWrite必要）
// ============================================================

class ContextMenuLayoutTest : public ::testing::Test {
protected:
    static ComPtr<IDWriteFactory> dwrite_;
    static ComPtr<ID2D1Factory> d2d_;

    static void SetUpTestSuite() {
        DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf());
    }

    static void TearDownTestSuite() {
        dwrite_.Reset();
        d2d_.Reset();
    }

    ContextMenu menu_;
    Theme theme_;

    void SetUp() override {
        theme_ = GetLightTheme();
        menu_.Init(d2d_.Get(), dwrite_.Get());
    }

    void BuildAndLayout(const ContextMenuParams& params) {
        menu_.TestBuildItems(params);
        menu_.TestCreateTextFormats(*params.theme);
        menu_.TestComputeLayout();
    }

    ContextMenuParams MakeParams(bool show_file = true) {
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
};

ComPtr<IDWriteFactory> ContextMenuLayoutTest::dwrite_;
ComPtr<ID2D1Factory> ContextMenuLayoutTest::d2d_;

// ─── メニューサイズ ───

TEST_F(ContextMenuLayoutTest, MenuWidthIsPositive) {
    BuildAndLayout(MakeParams());
    EXPECT_GT(menu_.GetMenuWidth(), 0.0f);
}

TEST_F(ContextMenuLayoutTest, MenuHeightIsPositive) {
    BuildAndLayout(MakeParams());
    EXPECT_GT(menu_.GetMenuHeight(), 0.0f);
}

TEST_F(ContextMenuLayoutTest, MenuWidthAtLeast160) {
    BuildAndLayout(MakeParams());
    EXPECT_GE(menu_.GetMenuWidth(), 160.0f);
}

TEST_F(ContextMenuLayoutTest, NonMdPaneMenuIsShorter) {
    BuildAndLayout(MakeParams(true));
    float full_h = menu_.GetMenuHeight();

    BuildAndLayout(MakeParams(false));
    float short_h = menu_.GetMenuHeight();

    EXPECT_LT(short_h, full_h);
}

// ─── 項目矩形 ───

TEST_F(ContextMenuLayoutTest, AllItemsHavePositiveHeight) {
    BuildAndLayout(MakeParams());
    for (const auto& item : menu_.GetItems()) {
        EXPECT_LT(item.rect.top, item.rect.bottom)
            << "item id=" << item.id;
    }
}

TEST_F(ContextMenuLayoutTest, ItemsSpanFullWidth) {
    BuildAndLayout(MakeParams());
    float w = menu_.GetMenuWidth();
    for (const auto& item : menu_.GetItems()) {
        EXPECT_FLOAT_EQ(item.rect.left, 0.0f);
        EXPECT_FLOAT_EQ(item.rect.right, w);
    }
}

TEST_F(ContextMenuLayoutTest, ItemsDoNotOverlapVertically) {
    BuildAndLayout(MakeParams());
    const auto& items = menu_.GetItems();
    for (size_t i = 1; i < items.size(); ++i) {
        EXPECT_GE(items[i].rect.top, items[i - 1].rect.bottom)
            << "item " << i << " overlaps item " << (i - 1);
    }
}

TEST_F(ContextMenuLayoutTest, ItemsFitWithinMenuHeight) {
    BuildAndLayout(MakeParams());
    float h = menu_.GetMenuHeight();
    for (const auto& item : menu_.GetItems()) {
        EXPECT_LE(item.rect.bottom, h);
    }
}

// ─── ナビゲーションボタン配置 ───

TEST_F(ContextMenuLayoutTest, NavButtonsHavePositiveSize) {
    BuildAndLayout(MakeParams());
    auto& nav = menu_.GetNavLayout();
    EXPECT_LT(nav.back_rect.left, nav.back_rect.right);
    EXPECT_LT(nav.back_rect.top, nav.back_rect.bottom);
    EXPECT_LT(nav.fwd_rect.left, nav.fwd_rect.right);
    EXPECT_LT(nav.fwd_rect.top, nav.fwd_rect.bottom);
}

TEST_F(ContextMenuLayoutTest, NavButtonsAreEqualSize) {
    BuildAndLayout(MakeParams());
    auto& nav = menu_.GetNavLayout();
    float bw = nav.back_rect.right - nav.back_rect.left;
    float bh = nav.back_rect.bottom - nav.back_rect.top;
    float fw = nav.fwd_rect.right - nav.fwd_rect.left;
    float fh = nav.fwd_rect.bottom - nav.fwd_rect.top;
    EXPECT_FLOAT_EQ(bw, fw);
    EXPECT_FLOAT_EQ(bh, fh);
}

TEST_F(ContextMenuLayoutTest, NavButtonsDoNotOverlap) {
    BuildAndLayout(MakeParams());
    auto& nav = menu_.GetNavLayout();
    EXPECT_LE(nav.back_rect.right, nav.fwd_rect.left);
}

TEST_F(ContextMenuLayoutTest, NavButtonsHaveGap) {
    BuildAndLayout(MakeParams());
    auto& nav = menu_.GetNavLayout();
    float gap = nav.fwd_rect.left - nav.back_rect.right;
    EXPECT_GT(gap, 0.0f);
}

TEST_F(ContextMenuLayoutTest, NavButtonsCenteredHorizontally) {
    BuildAndLayout(MakeParams());
    auto& nav = menu_.GetNavLayout();
    float w = menu_.GetMenuWidth();
    float btn_center = (nav.back_rect.left + nav.fwd_rect.right) / 2.0f;
    EXPECT_NEAR(btn_center, w / 2.0f, 1.0f);
}

TEST_F(ContextMenuLayoutTest, NavButtonsAreWithinNavRow) {
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

TEST_F(ContextMenuLayoutTest, HitTestOnEditFile) {
    BuildAndLayout(MakeParams());
    auto& items = menu_.GetItems();
    // EditFileは3番目（index 2）
    float cx = (items[2].rect.left + items[2].rect.right) / 2.0f;
    float cy = (items[2].rect.top + items[2].rect.bottom) / 2.0f;
    EXPECT_EQ(menu_.HitTest(cx, cy), IDM_EDIT_FILE);
}

TEST_F(ContextMenuLayoutTest, HitTestOnCopy) {
    BuildAndLayout(MakeParams());
    auto& items = menu_.GetItems();
    float cx = (items[3].rect.left + items[3].rect.right) / 2.0f;
    float cy = (items[3].rect.top + items[3].rect.bottom) / 2.0f;
    EXPECT_EQ(menu_.HitTest(cx, cy), IDM_COPY);
}

TEST_F(ContextMenuLayoutTest, HitTestOnDarkMode) {
    BuildAndLayout(MakeParams());
    auto& items = menu_.GetItems();
    auto& last = items.back();
    float cx = (last.rect.left + last.rect.right) / 2.0f;
    float cy = (last.rect.top + last.rect.bottom) / 2.0f;
    EXPECT_EQ(menu_.HitTest(cx, cy), IDM_TOGGLE_DARK_MODE);
}

TEST_F(ContextMenuLayoutTest, HitTestOnSeparatorReturnsZero) {
    BuildAndLayout(MakeParams());
    auto& items = menu_.GetItems();
    // 2番目（index 1）はセパレータ
    float cx = (items[1].rect.left + items[1].rect.right) / 2.0f;
    float cy = (items[1].rect.top + items[1].rect.bottom) / 2.0f;
    EXPECT_EQ(menu_.HitTest(cx, cy), 0);
}

TEST_F(ContextMenuLayoutTest, HitTestOutsideReturnsZero) {
    BuildAndLayout(MakeParams());
    EXPECT_EQ(menu_.HitTest(-10.0f, -10.0f), 0);
    EXPECT_EQ(menu_.HitTest(9999.0f, 9999.0f), 0);
}

// ─── ナビゲーションヒットテスト ───

TEST_F(ContextMenuLayoutTest, NavHitTestOnBackButton) {
    BuildAndLayout(MakeParams());
    auto& nav = menu_.GetNavLayout();
    float cx = (nav.back_rect.left + nav.back_rect.right) / 2.0f;
    float cy = (nav.back_rect.top + nav.back_rect.bottom) / 2.0f;
    EXPECT_EQ(menu_.NavHitTest(cx, cy), IDM_NAV_BACK);
}

TEST_F(ContextMenuLayoutTest, NavHitTestOnForwardButton) {
    BuildAndLayout(MakeParams());
    auto& nav = menu_.GetNavLayout();
    float cx = (nav.fwd_rect.left + nav.fwd_rect.right) / 2.0f;
    float cy = (nav.fwd_rect.top + nav.fwd_rect.bottom) / 2.0f;
    EXPECT_EQ(menu_.NavHitTest(cx, cy), IDM_NAV_FORWARD);
}

TEST_F(ContextMenuLayoutTest, NavHitTestBetweenButtonsReturnsZero) {
    BuildAndLayout(MakeParams());
    auto& nav = menu_.GetNavLayout();
    float gap_x = (nav.back_rect.right + nav.fwd_rect.left) / 2.0f;
    float cy = (nav.back_rect.top + nav.back_rect.bottom) / 2.0f;
    EXPECT_EQ(menu_.NavHitTest(gap_x, cy), 0);
}

TEST_F(ContextMenuLayoutTest, NavHitTestDisabledBackReturnsZero) {
    auto p = MakeParams();
    p.can_go_back = false;
    BuildAndLayout(p);
    auto& nav = menu_.GetNavLayout();
    float cx = (nav.back_rect.left + nav.back_rect.right) / 2.0f;
    float cy = (nav.back_rect.top + nav.back_rect.bottom) / 2.0f;
    EXPECT_EQ(menu_.NavHitTest(cx, cy), 0);
}

TEST_F(ContextMenuLayoutTest, NavHitTestDisabledForwardReturnsZero) {
    auto p = MakeParams();
    p.can_go_forward = false;
    BuildAndLayout(p);
    auto& nav = menu_.GetNavLayout();
    float cx = (nav.fwd_rect.left + nav.fwd_rect.right) / 2.0f;
    float cy = (nav.fwd_rect.top + nav.fwd_rect.bottom) / 2.0f;
    EXPECT_EQ(menu_.NavHitTest(cx, cy), 0);
}

TEST_F(ContextMenuLayoutTest, NavHitTestOutsideReturnsZero) {
    BuildAndLayout(MakeParams());
    EXPECT_EQ(menu_.NavHitTest(-10.0f, -10.0f), 0);
}
