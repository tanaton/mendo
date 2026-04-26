#include <gtest/gtest.h>
#include "command_generator.h"
#include "layout.h"
#include "mock_text_measurer.h"
#include "parser.h"
#include "search_state.h"
#include <optional>
#include <variant>

namespace {

class CmdGenFrameTest : public ::testing::Test {
protected:
    MockTextMeasurer mock_;
    LayoutEngine engine_;
    CommandGenerator gen_;
    Theme theme_;
    LayoutCache cache_;
    std::pmr::vector<Node> nodes_;

    void SetUp() override
    {
        theme_ = GetLightTheme();
        ASSERT_TRUE(engine_.Init(&mock_, theme_));
        gen_.SetTheme(&theme_);
        gen_.SetFormats({ nullptr, nullptr, nullptr, nullptr });
    }

    void Parse(const std::string& md, float viewport_w = 800.0f)
    {
        nodes_ = ParseMarkdown(md).nodes;
        cache_.Resize(nodes_.size());
        engine_.ComputeLayout(nodes_, cache_, viewport_w);
    }

    int CountCmd(const DrawCommandList& cmds, auto predicate)
    {
        int n = 0;
        for (const auto& c : cmds) {
            if (predicate(c)) n++;
        }
        return n;
    }
};

template <typename T>
const T* FindFirst(const DrawCommandList& cmds)
{
    for (const auto& c : cmds) {
        if (auto* p = std::get_if<T>(&c)) return p;
    }
    return nullptr;
}

} // namespace

// ═══════════════════════════════════════════════
// クリップ矩形とペイン原点
// ═══════════════════════════════════════════════

TEST_F(CmdGenFrameTest, PushClipMatchesPaneRect)
{
    Parse("Hello");
    const PaneRect pane{ 100.0f, 50.0f, 600.0f, 400.0f };
    auto& cmds = gen_.GenerateMdPane(nodes_, cache_, pane, 0.0f, TextSelection{});

    const auto* clip = FindFirst<PushClipCmd>(cmds);
    ASSERT_NE(clip, nullptr);
    EXPECT_FLOAT_EQ(clip->rect.left,   100.0f);
    EXPECT_FLOAT_EQ(clip->rect.top,     50.0f);
    EXPECT_FLOAT_EQ(clip->rect.right,  700.0f);
    EXPECT_FLOAT_EQ(clip->rect.bottom, 450.0f);
}

TEST_F(CmdGenFrameTest, TransformTranslatesByPaneOriginAndScroll)
{
    Parse("Hello");
    constexpr float origin_x = 120.0f;
    constexpr float scroll_y = 40.0f;
    const PaneRect pane{ origin_x, 0.0f, 600.0f, 400.0f };
    auto& cmds = gen_.GenerateMdPane(nodes_, cache_, pane, scroll_y, TextSelection{});

    const auto* first_xform = FindFirst<SetTransformCmd>(cmds);
    ASSERT_NE(first_xform, nullptr);
    EXPECT_FLOAT_EQ(first_xform->transform._31, origin_x);
    EXPECT_FLOAT_EQ(first_xform->transform._32, -scroll_y);
}

TEST_F(CmdGenFrameTest, TransformSnapsScrollToPixelWithDpi)
{
    // DPI=2.0 → round(scroll_y * 2) / 2 で 0.5px 境界にスナップ
    Parse("Hello");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 400.0f };
    const float scroll_y = 0.3f;
    const float dpi = 2.0f;
    auto& cmds = gen_.GenerateMdPane(nodes_, cache_, pane, scroll_y, TextSelection{}, -1, -1, -1, -1, dpi);

    const auto* xform = FindFirst<SetTransformCmd>(cmds);
    ASSERT_NE(xform, nullptr);
    EXPECT_FLOAT_EQ(xform->transform._32, -0.5f);
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

    int draw_lines = CountCmd(cmds, [](const auto& c) {
        return std::holds_alternative<DrawLineCmd>(c);
    });
    EXPECT_EQ(draw_lines, 0) << "すべてのノードがビューポート下ならDrawLineCmdは生成されない";
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
// first_visible の明示指定と自動探索
// ═══════════════════════════════════════════════

TEST_F(CmdGenFrameTest, ExplicitFirstVisibleMatchesAutoDiscovery)
{
    Parse("---\n\n---\n\n---\n\n---\n\n---");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 500.0f };

    // first_visible=-1 (自動) と first_visible=0 (明示) は 0 からスクロール時に同じ結果
    auto cmds_auto_saved = gen_.GenerateMdPane(nodes_, cache_, pane, 0.0f, TextSelection{}, -1).size();
    auto cmds_explicit_saved = gen_.GenerateMdPane(nodes_, cache_, pane, 0.0f, TextSelection{}, 0).size();
    EXPECT_EQ(cmds_auto_saved, cmds_explicit_saved);
}

TEST_F(CmdGenFrameTest, FirstVisibleBeyondEndProducesNoContent)
{
    Parse("---\n\n---\n\n---");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 500.0f };

    // first_visible がノード数以上 → 本体ループに入らない
    auto& cmds = gen_.GenerateMdPane(nodes_, cache_, pane, 0.0f, TextSelection{}, 999);
    int draw_lines = CountCmd(cmds, [](const auto& c) {
        return std::holds_alternative<DrawLineCmd>(c);
    });
    EXPECT_EQ(draw_lines, 0);
}

// ═══════════════════════════════════════════════
// ShrinkBuffers / SetSharedHitTestBuffer / SetSearchMatches
// ═══════════════════════════════════════════════

TEST_F(CmdGenFrameTest, ShrinkBuffersIsSafeWithoutSharedBuffer)
{
    Parse("Hello");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 400.0f };
    const auto baseline_size = gen_.GenerateMdPane(nodes_, cache_, pane, 0.0f, TextSelection{}).size();
    gen_.ShrinkBuffers();
    // ShrinkBuffers 後も同じ入力で同じコマンド列が再生成される
    const auto after_size = gen_.GenerateMdPane(nodes_, cache_, pane, 0.0f, TextSelection{}).size();
    EXPECT_EQ(after_size, baseline_size);
}

TEST_F(CmdGenFrameTest, SetSharedHitTestBufferPreventsShrink)
{
    Parse("Hello");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 400.0f };
    std::pmr::vector<DWRITE_HIT_TEST_METRICS> shared;
    shared.reserve(32);
    const auto capacity_before = shared.capacity();
    gen_.SetSharedHitTestBuffer(&shared);
    (void)gen_.GenerateMdPane(nodes_, cache_, pane, 0.0f, TextSelection{});
    gen_.ShrinkBuffers(); // shared 設定時は内部バッファを触らない
    // shared バッファ自体の容量は ShrinkBuffers では影響されない
    EXPECT_EQ(shared.capacity(), capacity_before);
}

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
    int fill_rects = CountCmd(cmds, [](const auto& c) {
        return std::holds_alternative<FillRectCmd>(c);
    });
    EXPECT_EQ(fill_rects, 0);
}

// ═══════════════════════════════════════════════
// テーマ変更の反映
// ═══════════════════════════════════════════════

TEST_F(CmdGenFrameTest, DarkThemeSelectionUsesDifferentStripeCache)
{
    auto light = GetLightTheme();
    auto dark = GetDarkTheme();
    Parse("| A | B |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 400.0f };

    auto find_stripe = [](const DrawCommandList& cmds) -> std::optional<D2D1_COLOR_F> {
        for (const auto& c : cmds) {
            if (auto* fr = std::get_if<FillRectCmd>(&c)) {
                if (fr->color.a > 0.0f && fr->color.a < 1.0f) {
                    return fr->color;
                }
            }
        }
        return std::nullopt;
    };

    gen_.SetTheme(&light);
    auto light_stripe = find_stripe(gen_.GenerateMdPane(nodes_, cache_, pane, 0.0f, TextSelection{}));
    ASSERT_TRUE(light_stripe.has_value());
    EXPECT_FLOAT_EQ(light_stripe->r, 0.0f);
    EXPECT_FLOAT_EQ(light_stripe->g, 0.0f);
    EXPECT_FLOAT_EQ(light_stripe->b, 0.0f);

    gen_.SetTheme(&dark);
    auto dark_stripe = find_stripe(gen_.GenerateMdPane(nodes_, cache_, pane, 0.0f, TextSelection{}));
    ASSERT_TRUE(dark_stripe.has_value());
    EXPECT_FLOAT_EQ(dark_stripe->r, 1.0f);
    EXPECT_FLOAT_EQ(dark_stripe->g, 1.0f);
    EXPECT_FLOAT_EQ(dark_stripe->b, 1.0f);
}
