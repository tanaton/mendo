#include <gtest/gtest.h>
#include "pane_controller.h"

class PaneControllerTest : public ::testing::Test {
protected:
    PaneController panes_;
};

// ═══════════════════════════════════════════════
// 表示状態
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, DefaultVisibility) {
    EXPECT_TRUE(panes_.IsFilePaneVisible());
    EXPECT_TRUE(panes_.IsTocPaneVisible());
}

TEST_F(PaneControllerTest, ToggleFilePane) {
    panes_.ToggleFilePane();
    EXPECT_FALSE(panes_.IsFilePaneVisible());
    panes_.ToggleFilePane();
    EXPECT_TRUE(panes_.IsFilePaneVisible());
}

TEST_F(PaneControllerTest, ToggleTocPane) {
    panes_.ToggleTocPane();
    EXPECT_FALSE(panes_.IsTocPaneVisible());
}

// ═══════════════════════════════════════════════
// 幅
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, DefaultWidths) {
    EXPECT_FLOAT_EQ(panes_.GetFilePaneWidth(), 220.0f);
    EXPECT_FLOAT_EQ(panes_.GetTocPaneWidth(), 220.0f);
}

TEST_F(PaneControllerTest, SetWidthClampedToMin) {
    panes_.SetFilePaneWidth(10.0f);
    EXPECT_GE(panes_.GetFilePaneWidth(), PaneController::PANE_MIN_WIDTH);
}

TEST_F(PaneControllerTest, SetWidthAcceptsLargeValue) {
    panes_.SetTocPaneWidth(500.0f);
    EXPECT_FLOAT_EQ(panes_.GetTocPaneWidth(), 500.0f);
}

// ═══════════════════════════════════════════════
// ペインスクロール
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, ScrollFilePaneByPositive) {
    bool changed = panes_.ScrollFilePaneBy(50.0f, 200.0f);
    EXPECT_TRUE(changed);
    EXPECT_FLOAT_EQ(panes_.FileScroll().scroll_y, 50.0f);
    EXPECT_FLOAT_EQ(panes_.FileScroll().max_scroll, 200.0f);
}

TEST_F(PaneControllerTest, ScrollFilePaneClampsToMax) {
    panes_.ScrollFilePaneBy(500.0f, 200.0f);
    EXPECT_FLOAT_EQ(panes_.FileScroll().scroll_y, 200.0f);
}

TEST_F(PaneControllerTest, ScrollFilePaneClampsToZero) {
    panes_.ScrollFilePaneBy(-50.0f, 200.0f);
    EXPECT_FLOAT_EQ(panes_.FileScroll().scroll_y, 0.0f);
}

TEST_F(PaneControllerTest, ScrollFilePaneNoChangeReturnsFalse) {
    bool changed = panes_.ScrollFilePaneBy(-10.0f, 200.0f);
    EXPECT_FALSE(changed);  // すでに0の位置にいる
}

TEST_F(PaneControllerTest, ScrollTocPaneByPositive) {
    bool changed = panes_.ScrollTocPaneBy(30.0f, 100.0f);
    EXPECT_TRUE(changed);
    EXPECT_FLOAT_EQ(panes_.TocScroll().scroll_y, 30.0f);
}

TEST_F(PaneControllerTest, ResetScrollStates) {
    panes_.ScrollFilePaneBy(50.0f, 200.0f);
    panes_.ScrollTocPaneBy(30.0f, 100.0f);
    panes_.ResetScrollStates();
    EXPECT_FLOAT_EQ(panes_.FileScroll().scroll_y, 0.0f);
    EXPECT_FLOAT_EQ(panes_.TocScroll().scroll_y, 0.0f);
}

// ═══════════════════════════════════════════════
// ホバー
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, HoverDefaultNegativeOne) {
    EXPECT_EQ(panes_.GetHoveredFileIndex(), -1);
    EXPECT_EQ(panes_.GetHoveredTocIndex(), -1);
}

TEST_F(PaneControllerTest, SetHoverReturnsTrueOnChange) {
    EXPECT_TRUE(panes_.SetHoveredFileIndex(3));
    EXPECT_EQ(panes_.GetHoveredFileIndex(), 3);
}

TEST_F(PaneControllerTest, SetHoverReturnsFalseOnSame) {
    panes_.SetHoveredFileIndex(3);
    EXPECT_FALSE(panes_.SetHoveredFileIndex(3));
}

TEST_F(PaneControllerTest, SetHoverTocReturnsTrueOnChange) {
    EXPECT_TRUE(panes_.SetHoveredTocIndex(5));
    EXPECT_EQ(panes_.GetHoveredTocIndex(), 5);
}

// ═══════════════════════════════════════════════
// ドラッグ
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, DragDefaultNone) {
    EXPECT_EQ(panes_.GetDragTarget(), PaneController::DragTarget::None);
}

TEST_F(PaneControllerTest, StartEndDrag) {
    panes_.StartDrag(PaneController::DragTarget::Splitter1);
    EXPECT_EQ(panes_.GetDragTarget(), PaneController::DragTarget::Splitter1);
    panes_.EndDrag();
    EXPECT_EQ(panes_.GetDragTarget(), PaneController::DragTarget::None);
}

TEST_F(PaneControllerTest, DragScrollOffset) {
    panes_.SetDragScrollOffset(12.5f);
    EXPECT_FLOAT_EQ(panes_.GetDragScrollOffset(), 12.5f);
}

// ═══════════════════════════════════════════════
// スプリッタードラッグ制約
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, DragSplitter1RespectsMinWidth) {
    panes_.DragSplitter1To(10.0f, 1200.0f, 4.0f);
    EXPECT_GE(panes_.GetFilePaneWidth(), PaneController::PANE_MIN_WIDTH);
}

TEST_F(PaneControllerTest, DragSplitter1RespectsMinMdWidth) {
    // 両ペイン表示時: file(960) + splitter(4) + toc(220) + splitter(4) = 1188
    // MDペインに残るのは12のみ(< 200)なので、制約されるべき
    panes_.DragSplitter1To(960.0f, 1200.0f, 4.0f);
    float remaining = 1200.0f - panes_.GetFilePaneWidth() - 4.0f - 220.0f - 4.0f;
    EXPECT_GE(remaining, PaneController::MD_PANE_MIN_WIDTH);
}

TEST_F(PaneControllerTest, DragSplitter2RespectsMinWidth) {
    // 目次の左端位置はレイアウトに依存する; 非常に小さくドラッグ
    panes_.DragSplitter2To(panes_.GetFilePaneWidth() + 4.0f + 10.0f, 1200.0f, 4.0f);
    EXPECT_GE(panes_.GetTocPaneWidth(), PaneController::PANE_MIN_WIDTH);
}

TEST_F(PaneControllerTest, DragSplitter2RespectsMinMdWidth) {
    // 目次ペインの幅を非常に大きくドラッグ
    panes_.DragSplitter2To(1190.0f, 1200.0f, 4.0f);
    float layout_width = panes_.GetFilePaneWidth() + 4.0f + panes_.GetTocPaneWidth() + 4.0f;
    float md_width = 1200.0f - layout_width;
    EXPECT_GE(md_width, PaneController::MD_PANE_MIN_WIDTH);
}

// ═══════════════════════════════════════════════
// ズーム
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, ApplyZoomScalesWidths) {
    float old_file = panes_.GetFilePaneWidth();
    float old_toc = panes_.GetTocPaneWidth();
    panes_.ApplyZoom(2.0f);
    EXPECT_FLOAT_EQ(panes_.GetFilePaneWidth(), old_file * 2.0f);
    EXPECT_FLOAT_EQ(panes_.GetTocPaneWidth(), old_toc * 2.0f);
}

TEST_F(PaneControllerTest, ApplyZoomScalesScrollPositions) {
    panes_.ScrollFilePaneBy(50.0f, 200.0f);
    panes_.ApplyZoom(1.5f);
    EXPECT_FLOAT_EQ(panes_.FileScroll().scroll_y, 75.0f);
    EXPECT_FLOAT_EQ(panes_.FileScroll().max_scroll, 300.0f);
}

// ═══════════════════════════════════════════════
// レイアウト計算
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, ComputeLayoutBothPanes) {
    auto layout = panes_.ComputeLayout(1200.0f, 800.0f, 4.0f);
    // ファイルペインはx=0から開始
    EXPECT_FLOAT_EQ(layout.file_rect.x, 0.0f);
    EXPECT_GT(layout.file_rect.width, 0.0f);
    // MDペインが存在すべき
    EXPECT_GT(layout.md_rect.width, 0.0f);
}

TEST_F(PaneControllerTest, ComputeLayoutNoFilePane) {
    panes_.ToggleFilePane();
    auto layout = panes_.ComputeLayout(1200.0f, 800.0f, 4.0f);
    EXPECT_FLOAT_EQ(layout.file_rect.width, 0.0f);
    EXPECT_GT(layout.md_rect.width, 0.0f);
}

TEST_F(PaneControllerTest, DetectZoneMdPane) {
    auto zone = panes_.DetectZone(800.0f, 1200.0f, 800.0f, 4.0f);
    EXPECT_EQ(zone, PaneZone::MdPane);
}

TEST_F(PaneControllerTest, DetectZoneFilePane) {
    auto zone = panes_.DetectZone(10.0f, 1200.0f, 800.0f, 4.0f);
    EXPECT_EQ(zone, PaneZone::FilePane);
}

// ═══════════════════════════════════════════════
// DetectZone — 追加ゾーン
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, DetectZoneSplitter1) {
    // Splitter1はファイルペインの直後
    float file_w = panes_.GetFilePaneWidth(); // 220
    auto zone = panes_.DetectZone(file_w + 2.0f, 1200.0f, 800.0f, 4.0f);
    EXPECT_EQ(zone, PaneZone::Splitter1);
}

TEST_F(PaneControllerTest, DetectZoneTocPane) {
    auto layout = panes_.ComputeLayout(1200.0f, 800.0f, 4.0f);
    float toc_mid = layout.toc_rect.x + layout.toc_rect.width * 0.5f;
    auto zone = panes_.DetectZone(toc_mid, 1200.0f, 800.0f, 4.0f);
    EXPECT_EQ(zone, PaneZone::TocPane);
}

// ═══════════════════════════════════════════════
// スプリッタードラッグ — ファイルペイン非表示時
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, DragSplitter1WithFilePaneHidden) {
    panes_.ToggleFilePane(); // ファイルペインを非表示
    // 非表示のファイルペインでsplitter1をドラッグしても正しくクランプされるべき
    panes_.DragSplitter1To(300.0f, 1200.0f, 4.0f);
    EXPECT_GE(panes_.GetFilePaneWidth(), PaneController::PANE_MIN_WIDTH);
}

TEST_F(PaneControllerTest, DragSplitter2WithTocPaneHidden) {
    panes_.ToggleTocPane(); // 目次ペインを非表示
    auto layout = panes_.ComputeLayout(1200.0f, 800.0f, 4.0f);
    panes_.DragSplitter2To(1000.0f, 1200.0f, 4.0f);
    EXPECT_GE(panes_.GetTocPaneWidth(), PaneController::PANE_MIN_WIDTH);
}

// ═══════════════════════════════════════════════
// ComputeLayout — 各種構成
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, ComputeLayoutNoPanes) {
    panes_.ToggleFilePane();
    panes_.ToggleTocPane();
    auto layout = panes_.ComputeLayout(1200.0f, 800.0f, 4.0f);
    // 全幅がMDペインに割り当てられる
    EXPECT_FLOAT_EQ(layout.file_rect.width, 0.0f);
    EXPECT_FLOAT_EQ(layout.toc_rect.width, 0.0f);
    EXPECT_GT(layout.md_rect.width, 0.0f);
}

TEST_F(PaneControllerTest, ComputeLayoutOnlyTocPane) {
    panes_.ToggleFilePane(); // ファイルペインを非表示
    auto layout = panes_.ComputeLayout(1200.0f, 800.0f, 4.0f);
    EXPECT_FLOAT_EQ(layout.file_rect.width, 0.0f);
    EXPECT_GT(layout.toc_rect.width, 0.0f);
    EXPECT_GT(layout.md_rect.width, 0.0f);
}

TEST_F(PaneControllerTest, ComputeLayoutOnlyFilePane) {
    panes_.ToggleTocPane(); // 目次ペインを非表示
    auto layout = panes_.ComputeLayout(1200.0f, 800.0f, 4.0f);
    EXPECT_GT(layout.file_rect.width, 0.0f);
    EXPECT_FLOAT_EQ(layout.toc_rect.width, 0.0f);
    EXPECT_GT(layout.md_rect.width, 0.0f);
}

// ═══════════════════════════════════════════════
// スクロール — ファイルペインと目次ペインの複合
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, ScrollBothPanesIndependently) {
    panes_.ScrollFilePaneBy(100.0f, 500.0f);
    panes_.ScrollTocPaneBy(50.0f, 300.0f);
    EXPECT_FLOAT_EQ(panes_.FileScroll().scroll_y, 100.0f);
    EXPECT_FLOAT_EQ(panes_.TocScroll().scroll_y, 50.0f);
}

// ═══════════════════════════════════════════════
// ズーム — スケールとスクロールの相互作用
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, ApplyZoomHalf) {
    panes_.ScrollFilePaneBy(100.0f, 500.0f);
    panes_.ScrollTocPaneBy(60.0f, 300.0f);
    float old_file_w = panes_.GetFilePaneWidth();
    panes_.ApplyZoom(0.5f);
    EXPECT_FLOAT_EQ(panes_.GetFilePaneWidth(), old_file_w * 0.5f);
    EXPECT_FLOAT_EQ(panes_.FileScroll().scroll_y, 50.0f);
    EXPECT_FLOAT_EQ(panes_.TocScroll().scroll_y, 30.0f);
}

// ═══════════════════════════════════════════════
// ドラッグターゲットの種類
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, DragTargetAllTypes) {
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
