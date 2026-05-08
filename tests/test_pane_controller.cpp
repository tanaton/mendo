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
    EXPECT_TRUE(panes_.IsFilePaneVisible());
    EXPECT_TRUE(panes_.IsTocPaneVisible());
}

TEST_F(PaneControllerTest, ToggleFilePane)
{
    panes_.ToggleFilePane();
    EXPECT_FALSE(panes_.IsFilePaneVisible());
    panes_.ToggleFilePane();
    EXPECT_TRUE(panes_.IsFilePaneVisible());
}

TEST_F(PaneControllerTest, ToggleTocPane)
{
    panes_.ToggleTocPane();
    EXPECT_FALSE(panes_.IsTocPaneVisible());
}

// ═══════════════════════════════════════════════
// 幅
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, DefaultWidths)
{
    EXPECT_FLOAT_EQ(panes_.GetFilePaneWidth(), PaneController::PANE_DEFAULT_WIDTH);
    EXPECT_FLOAT_EQ(panes_.GetTocPaneWidth(), PaneController::PANE_DEFAULT_WIDTH);
}

TEST_F(PaneControllerTest, SetWidthClampedToMin)
{
    panes_.SetFilePaneWidth(10.0f);
    EXPECT_GE(panes_.GetFilePaneWidth(), PaneController::PANE_MIN_WIDTH);
}

TEST_F(PaneControllerTest, SetWidthAcceptsLargeValue)
{
    panes_.SetTocPaneWidth(500.0f);
    EXPECT_FLOAT_EQ(panes_.GetTocPaneWidth(), 500.0f);
}

// ═══════════════════════════════════════════════
// ペインスクロール
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, ScrollFilePaneByPositive)
{
    bool changed = panes_.ScrollFilePaneBy(50.0f, 200.0f);
    EXPECT_TRUE(changed);
    EXPECT_FLOAT_EQ(panes_.FileScroll().scroll_y, 50.0f);
    EXPECT_FLOAT_EQ(panes_.FileScroll().max_scroll, 200.0f);
}

TEST_F(PaneControllerTest, ScrollFilePaneClampsToMax)
{
    panes_.ScrollFilePaneBy(500.0f, 200.0f);
    EXPECT_FLOAT_EQ(panes_.FileScroll().scroll_y, 200.0f);
}

TEST_F(PaneControllerTest, ScrollFilePaneClampsToZero)
{
    panes_.ScrollFilePaneBy(-50.0f, 200.0f);
    EXPECT_FLOAT_EQ(panes_.FileScroll().scroll_y, 0.0f);
}

TEST_F(PaneControllerTest, ScrollFilePaneNoChangeReturnsFalse)
{
    bool changed = panes_.ScrollFilePaneBy(-10.0f, 200.0f);
    EXPECT_FALSE(changed);  // すでに0の位置にいる
}

TEST_F(PaneControllerTest, ScrollTocPaneByPositive)
{
    bool changed = panes_.ScrollTocPaneBy(30.0f, 100.0f);
    EXPECT_TRUE(changed);
    EXPECT_FLOAT_EQ(panes_.TocScroll().scroll_y, 30.0f);
}

TEST_F(PaneControllerTest, ResetScrollStates)
{
    panes_.ScrollFilePaneBy(50.0f, 200.0f);
    panes_.ScrollTocPaneBy(30.0f, 100.0f);
    panes_.ResetScrollStates();
    EXPECT_FLOAT_EQ(panes_.FileScroll().scroll_y, 0.0f);
    EXPECT_FLOAT_EQ(panes_.TocScroll().scroll_y, 0.0f);
}

// ═══════════════════════════════════════════════
// ホバー
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, HoverDefaultNegativeOne)
{
    EXPECT_EQ(panes_.GetHoveredFileIndex(), -1);
    EXPECT_EQ(panes_.GetHoveredTocIndex(), -1);
}

TEST_F(PaneControllerTest, SetHoverReturnsTrueOnChange)
{
    EXPECT_TRUE(panes_.SetHoveredFileIndex(3));
    EXPECT_EQ(panes_.GetHoveredFileIndex(), 3);
}

TEST_F(PaneControllerTest, SetHoverReturnsFalseOnSame)
{
    panes_.SetHoveredFileIndex(3);
    EXPECT_FALSE(panes_.SetHoveredFileIndex(3));
}

TEST_F(PaneControllerTest, SetHoverTocReturnsTrueOnChange)
{
    EXPECT_TRUE(panes_.SetHoveredTocIndex(5));
    EXPECT_EQ(panes_.GetHoveredTocIndex(), 5);
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
    EXPECT_GE(panes_.GetFilePaneWidth(), PaneController::PANE_MIN_WIDTH);
}

TEST_F(PaneControllerTest, DragSplitter1RespectsMinMdWidth)
{
    // 両ペイン表示時: file(960) + splitter(4) + toc(220) + splitter(4) = 1188
    // MDペインに残るのは12のみ(< 200)なので、制約されるべき
    panes_.DragSplitter1To(960.0f, 1200.0f, 4.0f);
    float remaining = 1200.0f - panes_.GetFilePaneWidth() - 4.0f - 220.0f - 4.0f;
    EXPECT_GE(remaining, ::MD_PANE_MIN_WIDTH);
}

TEST_F(PaneControllerTest, DragSplitter2RespectsMinWidth)
{
    // 目次の左端位置はレイアウトに依存する; 非常に小さくドラッグ
    panes_.DragSplitter2To(panes_.GetFilePaneWidth() + 4.0f + 10.0f, 1200.0f, 4.0f);
    EXPECT_GE(panes_.GetTocPaneWidth(), PaneController::PANE_MIN_WIDTH);
}

TEST_F(PaneControllerTest, DragSplitter2RespectsMinMdWidth)
{
    // 目次ペインの幅を非常に大きくドラッグ
    panes_.DragSplitter2To(1190.0f, 1200.0f, 4.0f);
    float layout_width = panes_.GetFilePaneWidth() + 4.0f + panes_.GetTocPaneWidth() + 4.0f;
    float md_width = 1200.0f - layout_width;
    EXPECT_GE(md_width, ::MD_PANE_MIN_WIDTH);
}

// ═══════════════════════════════════════════════
// ズーム
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, ApplyZoomScalesWidths)
{
    float old_file = panes_.GetFilePaneWidth();
    float old_toc = panes_.GetTocPaneWidth();
    panes_.ApplyZoom(2.0f);
    EXPECT_FLOAT_EQ(panes_.GetFilePaneWidth(), old_file * 2.0f);
    EXPECT_FLOAT_EQ(panes_.GetTocPaneWidth(), old_toc * 2.0f);
}

TEST_F(PaneControllerTest, ApplyZoomScalesScrollPositions)
{
    panes_.ScrollFilePaneBy(50.0f, 200.0f);
    panes_.ApplyZoom(1.5f);
    EXPECT_FLOAT_EQ(panes_.FileScroll().scroll_y, 75.0f);
    EXPECT_FLOAT_EQ(panes_.FileScroll().max_scroll, 300.0f);
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
    panes_.ToggleFilePane();
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
    float file_w = panes_.GetFilePaneWidth(); // 220
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
    panes_.ToggleFilePane(); // ファイルペインを非表示
    // 非表示のファイルペインでsplitter1をドラッグしても正しくクランプされるべき
    panes_.DragSplitter1To(300.0f, 1200.0f, 4.0f);
    EXPECT_GE(panes_.GetFilePaneWidth(), PaneController::PANE_MIN_WIDTH);
}

TEST_F(PaneControllerTest, DragSplitter2WithTocPaneHidden)
{
    panes_.ToggleTocPane(); // 目次ペインを非表示
    auto layout = panes_.ComputeLayout(1200.0f, 800.0f, 4.0f);
    panes_.DragSplitter2To(1000.0f, 1200.0f, 4.0f);
    EXPECT_GE(panes_.GetTocPaneWidth(), PaneController::PANE_MIN_WIDTH);
}

// ═══════════════════════════════════════════════
// ComputeLayout — 各種構成
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, ComputeLayoutNoPanes)
{
    panes_.ToggleFilePane();
    panes_.ToggleTocPane();
    auto layout = panes_.ComputeLayout(1200.0f, 800.0f, 4.0f);
    // 全幅がMDペインに割り当てられる
    EXPECT_FLOAT_EQ(layout.file_rect.width, 0.0f);
    EXPECT_FLOAT_EQ(layout.toc_rect.width, 0.0f);
    EXPECT_GT(layout.md_rect.width, 0.0f);
}

TEST_F(PaneControllerTest, ComputeLayoutOnlyTocPane)
{
    panes_.ToggleFilePane(); // ファイルペインを非表示
    auto layout = panes_.ComputeLayout(1200.0f, 800.0f, 4.0f);
    EXPECT_FLOAT_EQ(layout.file_rect.width, 0.0f);
    EXPECT_GT(layout.toc_rect.width, 0.0f);
    EXPECT_GT(layout.md_rect.width, 0.0f);
}

TEST_F(PaneControllerTest, ComputeLayoutOnlyFilePane)
{
    panes_.ToggleTocPane(); // 目次ペインを非表示
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
    panes_.ScrollFilePaneBy(100.0f, 500.0f);
    panes_.ScrollTocPaneBy(50.0f, 300.0f);
    EXPECT_FLOAT_EQ(panes_.FileScroll().scroll_y, 100.0f);
    EXPECT_FLOAT_EQ(panes_.TocScroll().scroll_y, 50.0f);
}

// ═══════════════════════════════════════════════
// ズーム — スケールとスクロールの相互作用
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, ApplyZoomHalf)
{
    panes_.ScrollFilePaneBy(100.0f, 500.0f);
    panes_.ScrollTocPaneBy(60.0f, 300.0f);
    float old_file_w = panes_.GetFilePaneWidth();
    panes_.ApplyZoom(0.5f);
    EXPECT_FLOAT_EQ(panes_.GetFilePaneWidth(), old_file_w * 0.5f);
    EXPECT_FLOAT_EQ(panes_.FileScroll().scroll_y, 50.0f);
    EXPECT_FLOAT_EQ(panes_.TocScroll().scroll_y, 30.0f);
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
    panes_.ApplyZoom(1.0f / 5.0f);  // 元に戻す

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
    panes_.SetFilePaneVisible(false);
    EXPECT_FALSE(panes_.IsFilePaneVisible());
    panes_.SetFilePaneVisible(true);
    EXPECT_TRUE(panes_.IsFilePaneVisible());
}

TEST_F(PaneControllerTest, SetTocPaneVisible)
{
    panes_.SetTocPaneVisible(false);
    EXPECT_FALSE(panes_.IsTocPaneVisible());
    panes_.SetTocPaneVisible(true);
    EXPECT_TRUE(panes_.IsTocPaneVisible());
}

TEST_F(PaneControllerTest, SetVisibleAffectsLayout)
{
    panes_.SetFilePaneVisible(false);
    auto layout = panes_.ComputeLayout(1200.0f, 800.0f, 4.0f);
    EXPECT_FLOAT_EQ(layout.file_rect.width, 0.0f);
    EXPECT_GT(layout.md_rect.width, 0.0f);
}

// ═══════════════════════════════════════════════
// 閉じるボタンのホバー状態
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, CloseHoverDefaultFalse)
{
    EXPECT_FALSE(panes_.IsFileCloseHovered());
    EXPECT_FALSE(panes_.IsTocCloseHovered());
}

TEST_F(PaneControllerTest, SetFileCloseHoveredReturnsTrueOnChange)
{
    EXPECT_TRUE(panes_.SetFileCloseHovered(true));
    EXPECT_TRUE(panes_.IsFileCloseHovered());
}

TEST_F(PaneControllerTest, SetFileCloseHoveredReturnsFalseOnSame)
{
    panes_.SetFileCloseHovered(true);
    EXPECT_FALSE(panes_.SetFileCloseHovered(true));
}

TEST_F(PaneControllerTest, SetFileCloseHoveredReset)
{
    panes_.SetFileCloseHovered(true);
    EXPECT_TRUE(panes_.SetFileCloseHovered(false));
    EXPECT_FALSE(panes_.IsFileCloseHovered());
}

TEST_F(PaneControllerTest, SetTocCloseHoveredReturnsTrueOnChange)
{
    EXPECT_TRUE(panes_.SetTocCloseHovered(true));
    EXPECT_TRUE(panes_.IsTocCloseHovered());
}

TEST_F(PaneControllerTest, SetTocCloseHoveredReturnsFalseOnSame)
{
    panes_.SetTocCloseHovered(true);
    EXPECT_FALSE(panes_.SetTocCloseHovered(true));
}

TEST_F(PaneControllerTest, FileAndTocCloseHoverIndependent)
{
    panes_.SetFileCloseHovered(true);
    panes_.SetTocCloseHovered(true);
    EXPECT_TRUE(panes_.IsFileCloseHovered());
    EXPECT_TRUE(panes_.IsTocCloseHovered());
    panes_.SetFileCloseHovered(false);
    EXPECT_FALSE(panes_.IsFileCloseHovered());
    EXPECT_TRUE(panes_.IsTocCloseHovered());
}

// ═══════════════════════════════════════════════
// 更新ボタンのホバー状態
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, RefreshHoverDefaultFalse)
{
    EXPECT_FALSE(panes_.IsFileRefreshHovered());
}

TEST_F(PaneControllerTest, SetFileRefreshHoveredReturnsTrueOnChange)
{
    EXPECT_TRUE(panes_.SetFileRefreshHovered(true));
    EXPECT_TRUE(panes_.IsFileRefreshHovered());
}

TEST_F(PaneControllerTest, SetFileRefreshHoveredReturnsFalseOnSame)
{
    panes_.SetFileRefreshHovered(true);
    EXPECT_FALSE(panes_.SetFileRefreshHovered(true));
}

TEST_F(PaneControllerTest, SetFileRefreshHoveredReset)
{
    panes_.SetFileRefreshHovered(true);
    EXPECT_TRUE(panes_.SetFileRefreshHovered(false));
    EXPECT_FALSE(panes_.IsFileRefreshHovered());
}

TEST_F(PaneControllerTest, RefreshAndCloseHoverIndependent)
{
    panes_.SetFileCloseHovered(true);
    panes_.SetFileRefreshHovered(true);
    EXPECT_TRUE(panes_.IsFileCloseHovered());
    EXPECT_TRUE(panes_.IsFileRefreshHovered());
    panes_.SetFileCloseHovered(false);
    EXPECT_FALSE(panes_.IsFileCloseHovered());
    EXPECT_TRUE(panes_.IsFileRefreshHovered());
}

// ═══════════════════════════════════════════════
// 表示切替時のホバー状態リセット
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, ToggleFilePaneResetsHover)
{
    panes_.SetHoveredFileIndex(3);
    panes_.SetFileCloseHovered(true);
    panes_.SetFileRefreshHovered(true);
    panes_.ToggleFilePane(); // 非表示にする
    EXPECT_EQ(panes_.GetHoveredFileIndex(), -1);
    EXPECT_FALSE(panes_.IsFileCloseHovered());
    EXPECT_FALSE(panes_.IsFileRefreshHovered());
}

TEST_F(PaneControllerTest, ToggleTocPaneResetsHover)
{
    panes_.SetHoveredTocIndex(5);
    panes_.SetTocCloseHovered(true);
    panes_.ToggleTocPane();
    EXPECT_EQ(panes_.GetHoveredTocIndex(), -1);
    EXPECT_FALSE(panes_.IsTocCloseHovered());
}

TEST_F(PaneControllerTest, SetFilePaneVisibleResetsHoverOnChange)
{
    panes_.SetHoveredFileIndex(2);
    panes_.SetFileCloseHovered(true);
    panes_.SetFileRefreshHovered(true);
    panes_.SetFilePaneVisible(false);
    EXPECT_EQ(panes_.GetHoveredFileIndex(), -1);
    EXPECT_FALSE(panes_.IsFileCloseHovered());
    EXPECT_FALSE(panes_.IsFileRefreshHovered());
}

TEST_F(PaneControllerTest, SetFilePaneVisibleNoResetOnSameValue)
{
    panes_.SetHoveredFileIndex(2);
    panes_.SetFileCloseHovered(true);
    panes_.SetFileRefreshHovered(true);
    panes_.SetFilePaneVisible(true); // 変化なし
    EXPECT_EQ(panes_.GetHoveredFileIndex(), 2);
    EXPECT_TRUE(panes_.IsFileCloseHovered());
    EXPECT_TRUE(panes_.IsFileRefreshHovered());
}

TEST_F(PaneControllerTest, SetTocPaneVisibleResetsHoverOnChange)
{
    panes_.SetHoveredTocIndex(4);
    panes_.SetTocCloseHovered(true);
    panes_.SetTocPaneVisible(false);
    EXPECT_EQ(panes_.GetHoveredTocIndex(), -1);
    EXPECT_FALSE(panes_.IsTocCloseHovered());
}

// ═══════════════════════════════════════════════
// PANE_DEFAULT_WIDTH定数
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, DefaultWidthConstant)
{
    EXPECT_FLOAT_EQ(PaneController::PANE_DEFAULT_WIDTH, 220.0f);
    EXPECT_GT(PaneController::PANE_DEFAULT_WIDTH, PaneController::PANE_MIN_WIDTH);
}
