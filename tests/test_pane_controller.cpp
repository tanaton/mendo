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
// 表示状態
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, DefaultVisibility)
{
    EXPECT_TRUE(panes_.IsSidePaneVisible(PaneTarget::File));
    EXPECT_TRUE(panes_.IsSidePaneVisible(PaneTarget::Toc));
}

TEST_F(PaneControllerTest, ToggleFilePane)
{
    panes_.ToggleSidePane(PaneTarget::File);
    EXPECT_FALSE(panes_.IsSidePaneVisible(PaneTarget::File));
    panes_.ToggleSidePane(PaneTarget::File);
    EXPECT_TRUE(panes_.IsSidePaneVisible(PaneTarget::File));
}

TEST_F(PaneControllerTest, ToggleTocPane)
{
    panes_.ToggleSidePane(PaneTarget::Toc);
    EXPECT_FALSE(panes_.IsSidePaneVisible(PaneTarget::Toc));
}

// ═══════════════════════════════════════════════
// 幅
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, DefaultWidths)
{
    EXPECT_FLOAT_EQ(panes_.GetSidePaneWidth(PaneTarget::File), PaneController::PANE_DEFAULT_WIDTH);
    EXPECT_FLOAT_EQ(panes_.GetSidePaneWidth(PaneTarget::Toc), PaneController::PANE_DEFAULT_WIDTH);
}

TEST_F(PaneControllerTest, SetWidthClampedToMin)
{
    panes_.SetSidePaneWidth(PaneTarget::File, 10.0f);
    EXPECT_GE(panes_.GetSidePaneWidth(PaneTarget::File), PaneController::PANE_MIN_WIDTH);
}

TEST_F(PaneControllerTest, SetWidthAcceptsLargeValue)
{
    panes_.SetSidePaneWidth(PaneTarget::Toc, 500.0f);
    EXPECT_FLOAT_EQ(panes_.GetSidePaneWidth(PaneTarget::Toc), 500.0f);
}

// ═══════════════════════════════════════════════
// ペインスクロール
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, ScrollFilePaneByPositive)
{
    bool changed = panes_.ScrollSidePaneBy(PaneTarget::File, 50.0f, 200.0f);
    EXPECT_TRUE(changed);
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(PaneTarget::File).scroll_y, 50.0f);
}

TEST_F(PaneControllerTest, ScrollFilePaneClampsToMax)
{
    panes_.ScrollSidePaneBy(PaneTarget::File, 500.0f, 200.0f);
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(PaneTarget::File).scroll_y, 200.0f);
}

TEST_F(PaneControllerTest, ScrollFilePaneClampsToZero)
{
    panes_.ScrollSidePaneBy(PaneTarget::File, -50.0f, 200.0f);
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(PaneTarget::File).scroll_y, 0.0f);
}

TEST_F(PaneControllerTest, ScrollFilePaneNoChangeReturnsFalse)
{
    bool changed = panes_.ScrollSidePaneBy(PaneTarget::File, -10.0f, 200.0f);
    EXPECT_FALSE(changed); // すでに0の位置にいる
}

TEST_F(PaneControllerTest, ScrollTocPaneByPositive)
{
    bool changed = panes_.ScrollSidePaneBy(PaneTarget::Toc, 30.0f, 100.0f);
    EXPECT_TRUE(changed);
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(PaneTarget::Toc).scroll_y, 30.0f);
}

TEST_F(PaneControllerTest, ResetScrollStates)
{
    panes_.ScrollSidePaneBy(PaneTarget::File, 50.0f, 200.0f);
    panes_.ScrollSidePaneBy(PaneTarget::Toc, 30.0f, 100.0f);
    panes_.ResetScrollStates();
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(PaneTarget::File).scroll_y, 0.0f);
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(PaneTarget::Toc).scroll_y, 0.0f);
}

// ═══════════════════════════════════════════════
// ホバー
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, HoverDefaultNegativeOne)
{
    EXPECT_EQ(panes_.GetHoveredSideIndex(PaneTarget::File), -1);
    EXPECT_EQ(panes_.GetHoveredSideIndex(PaneTarget::Toc), -1);
}

TEST_F(PaneControllerTest, SetHoverReturnsTrueOnChange)
{
    EXPECT_TRUE(panes_.SetHoveredSideIndex(PaneTarget::File, 3));
    EXPECT_EQ(panes_.GetHoveredSideIndex(PaneTarget::File), 3);
}

TEST_F(PaneControllerTest, SetHoverReturnsFalseOnSame)
{
    panes_.SetHoveredSideIndex(PaneTarget::File, 3);
    EXPECT_FALSE(panes_.SetHoveredSideIndex(PaneTarget::File, 3));
}

TEST_F(PaneControllerTest, SetHoverTocReturnsTrueOnChange)
{
    EXPECT_TRUE(panes_.SetHoveredSideIndex(PaneTarget::Toc, 5));
    EXPECT_EQ(panes_.GetHoveredSideIndex(PaneTarget::Toc), 5);
}

// ═══════════════════════════════════════════════
// ドラッグ
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, DragDefaultNone)
{
    EXPECT_EQ(panes_.GetDragTarget(), PaneController::DragTarget::None);
}

TEST_F(PaneControllerTest, StartEndDrag)
{
    panes_.StartDrag(PaneController::DragTarget::Splitter1);
    EXPECT_EQ(panes_.GetDragTarget(), PaneController::DragTarget::Splitter1);
    panes_.EndDrag();
    EXPECT_EQ(panes_.GetDragTarget(), PaneController::DragTarget::None);
}

TEST_F(PaneControllerTest, DragScrollOffset)
{
    panes_.SetDragScrollOffset(12.5f);
    EXPECT_FLOAT_EQ(panes_.GetDragScrollOffset(), 12.5f);
}

// ═══════════════════════════════════════════════
// スプリッタードラッグ制約
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, DragSplitter1RespectsMinWidth)
{
    panes_.DragSplitterTo(PaneController::DragTarget::Splitter1, 10.0f, 1200.0f, 4.0f);
    EXPECT_GE(panes_.GetSidePaneWidth(PaneTarget::File), PaneController::PANE_MIN_WIDTH);
}

TEST_F(PaneControllerTest, DragSplitter1RespectsMinMdWidth)
{
    // 両ペイン表示時: file(960) + splitter(4) + toc(220) + splitter(4) = 1188
    // MDペインに残るのは12のみ(< 200)なので、制約されるべき
    panes_.DragSplitterTo(PaneController::DragTarget::Splitter1, 960.0f, 1200.0f, 4.0f);
    float remaining = 1200.0f - panes_.GetSidePaneWidth(PaneTarget::File) - 4.0f - 220.0f - 4.0f;
    EXPECT_GE(remaining, ::MD_PANE_MIN_WIDTH);
}

TEST_F(PaneControllerTest, DragSplitter2RespectsMinWidth)
{
    // 目次の左端位置はレイアウトに依存する; 非常に小さくドラッグ
    panes_.DragSplitterTo(PaneController::DragTarget::Splitter2, panes_.GetSidePaneWidth(PaneTarget::File) + 4.0f + 10.0f, 1200.0f, 4.0f);
    EXPECT_GE(panes_.GetSidePaneWidth(PaneTarget::Toc), PaneController::PANE_MIN_WIDTH);
}

TEST_F(PaneControllerTest, DragSplitter2RespectsMinMdWidth)
{
    // 目次ペインの幅を非常に大きくドラッグ
    panes_.DragSplitterTo(PaneController::DragTarget::Splitter2, 1190.0f, 1200.0f, 4.0f);
    float layout_width = panes_.GetSidePaneWidth(PaneTarget::File) + 4.0f + panes_.GetSidePaneWidth(PaneTarget::Toc) + 4.0f;
    float md_width = 1200.0f - layout_width;
    EXPECT_GE(md_width, ::MD_PANE_MIN_WIDTH);
}

// ═══════════════════════════════════════════════
// ズーム
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, ApplyZoomScalesWidths)
{
    float old_file = panes_.GetSidePaneWidth(PaneTarget::File);
    float old_toc = panes_.GetSidePaneWidth(PaneTarget::Toc);
    panes_.ApplyZoom(2.0f);
    EXPECT_FLOAT_EQ(panes_.GetSidePaneWidth(PaneTarget::File), old_file * 2.0f);
    EXPECT_FLOAT_EQ(panes_.GetSidePaneWidth(PaneTarget::Toc), old_toc * 2.0f);
}

TEST_F(PaneControllerTest, ApplyZoomScalesScrollPositions)
{
    panes_.ScrollSidePaneBy(PaneTarget::File, 50.0f, 200.0f);
    panes_.ApplyZoom(1.5f);
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(PaneTarget::File).scroll_y, 75.0f);
}

// ═══════════════════════════════════════════════
// スプリッタードラッグ — ファイルペイン非表示時
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, DragSplitter1WithFilePaneHidden)
{
    panes_.ToggleSidePane(PaneTarget::File); // ファイルペインを非表示
    // 非表示のファイルペインでsplitter1をドラッグしても正しくクランプされるべき
    panes_.DragSplitterTo(PaneController::DragTarget::Splitter1, 300.0f, 1200.0f, 4.0f);
    EXPECT_GE(panes_.GetSidePaneWidth(PaneTarget::File), PaneController::PANE_MIN_WIDTH);
}

TEST_F(PaneControllerTest, DragSplitter2WithTocPaneHidden)
{
    panes_.ToggleSidePane(PaneTarget::Toc); // 目次ペインを非表示
    panes_.DragSplitterTo(PaneController::DragTarget::Splitter2, 1000.0f, 1200.0f, 4.0f);
    EXPECT_GE(panes_.GetSidePaneWidth(PaneTarget::Toc), PaneController::PANE_MIN_WIDTH);
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
// ズーム — スケールとスクロールの相互作用
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, ApplyZoomHalf)
{
    panes_.ScrollSidePaneBy(PaneTarget::File, 100.0f, 500.0f);
    panes_.ScrollSidePaneBy(PaneTarget::Toc, 60.0f, 300.0f);
    float old_file_w = panes_.GetSidePaneWidth(PaneTarget::File);
    panes_.ApplyZoom(0.5f);
    EXPECT_FLOAT_EQ(panes_.GetSidePaneWidth(PaneTarget::File), old_file_w * 0.5f);
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(PaneTarget::File).scroll_y, 50.0f);
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(PaneTarget::Toc).scroll_y, 30.0f);
}

// ═══════════════════════════════════════════════
// 極端なズーム — MDペインのコンテンツ幅
// ═══════════════════════════════════════════════

// 500%ズームで両ペイン表示時、MDペインの実効コンテンツ幅が0以下になりうることを検証。
// RequestMermaidRendersはこの状態でスキップする必要がある。
TEST_F(PaneControllerTest, ExtremeZoomMdContentWidthCanBeZeroOrNegative)
{
    // 5倍ズームを模擬: ペイン幅を5倍にする
    panes_.ApplyZoom(5.0f);

    // テーマのマージンも5倍になる（margin_left=40*5=200, margin_right=40*5=200）
    float zoomed_margin_left = 40.0f * 5.0f;
    float zoomed_margin_right = 40.0f * 5.0f;
    float zoomed_splitter = 4.0f * 5.0f;

    // 幅1000pxのウィンドウでレイアウト
    auto layout = panes_.ComputeLayout(1000.0f, 800.0f, zoomed_splitter);
    float md_width = layout.md_rect.width;
    float content_width = md_width - zoomed_margin_left - zoomed_margin_right;

    // MDペインのコンテンツ幅は0以下になりうる
    EXPECT_LE(content_width, 0.0f)
        << "500%%ズーム+小さいウィンドウではcontent_widthは0以下になる";
}

// 元のズームに戻した後、MDペインのコンテンツ幅が正常に復帰することを検証。
TEST_F(PaneControllerTest, ZoomRestoreMdContentWidthPositive)
{
    panes_.ApplyZoom(5.0f);
    panes_.ApplyZoom(1.0f / 5.0f); // 元に戻す

    float margin_left = 40.0f;
    float margin_right = 40.0f;
    float splitter = 4.0f;

    auto layout = panes_.ComputeLayout(1200.0f, 800.0f, splitter);
    float content_width = layout.md_rect.width - margin_left - margin_right;

    EXPECT_GT(content_width, 0.0f)
        << "ズーム復帰後のcontent_widthは正であるべき";
}

// ═══════════════════════════════════════════════
// ドラッグターゲットの種類
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, DragTargetAllTypes)
{
    panes_.StartDrag(PaneController::DragTarget::FileScrollbar);
    EXPECT_EQ(panes_.GetDragTarget(), PaneController::DragTarget::FileScrollbar);
    panes_.EndDrag();

    panes_.StartDrag(PaneController::DragTarget::TocScrollbar);
    EXPECT_EQ(panes_.GetDragTarget(), PaneController::DragTarget::TocScrollbar);
    panes_.EndDrag();

    panes_.StartDrag(PaneController::DragTarget::Splitter2);
    EXPECT_EQ(panes_.GetDragTarget(), PaneController::DragTarget::Splitter2);
    panes_.EndDrag();
    EXPECT_EQ(panes_.GetDragTarget(), PaneController::DragTarget::None);
}

// ═══════════════════════════════════════════════
// 表示状態の直接設定
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, SetFilePaneVisible)
{
    panes_.SetSidePaneVisible(PaneTarget::File, false);
    EXPECT_FALSE(panes_.IsSidePaneVisible(PaneTarget::File));
    panes_.SetSidePaneVisible(PaneTarget::File, true);
    EXPECT_TRUE(panes_.IsSidePaneVisible(PaneTarget::File));
}

TEST_F(PaneControllerTest, SetTocPaneVisible)
{
    panes_.SetSidePaneVisible(PaneTarget::Toc, false);
    EXPECT_FALSE(panes_.IsSidePaneVisible(PaneTarget::Toc));
    panes_.SetSidePaneVisible(PaneTarget::Toc, true);
    EXPECT_TRUE(panes_.IsSidePaneVisible(PaneTarget::Toc));
}

// ═══════════════════════════════════════════════
// ヘッダーボタンのホバー状態
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, HoveredButtonDefaultNone)
{
    EXPECT_EQ(panes_.GetSideHoveredButton(PaneTarget::File), PaneHeaderButton::None);
    EXPECT_EQ(panes_.GetSideHoveredButton(PaneTarget::Toc), PaneHeaderButton::None);
}

TEST_F(PaneControllerTest, SetSideHoveredButtonReturnsTrueOnChange)
{
    EXPECT_TRUE(panes_.SetSideHoveredButton(PaneTarget::File, PaneHeaderButton::Refresh));
    EXPECT_EQ(panes_.GetSideHoveredButton(PaneTarget::File), PaneHeaderButton::Refresh);

    EXPECT_TRUE(panes_.SetSideHoveredButton(PaneTarget::File, PaneHeaderButton::Reveal));
    EXPECT_EQ(panes_.GetSideHoveredButton(PaneTarget::File), PaneHeaderButton::Reveal);
}

TEST_F(PaneControllerTest, SetSideHoveredButtonReturnsFalseOnSame)
{
    panes_.SetSideHoveredButton(PaneTarget::File, PaneHeaderButton::Close);
    EXPECT_FALSE(panes_.SetSideHoveredButton(PaneTarget::File, PaneHeaderButton::Close));
}

TEST_F(PaneControllerTest, ClearSideButtonHover)
{
    panes_.SetSideHoveredButton(PaneTarget::File, PaneHeaderButton::Reveal);
    EXPECT_TRUE(panes_.ClearSideButtonHover(PaneTarget::File));
    EXPECT_EQ(panes_.GetSideHoveredButton(PaneTarget::File), PaneHeaderButton::None);
    EXPECT_FALSE(panes_.ClearSideButtonHover(PaneTarget::File));
}

TEST_F(PaneControllerTest, FileAndTocHoveredButtonIndependent)
{
    panes_.SetSideHoveredButton(PaneTarget::File, PaneHeaderButton::Close);
    panes_.SetSideHoveredButton(PaneTarget::Toc, PaneHeaderButton::Close);
    panes_.ClearSideButtonHover(PaneTarget::File);
    EXPECT_EQ(panes_.GetSideHoveredButton(PaneTarget::File), PaneHeaderButton::None);
    EXPECT_EQ(panes_.GetSideHoveredButton(PaneTarget::Toc), PaneHeaderButton::Close);
}

// ═══════════════════════════════════════════════
// 表示切替時のホバー状態リセット
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, ToggleFilePaneResetsHover)
{
    panes_.SetHoveredSideIndex(PaneTarget::File, 3);
    panes_.SetSideHoveredButton(PaneTarget::File, PaneHeaderButton::Refresh);
    panes_.ToggleSidePane(PaneTarget::File); // 非表示にする
    EXPECT_EQ(panes_.GetHoveredSideIndex(PaneTarget::File), -1);
    EXPECT_EQ(panes_.GetSideHoveredButton(PaneTarget::File), PaneHeaderButton::None);
}

TEST_F(PaneControllerTest, ToggleTocPaneResetsHover)
{
    panes_.SetHoveredSideIndex(PaneTarget::Toc, 5);
    panes_.SetSideHoveredButton(PaneTarget::Toc, PaneHeaderButton::Close);
    panes_.ToggleSidePane(PaneTarget::Toc);
    EXPECT_EQ(panes_.GetHoveredSideIndex(PaneTarget::Toc), -1);
    EXPECT_EQ(panes_.GetSideHoveredButton(PaneTarget::Toc), PaneHeaderButton::None);
}

TEST_F(PaneControllerTest, SetFilePaneVisibleResetsHoverOnChange)
{
    panes_.SetHoveredSideIndex(PaneTarget::File, 2);
    panes_.SetSideHoveredButton(PaneTarget::File, PaneHeaderButton::Reveal);
    panes_.SetSidePaneVisible(PaneTarget::File, false);
    EXPECT_EQ(panes_.GetHoveredSideIndex(PaneTarget::File), -1);
    EXPECT_EQ(panes_.GetSideHoveredButton(PaneTarget::File), PaneHeaderButton::None);
}

TEST_F(PaneControllerTest, SetFilePaneVisibleNoResetOnSameValue)
{
    panes_.SetHoveredSideIndex(PaneTarget::File, 2);
    panes_.SetSideHoveredButton(PaneTarget::File, PaneHeaderButton::Refresh);
    panes_.SetSidePaneVisible(PaneTarget::File, true); // 変化なし
    EXPECT_EQ(panes_.GetHoveredSideIndex(PaneTarget::File), 2);
    EXPECT_EQ(panes_.GetSideHoveredButton(PaneTarget::File), PaneHeaderButton::Refresh);
}

TEST_F(PaneControllerTest, SetTocPaneVisibleResetsHoverOnChange)
{
    panes_.SetHoveredSideIndex(PaneTarget::Toc, 4);
    panes_.SetSideHoveredButton(PaneTarget::Toc, PaneHeaderButton::Close);
    panes_.SetSidePaneVisible(PaneTarget::Toc, false);
    EXPECT_EQ(panes_.GetHoveredSideIndex(PaneTarget::Toc), -1);
    EXPECT_EQ(panes_.GetSideHoveredButton(PaneTarget::Toc), PaneHeaderButton::None);
}

// ═══════════════════════════════════════════════
// PANE_DEFAULT_WIDTH定数
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, DefaultWidthConstant)
{
    EXPECT_FLOAT_EQ(PaneController::PANE_DEFAULT_WIDTH, 220.0f);
    EXPECT_GT(PaneController::PANE_DEFAULT_WIDTH, PaneController::PANE_MIN_WIDTH);
}

// ═══════════════════════════════════════════════
// スプリッタードラッグ — 表示幅が縮小されている状態 (保存幅 > ウィンドウ)
// ═══════════════════════════════════════════════

namespace {

constexpr float SPLITTER_W = 4.0f;
constexpr float WINDOW_H = 600.0f;
constexpr PaneController::DragTarget SPLITTERS[] = { PaneController::DragTarget::Splitter1, PaneController::DragTarget::Splitter2 };

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
    const float min_window = MD_PANE_MIN_WIDTH + SPLITTER_W * 2.0f + PaneController::PANE_MIN_WIDTH * 2.0f;
    std::uniform_real_distribution<float> window(min_window, 2400.0f);
    std::uniform_real_distribution<float> width(PaneController::PANE_MIN_WIDTH, 1400.0f);
    const float w = window(rng);
    const float f = width(rng);
    const float t = width(rng);
    return { w, f, t };
}

// 表示幅が PANE_MIN_WIDTH 未満まで縮んだケースは、ドラッグ側の最小幅制約が優先されるので対象外。
std::optional<PaneController> MakePanes(const DragCase& c)
{
    PaneController panes;
    panes.SetSidePaneWidth(PaneTarget::File, c.file_w);
    panes.SetSidePaneWidth(PaneTarget::Toc, c.toc_w);
    const auto layout = panes.ComputeLayout(c.window_w, WINDOW_H, SPLITTER_W);
    if (layout.file_rect.width < PaneController::PANE_MIN_WIDTH || layout.toc_rect.width < PaneController::PANE_MIN_WIDTH) {
        return std::nullopt;
    }
    return panes;
}

const char* SplitterName(PaneController::DragTarget t)
{
    return t == PaneController::DragTarget::Splitter1 ? "Splitter1" : "Splitter2";
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
                const auto& dragged = (target == PaneController::DragTarget::Splitter1) ? before.file_rect : before.toc_rect;
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
                const auto other = (target == PaneController::DragTarget::Splitter1) ? PaneTarget::Toc : PaneTarget::File;
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
