#include <gtest/gtest.h>
#include "pane_layout.h"
#include "ui_constants.h"
#include <format>
#include <optional>
#include <random>
#include <string>

namespace {

// 幅と表示状態を設定した PaneController で組む。
PaneLayout Layout(float total_w, float total_h, float file_w, float toc_w, float splitter_w, bool show_file, bool show_toc, float top_offset)
{
    PaneController panes;
    panes.SetSidePaneWidth(PaneTarget::File, file_w);
    panes.SetSidePaneWidth(PaneTarget::Toc, toc_w);
    panes.SetSidePaneVisible(PaneTarget::File, show_file);
    panes.SetSidePaneVisible(PaneTarget::Toc, show_toc);
    return panes.ComputeLayout(total_w, total_h, splitter_w, top_offset);
}

} // namespace

// ============================================================
// PaneController::ComputeLayout
// ============================================================

TEST(PaneLayout, BothPanesVisible)
{
    auto layout = Layout(1200.0f, 700.0f, 220.0f, 220.0f, 4.0f, true, true, 0.0f);

    // ファイルペインはx=0
    EXPECT_FLOAT_EQ(layout.file_rect.x, 0.0f);
    EXPECT_FLOAT_EQ(layout.file_rect.width, 220.0f);
    EXPECT_FLOAT_EQ(layout.file_rect.height, 700.0f);

    // 目次ペインはファイルペイン + スプリッターの後
    EXPECT_FLOAT_EQ(layout.toc_rect.x, 224.0f);  // 220 + 4
    EXPECT_FLOAT_EQ(layout.toc_rect.width, 220.0f);

    // MDペインは目次ペイン + スプリッターの後
    EXPECT_FLOAT_EQ(layout.md_rect.x, 448.0f);  // 224 + 220 + 4
    EXPECT_FLOAT_EQ(layout.md_rect.width, 1200.0f - 448.0f);
}

TEST(PaneLayout, NoPanesVisible)
{
    auto layout = Layout(1200.0f, 700.0f, 220.0f, 220.0f, 4.0f, false, false, 0.0f);

    // MDペインが全幅を占める
    EXPECT_FLOAT_EQ(layout.md_rect.x, 0.0f);
    EXPECT_FLOAT_EQ(layout.md_rect.width, 1200.0f);
}

TEST(PaneLayout, OnlyFilePaneVisible)
{
    auto layout = Layout(1200.0f, 700.0f, 220.0f, 220.0f, 4.0f, true, false, 0.0f);

    EXPECT_FLOAT_EQ(layout.file_rect.x, 0.0f);
    EXPECT_FLOAT_EQ(layout.file_rect.width, 220.0f);

    // 目次ペインはデフォルト(0,0)にあるべき
    EXPECT_FLOAT_EQ(layout.toc_rect.width, 0.0f);

    // MDペインはファイルペイン + スプリッターの後
    EXPECT_FLOAT_EQ(layout.md_rect.x, 224.0f);
}

TEST(PaneLayout, OnlyTocPaneVisible)
{
    auto layout = Layout(1200.0f, 700.0f, 220.0f, 220.0f, 4.0f, false, true, 0.0f);

    EXPECT_FLOAT_EQ(layout.file_rect.width, 0.0f);
    EXPECT_FLOAT_EQ(layout.toc_rect.x, 0.0f);
    EXPECT_FLOAT_EQ(layout.toc_rect.width, 220.0f);
    EXPECT_FLOAT_EQ(layout.md_rect.x, 224.0f);
}

TEST(PaneLayout, MdPaneMinWidth)
{
    // 全幅が小さすぎる: MDペインは少なくとも MD_PANE_MIN_WIDTH であるべき
    auto layout = Layout(400.0f, 700.0f, 220.0f, 220.0f, 4.0f, true, true, 0.0f);

    EXPECT_GE(layout.md_rect.width, MD_PANE_MIN_WIDTH);
}

TEST(PaneLayout, HeightPassedThrough)
{
    auto layout = Layout(1200.0f, 500.0f, 220.0f, 220.0f, 4.0f, true, true, 0.0f);

    EXPECT_FLOAT_EQ(layout.file_rect.height, 500.0f);
    EXPECT_FLOAT_EQ(layout.toc_rect.height, 500.0f);
    EXPECT_FLOAT_EQ(layout.md_rect.height, 500.0f);
}

TEST(PaneLayout, DifferentPaneWidths)
{
    auto layout = Layout(1200.0f, 700.0f, 150.0f, 300.0f, 4.0f, true, true, 0.0f);

    EXPECT_FLOAT_EQ(layout.file_rect.width, 150.0f);
    EXPECT_FLOAT_EQ(layout.toc_rect.width, 300.0f);
    EXPECT_FLOAT_EQ(layout.toc_rect.x, 154.0f);  // 150 + 4
    EXPECT_FLOAT_EQ(layout.md_rect.x, 458.0f);  // 154 + 300 + 4
}

// ============================================================
// DetectPaneZone
// ============================================================

TEST(DetectPaneZone, BothPanes)
{
    auto layout = Layout(1200.0f, 700.0f, 220.0f, 220.0f, 4.0f, true, true, 0.0f);

    // ファイルペイン内
    EXPECT_EQ(DetectPaneZone(100.0f, layout, 4.0f), PaneZone::FilePane);
    // スプリッター1上
    EXPECT_EQ(DetectPaneZone(221.0f, layout, 4.0f), PaneZone::Splitter1);
    // 目次ペイン内
    EXPECT_EQ(DetectPaneZone(300.0f, layout, 4.0f), PaneZone::TocPane);
    // スプリッター2上
    EXPECT_EQ(DetectPaneZone(445.0f, layout, 4.0f), PaneZone::Splitter2);
    // MDペイン内
    EXPECT_EQ(DetectPaneZone(500.0f, layout, 4.0f), PaneZone::MdPane);
}

TEST(DetectPaneZone, NoPanes)
{
    auto layout = Layout(1200.0f, 700.0f, 220.0f, 220.0f, 4.0f, false, false, 0.0f);

    EXPECT_EQ(DetectPaneZone(0.0f, layout, 4.0f), PaneZone::MdPane);
    EXPECT_EQ(DetectPaneZone(600.0f, layout, 4.0f), PaneZone::MdPane);
}

TEST(DetectPaneZone, OnlyFilePaneHidesToc)
{
    auto layout = Layout(1200.0f, 700.0f, 220.0f, 220.0f, 4.0f, true, false, 0.0f);

    EXPECT_EQ(DetectPaneZone(100.0f, layout, 4.0f), PaneZone::FilePane);
    EXPECT_EQ(DetectPaneZone(221.0f, layout, 4.0f), PaneZone::Splitter1);
    EXPECT_EQ(DetectPaneZone(500.0f, layout, 4.0f), PaneZone::MdPane);
}

TEST(DetectPaneZone, NegativeX)
{
    auto layout = Layout(1200.0f, 700.0f, 220.0f, 220.0f, 4.0f, true, true, 0.0f);

    EXPECT_EQ(DetectPaneZone(-10.0f, layout, 4.0f), PaneZone::None);
}

// ============================================================
// ComputeScrollInfo
// ============================================================

TEST(ComputeScrollInfo, ContentFits)
{
    PaneRect rect{ 0, 0, 220, 500 };
    auto info = ComputeScrollInfo(rect, 32.0f, 100.0f);

    EXPECT_FLOAT_EQ(info.content_top, 32.0f);
    EXPECT_FLOAT_EQ(info.content_height, 468.0f);
    EXPECT_FLOAT_EQ(info.total_content, 100.0f);
    EXPECT_FLOAT_EQ(info.max_scroll, 0.0f);  // コンテンツが収まる、スクロール不要
}

TEST(ComputeScrollInfo, ContentOverflows)
{
    PaneRect rect{ 0, 0, 220, 500 };
    auto info = ComputeScrollInfo(rect, 32.0f, 1000.0f);

    EXPECT_FLOAT_EQ(info.content_top, 32.0f);
    EXPECT_FLOAT_EQ(info.content_height, 468.0f);
    EXPECT_FLOAT_EQ(info.max_scroll, 532.0f);  // 1000 - 468
    EXPECT_GT(info.thumb_height, 0.0f);
}

TEST(ComputeScrollInfo, ThumbMinimumHeight)
{
    PaneRect rect{ 0, 0, 220, 500 };
    // 非常に大きなコンテンツ - つまみは最小サイズになるべき
    auto info = ComputeScrollInfo(rect, 32.0f, 100000.0f);

    EXPECT_GE(info.thumb_height, PANE_SCROLLBAR_THUMB_MIN);
}

TEST(ComputeScrollInfo, ZeroContent)
{
    PaneRect rect{ 0, 0, 220, 500 };
    auto info = ComputeScrollInfo(rect, 32.0f, 0.0f);

    EXPECT_FLOAT_EQ(info.max_scroll, 0.0f);
}

// ============================================================
// CenterPaneScrollOnItem (issue#259: アクティブ項目の中央追従)
// ============================================================

TEST(CenterPaneScrollOnItem, CentersItemInViewport)
{
    PaneScrollInfo info{};
    info.content_height = 400.0f;
    info.max_scroll = 600.0f;

    // 25 * 20 - (400 - 20) / 2 = 310
    EXPECT_FLOAT_EQ(CenterPaneScrollOnItem(25, 20.0f, info), 310.0f);
}

TEST(CenterPaneScrollOnItem, ClampsAtTop)
{
    PaneScrollInfo info{};
    info.content_height = 400.0f;
    info.max_scroll = 600.0f;

    // 先頭付近の項目は中央に置けないので 0 で止める
    EXPECT_FLOAT_EQ(CenterPaneScrollOnItem(2, 20.0f, info), 0.0f);
}

TEST(CenterPaneScrollOnItem, ClampsAtBottom)
{
    PaneScrollInfo info{};
    info.content_height = 400.0f;
    info.max_scroll = 600.0f;

    // 45 * 20 - 190 = 710 > max_scroll → 末尾で止める
    EXPECT_FLOAT_EQ(CenterPaneScrollOnItem(45, 20.0f, info), 600.0f);
}

// ============================================================
// ComputeThumbY
// ============================================================

TEST(ComputeThumbY, AtTop)
{
    PaneRect rect{ 0, 0, 220, 500 };
    auto info = ComputeScrollInfo(rect, 32.0f, 1000.0f);

    float thumb_y = ComputeThumbY(info, 0.0f);
    EXPECT_FLOAT_EQ(thumb_y, info.content_top);
}

TEST(ComputeThumbY, AtBottom)
{
    PaneRect rect{ 0, 0, 220, 500 };
    auto info = ComputeScrollInfo(rect, 32.0f, 1000.0f);

    float thumb_y = ComputeThumbY(info, info.max_scroll);
    // つまみの下端がコンテンツの下端と揃うべき
    EXPECT_NEAR(thumb_y + info.thumb_height, info.content_top + info.content_height, 0.01f);
}

TEST(ComputeThumbY, AtMiddle)
{
    PaneRect rect{ 0, 0, 220, 500 };
    auto info = ComputeScrollInfo(rect, 32.0f, 1000.0f);

    float thumb_y_top = ComputeThumbY(info, 0.0f);
    float thumb_y_bot = ComputeThumbY(info, info.max_scroll);
    float thumb_y_mid = ComputeThumbY(info, info.max_scroll * 0.5f);

    EXPECT_GT(thumb_y_mid, thumb_y_top);
    EXPECT_LT(thumb_y_mid, thumb_y_bot);
}

// ============================================================
// ScrollFromThumbY
// ============================================================

TEST(ScrollFromThumbY, AtTop)
{
    PaneRect rect{ 0, 0, 220, 500 };
    auto info = ComputeScrollInfo(rect, 32.0f, 1000.0f);

    float scroll = ScrollFromThumbY(info, info.content_top);
    EXPECT_NEAR(scroll, 0.0f, 0.01f);
}

TEST(ScrollFromThumbY, AtBottom)
{
    PaneRect rect{ 0, 0, 220, 500 };
    auto info = ComputeScrollInfo(rect, 32.0f, 1000.0f);

    float thumb_bottom_y = info.content_top + info.content_height - info.thumb_height;
    float scroll = ScrollFromThumbY(info, thumb_bottom_y);
    EXPECT_NEAR(scroll, info.max_scroll, 0.01f);
}

TEST(ScrollFromThumbY, RoundTrip)
{
    PaneRect rect{ 0, 0, 220, 500 };
    auto info = ComputeScrollInfo(rect, 32.0f, 1000.0f);

    // スクロール → つまみY → スクロール のラウンドトリップ
    float original_scroll = 250.0f;
    float thumb_y = ComputeThumbY(info, original_scroll);
    float recovered_scroll = ScrollFromThumbY(info, thumb_y);
    EXPECT_NEAR(recovered_scroll, original_scroll, 0.01f);
}

TEST(ScrollFromThumbY, ClampsBelowContent)
{
    PaneRect rect{ 0, 0, 220, 500 };
    auto info = ComputeScrollInfo(rect, 32.0f, 1000.0f);

    // つまみYがcontent_topより上の場合は0にクランプされるべき
    float scroll = ScrollFromThumbY(info, 0.0f);
    EXPECT_FLOAT_EQ(scroll, 0.0f);
}

TEST(ScrollFromThumbY, ClampsAboveContent)
{
    PaneRect rect{ 0, 0, 220, 500 };
    auto info = ComputeScrollInfo(rect, 32.0f, 1000.0f);

    // つまみYが大幅に下の場合はmax_scrollにクランプされるべき
    float scroll = ScrollFromThumbY(info, 99999.0f);
    EXPECT_FLOAT_EQ(scroll, info.max_scroll);
}

// ---- 追加エッジケース ----

TEST(PaneLayout, ZeroWidth)
{
    auto layout = Layout(0.0f, 600.0f, 220.0f, 220.0f, 4.0f, true, true, 0.0f);
    // MDペインはmd_min_widthを使用すべき
    EXPECT_GE(layout.md_rect.width, 200.0f);
}

TEST(PaneLayout, VeryNarrowWindow)
{
    auto layout = Layout(100.0f, 600.0f, 220.0f, 220.0f, 4.0f, true, true, 0.0f);
    // MDペインは最小幅を下回らないべき
    EXPECT_GE(layout.md_rect.width, 200.0f);
}

TEST(PaneLayout, OversizedSavedWidthsKeepMdOnScreen)
{
    // 広いモニタで保存した side 幅 (合計がウィンドウ超過) で狭い画面に起動しても、
    // MD ペインとスプリッタは画面内に収まり、操作不能ロックアウトにならない。
    const float total = 700.0f;
    const float md_min = MD_PANE_MIN_WIDTH;
    auto layout = Layout(total, 600.0f, 2000.0f, 2000.0f, 4.0f, true, true, 0.0f);

    EXPECT_GE(layout.md_rect.width, md_min);
    // MD ペイン左端が画面内に収まる (md_min 幅を確保できる位置)
    EXPECT_LE(layout.md_rect.x, total - md_min);
    // 両スプリッタが画面内に残る (ドラッグで回復可能)
    const float splitter1_x = layout.file_rect.x + layout.file_rect.width;
    const float splitter2_x = layout.toc_rect.x + layout.toc_rect.width;
    EXPECT_LT(splitter1_x, total);
    EXPECT_LT(splitter2_x, total);
}

TEST(PaneLayout, SingleOversizedPaneKeepsSplitterOnScreen)
{
    // 片ペインの保存幅だけでウィンドウを超える場合でも、そのスプリッタは画面内に残る。
    const float total = 600.0f;
    const float md_min = MD_PANE_MIN_WIDTH;
    auto layout = Layout(total, 600.0f, 5000.0f, 0.0f, 4.0f, true, false, 0.0f);

    EXPECT_GE(layout.md_rect.width, md_min);
    const float splitter1_x = layout.file_rect.x + layout.file_rect.width;
    EXPECT_LT(splitter1_x, total);
}

TEST(DetectPaneZone, ExactlySplitter1Edge)
{
    auto layout = Layout(1200.0f, 600.0f, 220.0f, 220.0f, 4.0f, true, true, 0.0f);
    float splitter1_x = layout.file_rect.x + layout.file_rect.width;
    EXPECT_EQ(DetectPaneZone(splitter1_x, layout, 4.0f), PaneZone::Splitter1);
    EXPECT_EQ(DetectPaneZone(splitter1_x + 3.9f, layout, 4.0f), PaneZone::Splitter1);
    EXPECT_EQ(DetectPaneZone(splitter1_x + 4.0f, layout, 4.0f), PaneZone::TocPane);
}

TEST(DetectPaneZone, ExactlySplitter2Edge)
{
    auto layout = Layout(1200.0f, 600.0f, 220.0f, 220.0f, 4.0f, true, true, 0.0f);
    float splitter2_x = layout.toc_rect.x + layout.toc_rect.width;
    EXPECT_EQ(DetectPaneZone(splitter2_x, layout, 4.0f), PaneZone::Splitter2);
}

TEST(ComputeScrollInfo, VerySmallContent)
{
    PaneRect rect{ 0, 0, 220, 500 };
    auto info = ComputeScrollInfo(rect, 32.0f, 1.0f);
    EXPECT_FLOAT_EQ(info.max_scroll, 0.0f);
    EXPECT_GE(info.thumb_height, PANE_SCROLLBAR_THUMB_MIN);
}

TEST(ComputeScrollInfo, ExactFit)
{
    PaneRect rect{ 0, 0, 220, 500 };
    float content_height = 500.0f - 32.0f; // ちょうど収まる
    auto info = ComputeScrollInfo(rect, 32.0f, content_height);
    EXPECT_FLOAT_EQ(info.max_scroll, 0.0f);
}

TEST(ComputeThumbY, ZeroMaxScroll)
{
    PaneRect rect{ 0, 0, 220, 500 };
    auto info = ComputeScrollInfo(rect, 32.0f, 100.0f); // コンテンツが収まる
    float thumb_y = ComputeThumbY(info, 0.0f);
    EXPECT_FLOAT_EQ(thumb_y, info.content_top);
}

TEST(ScrollFromThumbY, ZeroTrackRange)
{
    PaneRect rect{ 0, 0, 220, 500 };
    // コンテンツがちょうど収まる（またはそれ以下）=> スクロール可能な範囲なし
    auto info = ComputeScrollInfo(rect, 32.0f, 100.0f);
    float scroll = ScrollFromThumbY(info, 50.0f);
    EXPECT_FLOAT_EQ(scroll, 0.0f);
}

// ============================================================
// SidePaneLocalY
// ============================================================

TEST(SidePaneLocalY, OffsetsByContentTopAndScroll)
{
    // ペイン上端 32 + ヘッダー 32 の直下は、スクロール 0 でローカル 0、スクロール 40 でローカル 40。
    EXPECT_FLOAT_EQ(SidePaneLocalY(64.0f, 64.0f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(SidePaneLocalY(64.0f, 64.0f, 40.0f), 40.0f);
    EXPECT_FLOAT_EQ(SidePaneLocalY(100.0f, 64.0f, 10.0f), 46.0f);
}

// ============================================================
// HitTestUniformList
// ============================================================

TEST(HitTestUniformList, ReturnsIndexOfItemUnderY)
{
    EXPECT_EQ(HitTestUniformList(0.0f, 20.0f, 3), 0);
    EXPECT_EQ(HitTestUniformList(19.9f, 20.0f, 3), 0);
    EXPECT_EQ(HitTestUniformList(20.0f, 20.0f, 3), 1);
    EXPECT_EQ(HitTestUniformList(59.9f, 20.0f, 3), 2);
}

TEST(HitTestUniformList, OutOfRangeReturnsMinusOne)
{
    EXPECT_EQ(HitTestUniformList(-0.1f, 20.0f, 3), -1);
    EXPECT_EQ(HitTestUniformList(60.0f, 20.0f, 3), -1);
    EXPECT_EQ(HitTestUniformList(0.0f, 20.0f, 0), -1);
}

// 項目高さが未確定 (0) のときに 0 除算しない。
TEST(HitTestUniformList, NonPositiveItemHeightReturnsMinusOne)
{
    EXPECT_EQ(HitTestUniformList(10.0f, 0.0f, 3), -1);
    EXPECT_EQ(HitTestUniformList(10.0f, -5.0f, 3), -1);
}

// ============================================================
// ToPaneTarget
// ============================================================

TEST(PaneZoneConversion, SidePanesMapToTargets)
{
    EXPECT_EQ(ToPaneTarget(PaneZone::FilePane), PaneTarget::File);
    EXPECT_EQ(ToPaneTarget(PaneZone::TocPane), PaneTarget::Toc);
}

TEST(PaneZoneConversion, NonSidePaneZonesHaveNoTarget)
{
    for (const auto zone : { PaneZone::None, PaneZone::Splitter1, PaneZone::Splitter2, PaneZone::MdPane }) {
        EXPECT_FALSE(ToPaneTarget(zone).has_value());
    }
}

// 描画とヒット判定はこの判定を共有する。つまみが動かせないほど低いペインでは出さない。
TEST(ShowsVScrollbar, MatchesThumbMobility)
{
    EXPECT_TRUE(ShowsVScrollbar(ComputeScrollInfo({ 0.0f, 0.0f, 200.0f, 432.0f }, 32.0f, 1000.0f), false));
    EXPECT_FALSE(ShowsVScrollbar(ComputeScrollInfo({ 0.0f, 0.0f, 200.0f, 432.0f }, 32.0f, 400.0f), false));
    // 内容領域がつまみの最小高より低いと、つまみがトラック全体を占めて動かせない。
    EXPECT_FALSE(ShowsVScrollbar(ComputeScrollInfo({ 0.0f, 0.0f, 200.0f, 52.0f }, 32.0f, 1000.0f), false));
}

// 計測待ちで内容が伸びうる間は、ちょうど収まっていても出し続ける。
TEST(ShowsVScrollbar, KeepsShowingWhileContentMayGrow)
{
    const auto info = ComputeScrollInfo({ 0.0f, 0.0f, 600.0f, 400.0f }, 0.0f, 400.0f);
    EXPECT_FALSE(ShowsVScrollbar(info, false));
    EXPECT_TRUE(ShowsVScrollbar(info, true));
}

// ═══════════════════════════════════════════════
// PaneController
// ═══════════════════════════════════════════════

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
// ComputeLayout — 表示状態の反映
// 幾何計算自体は上の PaneLayout のテストで検証済み。ここでは File/Toc の表示フラグを
// 取り違えずに反映し、ゾーン判定のために PaneLayout へ写すことだけを確認する。
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, ComputeLayoutOnlyTocPane)
{
    panes_.ToggleSidePane(PaneTarget::File); // ファイルペインを非表示
    auto layout = panes_.ComputeLayout(1200.0f, 800.0f, 4.0f);
    EXPECT_FALSE(layout.show_file);
    EXPECT_TRUE(layout.show_toc);
    EXPECT_FLOAT_EQ(layout.file_rect.width, 0.0f);
    EXPECT_GT(layout.toc_rect.width, 0.0f);
    EXPECT_GT(layout.md_rect.width, 0.0f);
}

TEST_F(PaneControllerTest, ComputeLayoutOnlyFilePane)
{
    panes_.ToggleSidePane(PaneTarget::Toc); // 目次ペインを非表示
    auto layout = panes_.ComputeLayout(1200.0f, 800.0f, 4.0f);
    EXPECT_TRUE(layout.show_file);
    EXPECT_FALSE(layout.show_toc);
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
