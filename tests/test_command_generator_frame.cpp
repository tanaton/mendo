#include <gtest/gtest.h>
#include "cmd_gen_mock_test_base.h"
#include "d2d_util.h"
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
    auto& cmds = gen_.GenerateMdPane(nodes_, cache_, pane, 0.0f, TextSelection{});

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
    auto& cmds = gen_.GenerateMdPane(nodes_, cache_, pane, scroll_y, TextSelection{});

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
    auto& cmds = gen_.GenerateMdPane(nodes_, cache_, pane, scroll_y, TextSelection{}, OverlayButtonHit{}, dpi);

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
    auto& cmds = gen_.GenerateMdPane(nodes_, cache_, pane, 10000.0f, TextSelection{});

    EXPECT_EQ(CountCmd<DrawLineCmd>(cmds), 0) << "すべてのノードがビューポート上ならDrawLineCmdは生成されない";
    // PushClip + SetTransform×2 + PopClip のみ
    EXPECT_EQ(cmds.size(), 4u);
}

TEST_F(CmdGenFrameTest, EmptyNodesListEmitsOnlyFrameScaffolding)
{
    nodes_.clear();
    cache_.Resize(0);
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    auto& cmds = gen_.GenerateMdPane(nodes_, cache_, pane, 0.0f, TextSelection{});

    ASSERT_EQ(cmds.size(), 4u);
    EXPECT_TRUE(std::holds_alternative<PushClipCmd>(cmds[0]));
    EXPECT_TRUE(std::holds_alternative<SetTransformCmd>(cmds[1]));
    EXPECT_TRUE(std::holds_alternative<SetTransformCmd>(cmds[2]));
    EXPECT_TRUE(std::holds_alternative<PopClipCmd>(cmds[3]));
}

// ═══════════════════════════════════════════════
// SetSearchMatches
// ═══════════════════════════════════════════════

TEST_F(CmdGenFrameTest, SetSearchMatchesWithNullIsSafe)
{
    Parse("Hello world");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 400.0f };
    gen_.SetSearchMatches(nullptr, -1, 0);
    auto& cmds = gen_.GenerateMdPane(nodes_, cache_, pane, 0.0f, TextSelection{});
    EXPECT_FALSE(cmds.empty());
}

TEST_F(CmdGenFrameTest, SetSearchMatchesWithEmptyProducesNoHighlight)
{
    Parse("Hello world");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 400.0f };
    std::pmr::vector<SearchMatch> matches;
    gen_.SetSearchMatches(&matches, -1, 1);
    auto& cmds = gen_.GenerateMdPane(nodes_, cache_, pane, 0.0f, TextSelection{});
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
    const auto& cmds = gen_.GenerateMdPane(nodes_, cache_, pane, 0.0f, TextSelection{});
    EXPECT_TRUE(std::ranges::any_of(cmds, [](const DrawCommand& c) static {
        const auto* fr = std::get_if<FillRectCmd>(&c);
        return fr && fr->brush_id == BrushId::TableStripe;
    }));
}

// ストライプはライトで黒、ダークで白の半透明。
TEST(TableStripeColorTest, LightBlackDarkWhiteTranslucent)
{
    const auto light = TableStripeColor(false);
    const auto dark = TableStripeColor(true);
    EXPECT_FLOAT_EQ(light.r, 0.0f);
    EXPECT_FLOAT_EQ(dark.r, 1.0f);
    for (const auto& c : { light, dark }) {
        EXPECT_GT(c.a, 0.0f);
        EXPECT_LT(c.a, 1.0f);
    }
}
