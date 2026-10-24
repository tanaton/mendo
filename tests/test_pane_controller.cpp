#include <gtest/gtest.h>
#include "pane_controller.h"
#include "ui_constants.h"
#include <format>
#include <optional>
#include <random>
#include <string>

class PaneControllerTest : public ::testing::Test {
protected:
    PaneController panes_;
};

// ═══════════════════════════════════════════════
// 表示状態・幅・スクロール (ペインごとに同じ契約)
// ═══════════════════════════════════════════════

class PaneControllerPerPaneTest : public ::testing::TestWithParam<PaneTarget> {
protected:
    PaneController panes_;
    PaneTarget pane_ = GetParam();
    PaneTarget other_ = GetParam() == PaneTarget::File ? PaneTarget::Toc : PaneTarget::File;
};

INSTANTIATE_TEST_SUITE_P(Panes, PaneControllerPerPaneTest, ::testing::Values(PaneTarget::File, PaneTarget::Toc),
                         [](const ::testing::TestParamInfo<PaneTarget>& info) static {
                             return std::string(info.param == PaneTarget::File ? "File" : "Toc");
                         });

TEST_P(PaneControllerPerPaneTest, DefaultState)
{
    EXPECT_TRUE(panes_.IsSidePaneVisible(pane_));
    EXPECT_FLOAT_EQ(panes_.GetSidePaneWidth(pane_), SIDE_PANE_DEFAULT_WIDTH);
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(pane_).scroll_y, 0.0f);
}

TEST_P(PaneControllerPerPaneTest, ToggleAndSetVisibleAffectOnlyThatPane)
{
    panes_.ToggleSidePane(pane_);
    EXPECT_FALSE(panes_.IsSidePaneVisible(pane_));
    EXPECT_TRUE(panes_.IsSidePaneVisible(other_));
    panes_.ToggleSidePane(pane_);
    EXPECT_TRUE(panes_.IsSidePaneVisible(pane_));

    panes_.SetSidePaneVisible(pane_, false);
    EXPECT_FALSE(panes_.IsSidePaneVisible(pane_));
    EXPECT_TRUE(panes_.IsSidePaneVisible(other_));
    panes_.SetSidePaneVisible(pane_, true);
    EXPECT_TRUE(panes_.IsSidePaneVisible(pane_));
}

TEST_P(PaneControllerPerPaneTest, SetWidthClampsToMin)
{
    panes_.SetSidePaneWidth(pane_, 10.0f);
    EXPECT_FLOAT_EQ(panes_.GetSidePaneWidth(pane_), SIDE_PANE_MIN_WIDTH);
    panes_.SetSidePaneWidth(pane_, 500.0f);
    EXPECT_FLOAT_EQ(panes_.GetSidePaneWidth(pane_), 500.0f);
    EXPECT_FLOAT_EQ(panes_.GetSidePaneWidth(other_), SIDE_PANE_DEFAULT_WIDTH);
}

// 0..max_scroll にクランプし、位置が変わったときだけ true を返す。
TEST_P(PaneControllerPerPaneTest, ScrollByClampsAndReportsChange)
{
    EXPECT_FALSE(panes_.ScrollSidePaneBy(pane_, -10.0f, 200.0f));
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(pane_).scroll_y, 0.0f);

    EXPECT_TRUE(panes_.ScrollSidePaneBy(pane_, 50.0f, 200.0f));
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(pane_).scroll_y, 50.0f);

    EXPECT_TRUE(panes_.ScrollSidePaneBy(pane_, 500.0f, 200.0f));
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(pane_).scroll_y, 200.0f);

    EXPECT_TRUE(panes_.ScrollSidePaneBy(pane_, -500.0f, 200.0f));
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(pane_).scroll_y, 0.0f);
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(other_).scroll_y, 0.0f);
}

// ═══════════════════════════════════════════════
// ペインスクロール
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, ResetScrollStates)
{
    panes_.ScrollSidePaneBy(PaneTarget::File, 50.0f, 200.0f);
    panes_.ScrollSidePaneBy(PaneTarget::Toc, 30.0f, 100.0f);
    panes_.ResetScrollStates();
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(PaneTarget::File).scroll_y, 0.0f);
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(PaneTarget::Toc).scroll_y, 0.0f);
}

// ═══════════════════════════════════════════════
// スプリッタードラッグ制約
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, DragSplitter1RespectsMinWidth)
{
    panes_.DragSplitterTo(PaneTarget::File, 10.0f, 1200.0f, 4.0f);
    EXPECT_GE(panes_.GetSidePaneWidth(PaneTarget::File), SIDE_PANE_MIN_WIDTH);
}

TEST_F(PaneControllerTest, DragSplitter1RespectsMinMdWidth)
{
    // 両ペイン表示時: file(960) + splitter(4) + toc(220) + splitter(4) = 1188
    // MDペインに残るのは12のみ(< 200)なので、制約されるべき
    panes_.DragSplitterTo(PaneTarget::File, 960.0f, 1200.0f, 4.0f);
    float remaining = 1200.0f - panes_.GetSidePaneWidth(PaneTarget::File) - 4.0f - 220.0f - 4.0f;
    EXPECT_GE(remaining, ::MD_PANE_MIN_WIDTH);
}

TEST_F(PaneControllerTest, DragSplitter2RespectsMinWidth)
{
    // 目次の左端位置はレイアウトに依存する; 非常に小さくドラッグ
    panes_.DragSplitterTo(PaneTarget::Toc, panes_.GetSidePaneWidth(PaneTarget::File) + 4.0f + 10.0f, 1200.0f, 4.0f);
    EXPECT_GE(panes_.GetSidePaneWidth(PaneTarget::Toc), SIDE_PANE_MIN_WIDTH);
}

TEST_F(PaneControllerTest, DragSplitter2RespectsMinMdWidth)
{
    // 目次ペインの幅を非常に大きくドラッグ
    panes_.DragSplitterTo(PaneTarget::Toc, 1190.0f, 1200.0f, 4.0f);
    float layout_width = panes_.GetSidePaneWidth(PaneTarget::File) + 4.0f + panes_.GetSidePaneWidth(PaneTarget::Toc) + 4.0f;
    float md_width = 1200.0f - layout_width;
    EXPECT_GE(md_width, ::MD_PANE_MIN_WIDTH);
}

// ═══════════════════════════════════════════════
// スプリッタードラッグ — ファイルペイン非表示時
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, DragSplitter1WithFilePaneHidden)
{
    panes_.ToggleSidePane(PaneTarget::File); // ファイルペインを非表示
    // 非表示のファイルペインでsplitter1をドラッグしても正しくクランプされるべき
    panes_.DragSplitterTo(PaneTarget::File, 300.0f, 1200.0f, 4.0f);
    EXPECT_GE(panes_.GetSidePaneWidth(PaneTarget::File), SIDE_PANE_MIN_WIDTH);
}

TEST_F(PaneControllerTest, DragSplitter2WithTocPaneHidden)
{
    panes_.ToggleSidePane(PaneTarget::Toc); // 目次ペインを非表示
    panes_.DragSplitterTo(PaneTarget::Toc, 1000.0f, 1200.0f, 4.0f);
    EXPECT_GE(panes_.GetSidePaneWidth(PaneTarget::Toc), SIDE_PANE_MIN_WIDTH);
}

// ═══════════════════════════════════════════════
// ComputeLayout — 表示状態の受け渡し
// 幾何計算自体は test_pane_layout.cpp で検証済み。ここでは File/Toc の表示フラグが
// 取り違えずに ComputePaneLayout へ渡ることだけを確認する。
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, ComputeLayoutOnlyTocPane)
{
    panes_.ToggleSidePane(PaneTarget::File); // ファイルペインを非表示
    auto layout = panes_.ComputeLayout(1200.0f, 800.0f, 4.0f);
    EXPECT_FLOAT_EQ(layout.file_rect.width, 0.0f);
    EXPECT_GT(layout.toc_rect.width, 0.0f);
    EXPECT_GT(layout.md_rect.width, 0.0f);
}

TEST_F(PaneControllerTest, ComputeLayoutOnlyFilePane)
{
    panes_.ToggleSidePane(PaneTarget::Toc); // 目次ペインを非表示
    auto layout = panes_.ComputeLayout(1200.0f, 800.0f, 4.0f);
    EXPECT_GT(layout.file_rect.width, 0.0f);
    EXPECT_FLOAT_EQ(layout.toc_rect.width, 0.0f);
    EXPECT_GT(layout.md_rect.width, 0.0f);
}

// ═══════════════════════════════════════════════
// スクロール — ファイルペインと目次ペインの複合
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, ScrollBothPanesIndependently)
{
    panes_.ScrollSidePaneBy(PaneTarget::File, 100.0f, 500.0f);
    panes_.ScrollSidePaneBy(PaneTarget::Toc, 50.0f, 300.0f);
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(PaneTarget::File).scroll_y, 100.0f);
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(PaneTarget::Toc).scroll_y, 50.0f);
}

// ═══════════════════════════════════════════════
// SIDE_PANE_DEFAULT_WIDTH定数
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, DefaultWidthConstant)
{
    EXPECT_FLOAT_EQ(SIDE_PANE_DEFAULT_WIDTH, 220.0f);
    EXPECT_GT(SIDE_PANE_DEFAULT_WIDTH, SIDE_PANE_MIN_WIDTH);
}

// ═══════════════════════════════════════════════
// スプリッタードラッグ — 表示幅が縮小されている状態 (保存幅 > ウィンドウ)
// ═══════════════════════════════════════════════

namespace {

constexpr float SPLITTER_W = 4.0f;
constexpr float WINDOW_H = 600.0f;
// 右端にスプリッタを持つペイン (File = Splitter1, Toc = Splitter2)。
constexpr PaneTarget SPLITTERS[] = { PaneTarget::File, PaneTarget::Toc };

struct DragCase {
    float window_w;
    float file_w;
    float toc_w;

    std::string Describe(int seed, int step) const
    {
        return std::format("seed={} step={} window={} file={} toc={}", seed, step, window_w, file_w, toc_w);
    }
};

// 論理幅の合計がウィンドウを超えて表示幅が縮小されるケースを多めに含む。
DragCase RandomDragCase(std::mt19937& rng)
{
    const float min_window = MD_PANE_MIN_WIDTH + SPLITTER_W * 2.0f + SIDE_PANE_MIN_WIDTH * 2.0f;
    std::uniform_real_distribution<float> window(min_window, 2400.0f);
    std::uniform_real_distribution<float> width(SIDE_PANE_MIN_WIDTH, 1400.0f);
    const float w = window(rng);
    const float f = width(rng);
    const float t = width(rng);
    return { w, f, t };
}

// 表示幅が SIDE_PANE_MIN_WIDTH 未満まで縮んだケースは、ドラッグ側の最小幅制約が優先されるので対象外。
std::optional<PaneController> MakePanes(const DragCase& c)
{
    PaneController panes;
    panes.SetSidePaneWidth(PaneTarget::File, c.file_w);
    panes.SetSidePaneWidth(PaneTarget::Toc, c.toc_w);
    const auto layout = panes.ComputeLayout(c.window_w, WINDOW_H, SPLITTER_W);
    if (layout.file_rect.width < SIDE_PANE_MIN_WIDTH || layout.toc_rect.width < SIDE_PANE_MIN_WIDTH) {
        return std::nullopt;
    }
    return panes;
}

const char* SplitterName(PaneTarget t)
{
    return t == PaneTarget::File ? "Splitter1" : "Splitter2";
}

} // namespace

// 表示中のスプリッタ位置へドラッグ (= 掴んだだけで動かしていない) してもレイアウトは変わらない。
TEST(PaneControllerDragProperty, DragToCurrentSplitterPositionKeepsLayout)
{
    int shrunk_cases = 0;
    for (int seed = 1; seed <= 4; seed++) {
        std::mt19937 rng(seed);
        for (int step = 0; step < 300; step++) {
            const auto c = RandomDragCase(rng);
            for (const auto target : SPLITTERS) {
                auto panes = MakePanes(c);
                if (!panes) {
                    continue;
                }
                const auto before = panes->ComputeLayout(c.window_w, WINDOW_H, SPLITTER_W);
                shrunk_cases += (before.file_rect.width < c.file_w) ? 1 : 0;
                const auto& dragged = (target == PaneTarget::File) ? before.file_rect : before.toc_rect;
                SCOPED_TRACE(c.Describe(seed, step) + " " + SplitterName(target));

                panes->DragSplitterTo(target, dragged.x + dragged.width, c.window_w, SPLITTER_W);
                const auto after = panes->ComputeLayout(c.window_w, WINDOW_H, SPLITTER_W);
                for (const auto t : { PaneTarget::File, PaneTarget::Toc }) {
                    EXPECT_NEAR(after.Get(t).x, before.Get(t).x, 1e-3f);
                    EXPECT_NEAR(after.Get(t).width, before.Get(t).width, 1e-3f);
                }
                EXPECT_NEAR(after.md_rect.x, before.md_rect.x, 1e-3f);
                if (HasFailure()) {
                    return;
                }
            }
        }
    }
    // 縮小表示の分岐を十分に踏んでいること (乱数範囲を変えたときの空振り防止)。
    EXPECT_GT(shrunk_cases, 200);
}

// 片方のスプリッタを動かしても、もう片方のペインの表示幅は変わらない。
TEST(PaneControllerDragProperty, DraggingOneSplitterKeepsOtherPaneWidth)
{
    for (int seed = 1; seed <= 4; seed++) {
        std::mt19937 rng(seed + 100);
        for (int step = 0; step < 300; step++) {
            const auto c = RandomDragCase(rng);
            const float dip_x = std::uniform_real_distribution<float>(0.0f, c.window_w)(rng);
            for (const auto target : SPLITTERS) {
                auto panes = MakePanes(c);
                if (!panes) {
                    continue;
                }
                const auto before = panes->ComputeLayout(c.window_w, WINDOW_H, SPLITTER_W);
                const auto other = (target == PaneTarget::File) ? PaneTarget::Toc : PaneTarget::File;
                SCOPED_TRACE(c.Describe(seed, step) + std::format(" {} dip_x={}", SplitterName(target), dip_x));

                panes->DragSplitterTo(target, dip_x, c.window_w, SPLITTER_W);
                const auto after = panes->ComputeLayout(c.window_w, WINDOW_H, SPLITTER_W);
                EXPECT_NEAR(after.Get(other).width, before.Get(other).width, 1e-3f);
                EXPECT_GE(after.md_rect.width, MD_PANE_MIN_WIDTH - 1e-3f);
                if (HasFailure()) {
                    return;
                }
            }
        }
    }
}
