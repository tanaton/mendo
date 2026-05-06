// CommandGenerator の内容系（Heading 下線 / blockquote 装飾 / カリング境界）。
// MockTextMeasurer 経路で踏める範囲のみ。ハイライトや button 配置は
// IDWriteTextFormat / IDWriteTextLayout が必要なため別ファイル
// (test_command_generator_highlight.cpp) で扱う。
#include <gtest/gtest.h>
#include "cmd_gen_mock_test_base.h"
#include "test_helpers.h"
#include <algorithm>
#include <map>
#include <set>
#include <variant>

namespace {

class CmdGenContentTest : public CmdGenMockTestBase {};

} // namespace

// ---- Heading 下線 ----
// h1 / h2 のみ entry.height の下に下線が描画される（hr_color）。
// h3 以降では下線が出ない。

TEST_F(CmdGenContentTest, HeadingH1_EmitsUnderlineWithHrColor)
{
    Parse("# Title");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    auto& cmds = gen_.GenerateMdPane(nodes_, cache_, pane, 0.0f, TextSelection{});

    const auto hr_lines = std::ranges::count_if(cmds, [&](const auto& c) {
        if (auto* l = std::get_if<DrawLineCmd>(&c)) {
            return ColorEq(l->color, theme_.hr_color);
        }
        return false;
    });
    EXPECT_GE(hr_lines, 1) << "h1 では hr_color の下線が 1 本以上出るはず";
}

TEST_F(CmdGenContentTest, HeadingH2_EmitsUnderlineWithCorrectThickness)
{
    Parse("## Subtitle");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    auto& cmds = gen_.GenerateMdPane(nodes_, cache_, pane, 0.0f, TextSelection{});

    bool found = false;
    for (const auto& c : cmds) {
        if (auto* l = std::get_if<DrawLineCmd>(&c)) {
            if (ColorEq(l->color, theme_.hr_color) && l->stroke_width == theme_.h2_underline_thickness) {
                found = true;
                break;
            }
        }
    }
    EXPECT_TRUE(found) << "h2 下線は h2_underline_thickness を使うはず";
}

TEST_F(CmdGenContentTest, HeadingH3_DoesNotEmitUnderline)
{
    Parse("### Sub");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    auto& cmds = gen_.GenerateMdPane(nodes_, cache_, pane, 0.0f, TextSelection{});

    const auto draw_lines = std::ranges::count_if(cmds, [](const auto& c) {
        return std::holds_alternative<DrawLineCmd>(c);
    });
    EXPECT_EQ(draw_lines, 0) << "h3 は下線を発行しない";
}

// ---- blockquote グループ装飾 ----

TEST_F(CmdGenContentTest, BlockQuote_EmitsBarLine)
{
    Parse("> Quoted");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    auto& cmds = gen_.GenerateMdPane(nodes_, cache_, pane, 0.0f, TextSelection{});

    const auto bar_lines = std::ranges::count_if(cmds, [&](const auto& c) {
        if (auto* l = std::get_if<DrawLineCmd>(&c)) {
            return l->stroke_width == theme_.blockquote_bar_width && ColorEq(l->color, theme_.blockquote_bar_color);
        }
        return false;
    });
    EXPECT_GE(bar_lines, 1) << "blockquote には bar_color の縦バーが 1 本以上出る";
}

TEST_F(CmdGenContentTest, BlockQuote_FullyAboveViewport_NoBar)
{
    Parse("> Quoted");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 50.0f };
    // viewport_top を blockquote の下端より十分大きくする
    auto& cmds = gen_.GenerateMdPane(nodes_, cache_, pane, 100000.0f, TextSelection{});

    const auto bar_lines = std::ranges::count_if(cmds, [](const auto& c) {
        return std::holds_alternative<DrawLineCmd>(c);
    });
    EXPECT_EQ(bar_lines, 0) << "viewport より上にあるグループは bar が描画されない";
}

// ネスト blockquote の各レベルを別 x 座標のバーで描画する (PR #156)
TEST_F(CmdGenContentTest, NestedBlockQuote_EmitsBarPerLevel)
{
    Parse("> outer\n> > inner");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    auto& cmds = gen_.GenerateMdPane(nodes_, cache_, pane, 0.0f, TextSelection{});

    std::set<float> bar_x_positions;
    for (const auto& c : cmds) {
        if (auto* l = std::get_if<DrawLineCmd>(&c)) {
            if (l->stroke_width == theme_.blockquote_bar_width && ColorEq(l->color, theme_.blockquote_bar_color)) {
                bar_x_positions.insert(l->p0.x);
            }
        }
    }
    EXPECT_GE(bar_x_positions.size(), 2u)
        << "2 段ネストではレベルごとに別 x 座標のバーが出る";
}

// 外→内→外の連続描画: 外側 (level=1) はネストを貫通して 1 本のバーになる
TEST_F(CmdGenContentTest, NestedBlockQuote_OuterBarSpansAcrossInnerNesting)
{
    Parse("> outer\n> > inner\n>\n> back to outer");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    auto& cmds = gen_.GenerateMdPane(nodes_, cache_, pane, 0.0f, TextSelection{});

    std::map<float, int> count_by_x;
    for (const auto& c : cmds) {
        if (auto* l = std::get_if<DrawLineCmd>(&c)) {
            if (l->stroke_width == theme_.blockquote_bar_width && ColorEq(l->color, theme_.blockquote_bar_color)) {
                count_by_x[l->p0.x]++;
            }
        }
    }
    ASSERT_GE(count_by_x.size(), 2u);
    // 最も左の x 座標が level=1 (外側) のバー。連続描画されているなら 1 本のみ。
    EXPECT_EQ(count_by_x.begin()->second, 1)
        << "外側バーはネストを貫通して 1 本連続描画される";
}

// Alert ネスト + 後段: 最外側バーは Alert 色で描画される
TEST_F(CmdGenContentTest, AlertWithNested_OuterBarUsesAlertColor)
{
    Parse("> [!NOTE]\n> Alert head\n> > inner\n>\n> Alert continues");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    auto& cmds = gen_.GenerateMdPane(nodes_, cache_, pane, 0.0f, TextSelection{});

    const auto note_color = theme_.alert_color[AlertColorIndex(AlertType::Note)];
    const bool has_alert_bar = std::ranges::any_of(cmds, [&](const auto& c) {
        if (auto* l = std::get_if<DrawLineCmd>(&c)) {
            return ColorEq(l->color, note_color);
        }
        return false;
    });
    EXPECT_TRUE(has_alert_bar)
        << "ネストありの Alert でも最外側バーは Note 色で描画される";
}

// ---- HorizontalRule カリング: 上方向 ----
// 既存の test_command_generator_frame.cpp に「下方向」カリングはあるが
// 「上方向」（大量スクロールで全 hr が viewport 上にある）は未網羅。

TEST_F(CmdGenContentTest, HorizontalRule_AllAboveViewport_NoLines)
{
    Parse("---\n\n---\n\n---");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 100.0f };
    // 全 hr ノードより十分下にスクロール → 上方向カリングを踏む
    auto& cmds = gen_.GenerateMdPane(nodes_, cache_, pane, 100000.0f, TextSelection{});

    const auto draw_lines = std::ranges::count_if(cmds, [](const auto& c) {
        return std::holds_alternative<DrawLineCmd>(c);
    });
    EXPECT_EQ(draw_lines, 0);
}

// ---- 複数 hr のうち先頭だけ可視なケース ----
// 上方向カリングと下方向カリングが両端で同時に効くケース。

TEST_F(CmdGenContentTest, HorizontalRule_PartialViewportLimitsToVisible)
{
    Parse("---\n\n---\n\n---\n\n---\n\n---");
    ASSERT_FALSE(nodes_.empty());

    // 1 本目の hr の下端より少し大きい viewport を用意する。
    const float first_hr_bottom = cache_[0].text_top + cache_[0].height;
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, first_hr_bottom + 1.0f };
    auto& cmds = gen_.GenerateMdPane(nodes_, cache_, pane, 0.0f, TextSelection{});

    const auto draw_lines = std::ranges::count_if(cmds, [](const auto& c) {
        return std::holds_alternative<DrawLineCmd>(c);
    });
    EXPECT_GE(draw_lines, 1) << "1 本目は確実に可視範囲内";
    EXPECT_LT(draw_lines, 5) << "全 5 本は描画されない（後続はカリングされる）";
}
