// CommandGenerator の内容系（Heading 下線 / blockquote 装飾 / カリング境界）。
// MockTextMeasurer 経路で踏める範囲のみ。ハイライトや button 配置は
// IDWriteTextFormat / IDWriteTextLayout が必要なため別ファイル
// (test_command_generator_highlight.cpp) で扱う。
#include <gtest/gtest.h>
#include "cmd_gen_mock_test_base.h"
#include "test_helpers.h"
#include <algorithm>
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
            if (ColorEq(l->color, theme_.hr_color)
                && l->stroke_width == theme_.h2_underline_thickness) {
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
            return l->stroke_width == theme_.blockquote_bar_width
                && ColorEq(l->color, theme_.blockquote_bar_color);
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
    const float first_hr_bottom = cache_[0].y_position + cache_[0].height;
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, first_hr_bottom + 1.0f };
    auto& cmds = gen_.GenerateMdPane(nodes_, cache_, pane, 0.0f, TextSelection{});

    const auto draw_lines = std::ranges::count_if(cmds, [](const auto& c) {
        return std::holds_alternative<DrawLineCmd>(c);
    });
    EXPECT_GE(draw_lines, 1) << "1 本目は確実に可視範囲内";
    EXPECT_LT(draw_lines, 5) << "全 5 本は描画されない（後続はカリングされる）";
}
