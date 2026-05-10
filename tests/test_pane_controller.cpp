#include <gtest/gtest.h>
#include "pane_controller.h"
#include "ui_constants.h"

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
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(PaneTarget::File).max_scroll, 200.0f);
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
    panes_.DragSplitter1To(10.0f, 1200.0f, 4.0f);
    EXPECT_GE(panes_.GetSidePaneWidth(PaneTarget::File), PaneController::PANE_MIN_WIDTH);
}

TEST_F(PaneControllerTest, DragSplitter1RespectsMinMdWidth)
{
    // 両ペイン表示時: file(960) + splitter(4) + toc(220) + splitter(4) = 1188
    // MDペインに残るのは12のみ(< 200)なので、制約されるべき
    panes_.DragSplitter1To(960.0f, 1200.0f, 4.0f);
    float remaining = 1200.0f - panes_.GetSidePaneWidth(PaneTarget::File) - 4.0f - 220.0f - 4.0f;
    EXPECT_GE(remaining, ::MD_PANE_MIN_WIDTH);
}

TEST_F(PaneControllerTest, DragSplitter2RespectsMinWidth)
{
    // 目次の左端位置はレイアウトに依存する; 非常に小さくドラッグ
    panes_.DragSplitter2To(panes_.GetSidePaneWidth(PaneTarget::File) + 4.0f + 10.0f, 1200.0f, 4.0f);
    EXPECT_GE(panes_.GetSidePaneWidth(PaneTarget::Toc), PaneController::PANE_MIN_WIDTH);
}

TEST_F(PaneControllerTest, DragSplitter2RespectsMinMdWidth)
{
    // 目次ペインの幅を非常に大きくドラッグ
    panes_.DragSplitter2To(1190.0f, 1200.0f, 4.0f);
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
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(PaneTarget::File).max_scroll, 300.0f);
}

// ═══════════════════════════════════════════════
// レイアウト計算
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, ComputeLayoutBothPanes)
{
    auto layout = panes_.ComputeLayout(1200.0f, 800.0f, 4.0f);
    // ファイルペインはx=0から開始
    EXPECT_FLOAT_EQ(layout.file_rect.x, 0.0f);
    EXPECT_GT(layout.file_rect.width, 0.0f);
    // MDペインが存在すべき
    EXPECT_GT(layout.md_rect.width, 0.0f);
}

TEST_F(PaneControllerTest, ComputeLayoutNoFilePane)
{
    panes_.ToggleSidePane(PaneTarget::File);
    auto layout = panes_.ComputeLayout(1200.0f, 800.0f, 4.0f);
    EXPECT_FLOAT_EQ(layout.file_rect.width, 0.0f);
    EXPECT_GT(layout.md_rect.width, 0.0f);
}

TEST_F(PaneControllerTest, DetectZoneMdPane)
{
    auto zone = panes_.DetectZone(800.0f, 1200.0f, 800.0f, 4.0f);
    EXPECT_EQ(zone, PaneZone::MdPane);
}

TEST_F(PaneControllerTest, DetectZoneFilePane)
{
    auto zone = panes_.DetectZone(10.0f, 1200.0f, 800.0f, 4.0f);
    EXPECT_EQ(zone, PaneZone::FilePane);
}

// ═══════════════════════════════════════════════
// DetectZone — 追加ゾーン
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, DetectZoneSplitter1)
{
    // Splitter1はファイルペインの直後
    float file_w = panes_.GetSidePaneWidth(PaneTarget::File); // 220
    auto zone = panes_.DetectZone(file_w + 2.0f, 1200.0f, 800.0f, 4.0f);
    EXPECT_EQ(zone, PaneZone::Splitter1);
}

TEST_F(PaneControllerTest, DetectZoneTocPane)
{
    auto layout = panes_.ComputeLayout(1200.0f, 800.0f, 4.0f);
    float toc_mid = layout.toc_rect.x + layout.toc_rect.width * 0.5f;
    auto zone = panes_.DetectZone(toc_mid, 1200.0f, 800.0f, 4.0f);
    EXPECT_EQ(zone, PaneZone::TocPane);
}

// ═══════════════════════════════════════════════
// スプリッタードラッグ — ファイルペイン非表示時
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, DragSplitter1WithFilePaneHidden)
{
    panes_.ToggleSidePane(PaneTarget::File); // ファイルペインを非表示
    // 非表示のファイルペインでsplitter1をドラッグしても正しくクランプされるべき
    panes_.DragSplitter1To(300.0f, 1200.0f, 4.0f);
    EXPECT_GE(panes_.GetSidePaneWidth(PaneTarget::File), PaneController::PANE_MIN_WIDTH);
}

TEST_F(PaneControllerTest, DragSplitter2WithTocPaneHidden)
{
    panes_.ToggleSidePane(PaneTarget::Toc); // 目次ペインを非表示
    auto layout = panes_.ComputeLayout(1200.0f, 800.0f, 4.0f);
    panes_.DragSplitter2To(1000.0f, 1200.0f, 4.0f);
    EXPECT_GE(panes_.GetSidePaneWidth(PaneTarget::Toc), PaneController::PANE_MIN_WIDTH);
}

// ═══════════════════════════════════════════════
// ComputeLayout — 各種構成
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, ComputeLayoutNoPanes)
{
    panes_.ToggleSidePane(PaneTarget::File);
    panes_.ToggleSidePane(PaneTarget::Toc);
    auto layout = panes_.ComputeLayout(1200.0f, 800.0f, 4.0f);
    // 全幅がMDペインに割り当てられる
    EXPECT_FLOAT_EQ(layout.file_rect.width, 0.0f);
    EXPECT_FLOAT_EQ(layout.toc_rect.width, 0.0f);
    EXPECT_GT(layout.md_rect.width, 0.0f);
}

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

TEST_F(PaneControllerTest, SetVisibleAffectsLayout)
{
    panes_.SetSidePaneVisible(PaneTarget::File, false);
    auto layout = panes_.ComputeLayout(1200.0f, 800.0f, 4.0f);
    EXPECT_FLOAT_EQ(layout.file_rect.width, 0.0f);
    EXPECT_GT(layout.md_rect.width, 0.0f);
}

// ═══════════════════════════════════════════════
// 閉じるボタンのホバー状態
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, CloseHoverDefaultFalse)
{
    EXPECT_FALSE(panes_.IsSideCloseHovered(PaneTarget::File));
    EXPECT_FALSE(panes_.IsSideCloseHovered(PaneTarget::Toc));
}

TEST_F(PaneControllerTest, SetFileCloseHoveredReturnsTrueOnChange)
{
    EXPECT_TRUE(panes_.SetSideCloseHovered(PaneTarget::File, true));
    EXPECT_TRUE(panes_.IsSideCloseHovered(PaneTarget::File));
}

TEST_F(PaneControllerTest, SetFileCloseHoveredReturnsFalseOnSame)
{
    panes_.SetSideCloseHovered(PaneTarget::File, true);
    EXPECT_FALSE(panes_.SetSideCloseHovered(PaneTarget::File, true));
}

TEST_F(PaneControllerTest, SetFileCloseHoveredReset)
{
    panes_.SetSideCloseHovered(PaneTarget::File, true);
    EXPECT_TRUE(panes_.SetSideCloseHovered(PaneTarget::File, false));
    EXPECT_FALSE(panes_.IsSideCloseHovered(PaneTarget::File));
}

TEST_F(PaneControllerTest, SetTocCloseHoveredReturnsTrueOnChange)
{
    EXPECT_TRUE(panes_.SetSideCloseHovered(PaneTarget::Toc, true));
    EXPECT_TRUE(panes_.IsSideCloseHovered(PaneTarget::Toc));
}

TEST_F(PaneControllerTest, SetTocCloseHoveredReturnsFalseOnSame)
{
    panes_.SetSideCloseHovered(PaneTarget::Toc, true);
    EXPECT_FALSE(panes_.SetSideCloseHovered(PaneTarget::Toc, true));
}

TEST_F(PaneControllerTest, FileAndTocCloseHoverIndependent)
{
    panes_.SetSideCloseHovered(PaneTarget::File, true);
    panes_.SetSideCloseHovered(PaneTarget::Toc, true);
    EXPECT_TRUE(panes_.IsSideCloseHovered(PaneTarget::File));
    EXPECT_TRUE(panes_.IsSideCloseHovered(PaneTarget::Toc));
    panes_.SetSideCloseHovered(PaneTarget::File, false);
    EXPECT_FALSE(panes_.IsSideCloseHovered(PaneTarget::File));
    EXPECT_TRUE(panes_.IsSideCloseHovered(PaneTarget::Toc));
}

// ═══════════════════════════════════════════════
// 更新ボタンのホバー状態
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, RefreshHoverDefaultFalse)
{
    EXPECT_FALSE(panes_.IsSideRefreshHovered(PaneTarget::File));
}

TEST_F(PaneControllerTest, SetFileRefreshHoveredReturnsTrueOnChange)
{
    EXPECT_TRUE(panes_.SetSideRefreshHovered(PaneTarget::File, true));
    EXPECT_TRUE(panes_.IsSideRefreshHovered(PaneTarget::File));
}

TEST_F(PaneControllerTest, SetFileRefreshHoveredReturnsFalseOnSame)
{
    panes_.SetSideRefreshHovered(PaneTarget::File, true);
    EXPECT_FALSE(panes_.SetSideRefreshHovered(PaneTarget::File, true));
}

TEST_F(PaneControllerTest, SetFileRefreshHoveredReset)
{
    panes_.SetSideRefreshHovered(PaneTarget::File, true);
    EXPECT_TRUE(panes_.SetSideRefreshHovered(PaneTarget::File, false));
    EXPECT_FALSE(panes_.IsSideRefreshHovered(PaneTarget::File));
}

TEST_F(PaneControllerTest, RefreshAndCloseHoverIndependent)
{
    panes_.SetSideCloseHovered(PaneTarget::File, true);
    panes_.SetSideRefreshHovered(PaneTarget::File, true);
    EXPECT_TRUE(panes_.IsSideCloseHovered(PaneTarget::File));
    EXPECT_TRUE(panes_.IsSideRefreshHovered(PaneTarget::File));
    panes_.SetSideCloseHovered(PaneTarget::File, false);
    EXPECT_FALSE(panes_.IsSideCloseHovered(PaneTarget::File));
    EXPECT_TRUE(panes_.IsSideRefreshHovered(PaneTarget::File));
}

// ═══════════════════════════════════════════════
// 表示切替時のホバー状態リセット
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, ToggleFilePaneResetsHover)
{
    panes_.SetHoveredSideIndex(PaneTarget::File, 3);
    panes_.SetSideCloseHovered(PaneTarget::File, true);
    panes_.SetSideRefreshHovered(PaneTarget::File, true);
    panes_.ToggleSidePane(PaneTarget::File); // 非表示にする
    EXPECT_EQ(panes_.GetHoveredSideIndex(PaneTarget::File), -1);
    EXPECT_FALSE(panes_.IsSideCloseHovered(PaneTarget::File));
    EXPECT_FALSE(panes_.IsSideRefreshHovered(PaneTarget::File));
}

TEST_F(PaneControllerTest, ToggleTocPaneResetsHover)
{
    panes_.SetHoveredSideIndex(PaneTarget::Toc, 5);
    panes_.SetSideCloseHovered(PaneTarget::Toc, true);
    panes_.ToggleSidePane(PaneTarget::Toc);
    EXPECT_EQ(panes_.GetHoveredSideIndex(PaneTarget::Toc), -1);
    EXPECT_FALSE(panes_.IsSideCloseHovered(PaneTarget::Toc));
}

TEST_F(PaneControllerTest, SetFilePaneVisibleResetsHoverOnChange)
{
    panes_.SetHoveredSideIndex(PaneTarget::File, 2);
    panes_.SetSideCloseHovered(PaneTarget::File, true);
    panes_.SetSideRefreshHovered(PaneTarget::File, true);
    panes_.SetSidePaneVisible(PaneTarget::File, false);
    EXPECT_EQ(panes_.GetHoveredSideIndex(PaneTarget::File), -1);
    EXPECT_FALSE(panes_.IsSideCloseHovered(PaneTarget::File));
    EXPECT_FALSE(panes_.IsSideRefreshHovered(PaneTarget::File));
}

TEST_F(PaneControllerTest, SetFilePaneVisibleNoResetOnSameValue)
{
    panes_.SetHoveredSideIndex(PaneTarget::File, 2);
    panes_.SetSideCloseHovered(PaneTarget::File, true);
    panes_.SetSideRefreshHovered(PaneTarget::File, true);
    panes_.SetSidePaneVisible(PaneTarget::File, true); // 変化なし
    EXPECT_EQ(panes_.GetHoveredSideIndex(PaneTarget::File), 2);
    EXPECT_TRUE(panes_.IsSideCloseHovered(PaneTarget::File));
    EXPECT_TRUE(panes_.IsSideRefreshHovered(PaneTarget::File));
}

TEST_F(PaneControllerTest, SetTocPaneVisibleResetsHoverOnChange)
{
    panes_.SetHoveredSideIndex(PaneTarget::Toc, 4);
    panes_.SetSideCloseHovered(PaneTarget::Toc, true);
    panes_.SetSidePaneVisible(PaneTarget::Toc, false);
    EXPECT_EQ(panes_.GetHoveredSideIndex(PaneTarget::Toc), -1);
    EXPECT_FALSE(panes_.IsSideCloseHovered(PaneTarget::Toc));
}

// ═══════════════════════════════════════════════
// PANE_DEFAULT_WIDTH定数
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, DefaultWidthConstant)
{
    EXPECT_FLOAT_EQ(PaneController::PANE_DEFAULT_WIDTH, 220.0f);
    EXPECT_GT(PaneController::PANE_DEFAULT_WIDTH, PaneController::PANE_MIN_WIDTH);
}
