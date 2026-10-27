#include <gtest/gtest.h>
#include "cmd_gen_mock_test_base.h"
#include "d2d_util.h"
#include "hit_test_service.h"
#include "search_state.h"
#include <algorithm>
#include <optional>
#include <variant>

namespace {

class CmdGenFrameTest : public CmdGenMockTestBase {};

} // namespace

// ═══════════════════════════════════════════════
// クリップ矩形とペイン原点
// ═══════════════════════════════════════════════

TEST_F(CmdGenFrameTest, PushClipMatchesPaneRect)
{
    Parse("Hello");
    const PaneRect pane{ 100.0f, 50.0f, 600.0f, 400.0f };
    auto& cmds = gen_.GenerateMdPane({ .nodes = nodes_, .cache = cache_, .selection = TextSelection{}, .md_pane_rect = pane }, 1.0f);

    const auto clip = FindFirst<PushClipCmd>(cmds);
    ASSERT_TRUE(clip.has_value());
    EXPECT_FLOAT_EQ(clip->rect.left, 100.0f);
    EXPECT_FLOAT_EQ(clip->rect.top, 50.0f);
    EXPECT_FLOAT_EQ(clip->rect.right, 700.0f);
    EXPECT_FLOAT_EQ(clip->rect.bottom, 450.0f);
}

TEST_F(CmdGenFrameTest, TransformTranslatesByPaneOriginOnly)
{
    // Transform の Y は常に 0、スクロール量は描画コマンドの Y に直接合算される。
    Parse("---");
    constexpr float origin_x = 120.0f;
    constexpr float scroll_y = 40.0f;
    const PaneRect pane{ origin_x, 0.0f, 600.0f, 400.0f };
    auto& cmds = gen_.GenerateMdPane({ .nodes = nodes_, .cache = cache_, .selection = TextSelection{}, .md_pane_rect = pane, .scroll_y = scroll_y }, 1.0f);

    const auto first_xform = FindFirst<SetTransformCmd>(cmds);
    ASSERT_TRUE(first_xform.has_value());
    EXPECT_FLOAT_EQ(first_xform->transform._31, origin_x);
    EXPECT_FLOAT_EQ(first_xform->transform._32, 0.0f);

    // スクロール量は HR の Y 座標に CPU 側で直接合算される。
    const auto line = FindFirst<DrawLineCmd>(cmds);
    ASSERT_TRUE(line.has_value());
    const float expected_y = cache_.Top(0) - scroll_y + theme_.paragraph_spacing * 0.5f;
    EXPECT_FLOAT_EQ(line->p0.y, expected_y);
}

TEST_F(CmdGenFrameTest, ScrollSnapsToPixelWithDpi)
{
    // DPI=2.0 → round(scroll_y * 2) / 2 で 0.5px 境界にスナップ。
    // スナップは Transform でなく描画コマンドの Y 計算に反映される。
    Parse("---");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 400.0f };
    const float scroll_y = 0.3f;
    const float dpi = 2.0f;
    auto& cmds = gen_.GenerateMdPane({ .nodes = nodes_, .cache = cache_, .selection = TextSelection{}, .md_pane_rect = pane, .scroll_y = scroll_y }, dpi);

    const auto xform = FindFirst<SetTransformCmd>(cmds);
    ASSERT_TRUE(xform.has_value());
    EXPECT_FLOAT_EQ(xform->transform._32, 0.0f);

    const auto line = FindFirst<DrawLineCmd>(cmds);
    ASSERT_TRUE(line.has_value());
    const float snapped_scroll = 0.5f; // round(0.3 * 2) / 2
    const float expected_y = cache_.Top(0) - snapped_scroll + theme_.paragraph_spacing * 0.5f;
    EXPECT_FLOAT_EQ(line->p0.y, expected_y);
}

// ═══════════════════════════════════════════════
// ビューポートカリング
// ═══════════════════════════════════════════════

TEST_F(CmdGenFrameTest, ScrolledBeyondBottomProducesOnlyFrameCommands)
{
    // 数個の水平線を生成し、ビューポートより大きく下にスクロールする
    Parse("---\n\n---\n\n---");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 100.0f };
    // 全ノードより下にスクロール → DrawLineCmd なし
    auto& cmds = gen_.GenerateMdPane({ .nodes = nodes_, .cache = cache_, .selection = TextSelection{}, .md_pane_rect = pane, .scroll_y = 10000.0f }, 1.0f);

    EXPECT_EQ(CountCmd<DrawLineCmd>(cmds), 0) << "すべてのノードがビューポート上ならDrawLineCmdは生成されない";
    // PushClip + SetTransform×2 + PopClip のみ
    EXPECT_EQ(cmds.size(), 4u);
}

TEST_F(CmdGenFrameTest, EmptyNodesListEmitsOnlyFrameScaffolding)
{
    nodes_.clear();
    cache_ = LayoutCache(0);
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    auto& cmds = gen_.GenerateMdPane({ .nodes = nodes_, .cache = cache_, .selection = TextSelection{}, .md_pane_rect = pane }, 1.0f);

    ASSERT_EQ(cmds.size(), 4u);
    EXPECT_TRUE(std::holds_alternative<PushClipCmd>(cmds[0]));
    EXPECT_TRUE(std::holds_alternative<SetTransformCmd>(cmds[1]));
    EXPECT_TRUE(std::holds_alternative<SetTransformCmd>(cmds[2]));
    EXPECT_TRUE(std::holds_alternative<PopClipCmd>(cmds[3]));
}

// ═══════════════════════════════════════════════
// 検索マッチ
// ═══════════════════════════════════════════════

TEST_F(CmdGenFrameTest, EmptySearchMatchesProduceNoHighlight)
{
    Parse("Hello world");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 400.0f };
    auto& cmds = gen_.GenerateMdPane({ .nodes = nodes_, .cache = cache_, .selection = TextSelection{}, .md_pane_rect = pane, .search_generation = 1 }, 1.0f);
    // マッチ空のため FillRectCmd は選択ハイライト経路以外は生成されない
    // (paragraph text_layout=null なので選択経路も走らない)
    EXPECT_EQ(CountCmd<FillRectCmd>(cmds), 0);
}

// ═══════════════════════════════════════════════
// テーマ変更の反映
// ═══════════════════════════════════════════════

TEST_F(CmdGenFrameTest, TableStripeUsesStripeBrush)
{
    Parse("| A | B |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 400.0f };
    const auto& cmds = gen_.GenerateMdPane({ .nodes = nodes_, .cache = cache_, .selection = TextSelection{}, .md_pane_rect = pane }, 1.0f);
    EXPECT_TRUE(std::ranges::any_of(cmds, [](const DrawCommand& c) static {
        const auto* fr = std::get_if<FillRectCmd>(&c);
        return fr && fr->brush_id == BrushId::TableStripe;
    }));
}

// ストライプはライトで黒、ダークで白の半透明。
TEST(TableStripeColorTest, LightBlackDarkWhiteTranslucent)
{
    const auto light = GetLightTheme().Color(BrushId::TableStripe);
    const auto dark = GetDarkTheme().Color(BrushId::TableStripe);
    EXPECT_FLOAT_EQ(light.r, 0.0f);
    EXPECT_FLOAT_EQ(dark.r, 1.0f);
    for (const auto& c : { light, dark }) {
        EXPECT_GT(c.a, 0.0f);
        EXPECT_LT(c.a, 1.0f);
    }
}

// ═══════════════════════════════════════════════
// 本文ペインに重ねるもの
// ═══════════════════════════════════════════════

namespace {

constexpr PaneRect kOverlayPane{ 100.0f, 50.0f, 600.0f, 400.0f };

} // namespace

// 点はペインの中央を回り、先頭が不透明で後ろほど薄い。
TEST(OverlayCommandsTest, LoadingSpinnerDotsFadeAroundCenter)
{
    DrawCommandList cmds;
    overlay::EmitLoadingSpinner(cmds, 0.0f, kOverlayPane);
    const auto dots = CollectCmds<FillEllipseCmd>(cmds);
    ASSERT_EQ(dots.size(), static_cast<size_t>(spinner::DOT_COUNT));
    EXPECT_EQ(cmds.size(), dots.size());
    EXPECT_FLOAT_EQ(dots[0].opacity, 1.0f);
    EXPECT_NEAR(dots[0].center.x, 400.0f + spinner::RADIUS, 1e-3f);
    EXPECT_NEAR(dots[0].center.y, 250.0f, 1e-3f);
    for (size_t i = 0; i < dots.size(); ++i) {
        EXPECT_EQ(dots[i].brush_id, BrushId::Text);
        EXPECT_NEAR(std::hypot(dots[i].center.x - 400.0f, dots[i].center.y - 250.0f), spinner::RADIUS, 1e-3f);
        if (i > 0) {
            EXPECT_LT(dots[i].opacity, dots[i - 1].opacity);
        }
    }
}

// ボタンはヒットテストと同じ矩形に、押せるか・ホバー中かで決まる不透明度の白黒で塗る。
TEST(OverlayCommandsTest, NavButtonsUseHitTestRectsAndStateOpacity)
{
    TextLayoutCache text_layouts;
    DrawCommandList cmds;
    overlay::EmitNavButtons(cmds, text_layouts, nullptr, kOverlayPane, /*dark=*/false, /*can_back=*/true, /*can_forward=*/false, NavDirection::Back);
    const auto bgs = CollectCmds<FillRoundedRectCmd>(cmds);
    ASSERT_EQ(bgs.size(), 2u);
    EXPECT_EQ(bgs[0].brush_id, BrushId::OverlayBlack);
    EXPECT_FLOAT_EQ(bgs[0].rect.left, NavBackButtonRect(kOverlayPane).left);
    EXPECT_FLOAT_EQ(bgs[1].rect.left, NavForwardButtonRect(kOverlayPane).left);
    EXPECT_FLOAT_EQ(bgs[0].opacity, 0.25f) << "ホバー中";
    EXPECT_FLOAT_EQ(bgs[1].opacity, 0.05f) << "押せない";

    cmds.clear();
    overlay::EmitNavButtons(cmds, text_layouts, nullptr, kOverlayPane, /*dark=*/true, true, true, NavDirection::None);
    const auto dark_bgs = CollectCmds<FillRoundedRectCmd>(cmds);
    ASSERT_EQ(dark_bgs.size(), 2u);
    EXPECT_EQ(dark_bgs[0].brush_id, BrushId::OverlayWhite);
    EXPECT_FLOAT_EQ(dark_bgs[0].opacity, 0.15f);
    EXPECT_FLOAT_EQ(dark_bgs[1].opacity, 0.15f);
}

// ジェスチャーの方向が無ければ何も積まない。パネルはダークで灰色、ライトで黒。
TEST(OverlayCommandsTest, GestureOverlayPanelFollowsTheme)
{
    TextLayoutCache text_layouts;
    DrawCommandList cmds;
    overlay::EmitGestureOverlay(cmds, text_layouts, nullptr, kOverlayPane, false, NavDirection::None);
    EXPECT_TRUE(cmds.empty());

    overlay::EmitGestureOverlay(cmds, text_layouts, nullptr, kOverlayPane, false, NavDirection::Back);
    auto panels = CollectCmds<FillRoundedRectCmd>(cmds);
    ASSERT_EQ(panels.size(), 1u);
    EXPECT_EQ(panels[0].brush_id, BrushId::OverlayBlack);
    EXPECT_FLOAT_EQ(panels[0].opacity, 0.6f);
    EXPECT_FLOAT_EQ((panels[0].rect.left + panels[0].rect.right) * 0.5f, 400.0f);
    EXPECT_FLOAT_EQ(panels[0].rect.right - panels[0].rect.left, GESTURE_OVERLAY_WIDTH);

    cmds.clear();
    overlay::EmitGestureOverlay(cmds, text_layouts, nullptr, kOverlayPane, true, NavDirection::Forward);
    panels = CollectCmds<FillRoundedRectCmd>(cmds);
    ASSERT_EQ(panels.size(), 1u);
    EXPECT_EQ(panels[0].brush_id, BrushId::OverlayGestureBg);
    EXPECT_FLOAT_EQ(panels[0].opacity, 0.8f);
}

// トーストはナビボタンの上に置く。空なら何も積まない。
TEST(OverlayCommandsTest, ToastSitsAboveNavButtons)
{
    TextLayoutCache text_layouts;
    DrawCommandList cmds;
    overlay::EmitToast(cmds, text_layouts, nullptr, kOverlayPane, false, L"");
    EXPECT_TRUE(cmds.empty());

    overlay::EmitToast(cmds, text_layouts, nullptr, kOverlayPane, false, L"message");
    const auto panels = CollectCmds<FillRoundedRectCmd>(cmds);
    ASSERT_EQ(panels.size(), 1u);
    EXPECT_FLOAT_EQ(panels[0].opacity, 0.7f);
    EXPECT_LT(panels[0].rect.bottom, NavBackButtonRect(kOverlayPane).top);
    EXPECT_FLOAT_EQ(panels[0].rect.bottom - panels[0].rect.top, TOAST_OVERLAY_HEIGHT);
}
