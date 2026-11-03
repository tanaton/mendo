#include <gtest/gtest.h>
#include "pane_layout.h"
#include "ui_constants.h"
#include <format>
#include <optional>
#include <random>
#include <string>

namespace {

constexpr float SPLITTER_W = 4.0f;

struct LayoutParams {
    float total_w = 1200.0f;
    float total_h = 700.0f;
    float file_w = 220.0f;
    float toc_w = 220.0f;
    bool show_file = true;
    bool show_toc = true;
};

// 幅と表示状態を設定した PaneController で組む。
PaneLayout Layout(const LayoutParams& p = {})
{
    PaneController panes;
    panes.SetSidePaneWidth(PaneTarget::File, p.file_w);
    panes.SetSidePaneWidth(PaneTarget::Toc, p.toc_w);
    panes.SetSidePaneVisible(PaneTarget::File, p.show_file);
    panes.SetSidePaneVisible(PaneTarget::Toc, p.show_toc);
    return panes.ComputeLayout(p.total_w, p.total_h, SPLITTER_W, 0.0f);
}

void ExpectRect(const PaneRect& actual, const PaneRect& expected)
{
    EXPECT_FLOAT_EQ(actual.x, expected.x);
    EXPECT_FLOAT_EQ(actual.y, expected.y);
    EXPECT_FLOAT_EQ(actual.width, expected.width);
    EXPECT_FLOAT_EQ(actual.height, expected.height);
}

// 高さ 500 のペインにヘッダー 32 を載せたときのスクロール情報。内容領域は 32 から 468。
PaneScrollInfo ScrollInfo(float total_content = 1000.0f)
{
    return ComputeScrollInfo(PaneRect{ 0, 0, 220, 500 }, 32.0f, total_content);
}

} // namespace

// ============================================================
// PaneController::ComputeLayout
// ============================================================

// 表示中のサイドペインを左から幅とスプリッタぶんずつ並べ、残りを MD ペインに渡す。隠したペインの矩形は空。
TEST(PaneLayout, PlacesVisiblePanesFromLeft)
{
    struct Case {
        const char* name;
        LayoutParams params;
        PaneRect file;
        PaneRect toc;
        PaneRect md;
    };
    const Case kCases[] = {
        { "Both", {}, { 0, 0, 220, 700 }, { 224, 0, 220, 700 }, { 448, 0, 752, 700 } },
        { "None", { .show_file = false, .show_toc = false }, {}, {}, { 0, 0, 1200, 700 } },
        { "OnlyFile", { .show_toc = false }, { 0, 0, 220, 700 }, {}, { 224, 0, 976, 700 } },
        { "OnlyToc", { .show_file = false }, {}, { 0, 0, 220, 700 }, { 224, 0, 976, 700 } },
        { "Height", { .total_h = 500.0f }, { 0, 0, 220, 500 }, { 224, 0, 220, 500 }, { 448, 0, 752, 500 } },
        { "DifferentWidths", { .file_w = 150.0f, .toc_w = 300.0f }, { 0, 0, 150, 700 }, { 154, 0, 300, 700 }, { 458, 0, 742, 700 } },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        const auto layout = Layout(c.params);
        EXPECT_EQ(layout.show_file, c.params.show_file);
        EXPECT_EQ(layout.show_toc, c.params.show_toc);
        ExpectRect(layout.file_rect, c.file);
        ExpectRect(layout.toc_rect, c.toc);
        ExpectRect(layout.md_rect, c.md);
    }
}

TEST(PaneLayout, MdPaneMinWidth)
{
    // 全幅が小さすぎる: MDペインは少なくとも MD_PANE_MIN_WIDTH であるべき
    for (const float total : { 400.0f, 100.0f, 0.0f }) {
        EXPECT_GE(Layout({ .total_w = total }).md_rect.width, MD_PANE_MIN_WIDTH) << total;
    }
}

// ============================================================
// DetectPaneZone
// ============================================================

// サイドペインは [x, x + 幅)、その右のスプリッタは [x + 幅, x + 幅 + SPLITTER_W)。隠したペインとスプリッタは MD ペインが詰める。
TEST(DetectPaneZone, FollowsPaneAndSplitterRanges)
{
    struct Case {
        const char* name;
        LayoutParams params;
        float x;
        PaneZone expected;
    };
    const Case kCases[] = {
        { "Both", {}, 100.0f, PaneZone::FilePane },
        { "Both", {}, 220.0f, PaneZone::Splitter1 },
        { "Both", {}, 221.0f, PaneZone::Splitter1 },
        { "Both", {}, 223.9f, PaneZone::Splitter1 },
        { "Both", {}, 224.0f, PaneZone::TocPane },
        { "Both", {}, 300.0f, PaneZone::TocPane },
        { "Both", {}, 444.0f, PaneZone::Splitter2 },
        { "Both", {}, 445.0f, PaneZone::Splitter2 },
        { "Both", {}, 500.0f, PaneZone::MdPane },
        { "Both", {}, -10.0f, PaneZone::None },
        { "None", { .show_file = false, .show_toc = false }, 0.0f, PaneZone::MdPane },
        { "None", { .show_file = false, .show_toc = false }, 600.0f, PaneZone::MdPane },
        { "OnlyFile", { .show_toc = false }, 100.0f, PaneZone::FilePane },
        { "OnlyFile", { .show_toc = false }, 221.0f, PaneZone::Splitter1 },
        { "OnlyFile", { .show_toc = false }, 500.0f, PaneZone::MdPane },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::Message() << c.name << " x=" << c.x);
        EXPECT_EQ(DetectPaneZone(c.x, Layout(c.params), SPLITTER_W), c.expected);
    }
}

// ============================================================
// ComputeScrollInfo
// ============================================================

// 収まる内容はスクロールせず、つまみが内容領域を占めて動かない。
TEST(ComputeScrollInfo, FittingContentHasNoMaxScroll)
{
    for (const float total : { 0.0f, 1.0f, 100.0f, 500.0f - 32.0f }) {
        SCOPED_TRACE(total);
        const auto info = ScrollInfo(total);
        EXPECT_FLOAT_EQ(info.content_top, 32.0f);
        EXPECT_FLOAT_EQ(info.content_height, 468.0f);
        EXPECT_FLOAT_EQ(info.total_content, total);
        EXPECT_FLOAT_EQ(info.max_scroll, 0.0f);
        EXPECT_GE(info.thumb_height, PANE_SCROLLBAR_THUMB_MIN);
        EXPECT_FLOAT_EQ(ComputeThumbY(info, 0.0f), info.content_top);
        EXPECT_FLOAT_EQ(ScrollFromThumbY(info, 50.0f), 0.0f);
    }
}

// あふれた分だけスクロールでき、つまみは内容の割合で縮むが最小の高さを下回らない。
TEST(ComputeScrollInfo, ContentOverflows)
{
    const auto info = ScrollInfo(1000.0f);
    EXPECT_FLOAT_EQ(info.content_top, 32.0f);
    EXPECT_FLOAT_EQ(info.content_height, 468.0f);
    EXPECT_FLOAT_EQ(info.max_scroll, 532.0f);
    EXPECT_GT(info.thumb_height, PANE_SCROLLBAR_THUMB_MIN);
    EXPECT_LT(info.thumb_height, info.content_height);

    EXPECT_FLOAT_EQ(ScrollInfo(100000.0f).thumb_height, PANE_SCROLLBAR_THUMB_MIN);
}

// ============================================================
// CenterPaneScrollOnItem (issue#259: アクティブ項目の中央追従)
// ============================================================

// 項目を表示域の中央に置き、先頭・末尾付近では 0..max_scroll に収める。
TEST(CenterPaneScrollOnItem, CentersItemAndClamps)
{
    PaneScrollInfo info{};
    info.content_height = 400.0f;
    info.max_scroll = 600.0f;
    // 25 * 20 - (400 - 20) / 2 = 310
    EXPECT_FLOAT_EQ(CenterPaneScrollOnItem(25, 20.0f, info), 310.0f);
    EXPECT_FLOAT_EQ(CenterPaneScrollOnItem(2, 20.0f, info), 0.0f);
    // 45 * 20 - 190 = 710 > max_scroll
    EXPECT_FLOAT_EQ(CenterPaneScrollOnItem(45, 20.0f, info), 600.0f);
}

// ============================================================
// ComputeThumbY / ScrollFromThumbY
// ============================================================

// スクロール 0..max_scroll はつまみの上端が内容領域の上端からつまみが下端に着く位置までに対応し、互いに逆変換になる。
TEST(PaneScrollThumb, ThumbYAndScrollAreInverse)
{
    const auto info = ScrollInfo();
    const float thumb_bottom_y = info.content_top + info.content_height - info.thumb_height;

    EXPECT_FLOAT_EQ(ComputeThumbY(info, 0.0f), info.content_top);
    EXPECT_NEAR(ComputeThumbY(info, info.max_scroll), thumb_bottom_y, 0.01f);
    const float mid = ComputeThumbY(info, info.max_scroll * 0.5f);
    EXPECT_GT(mid, info.content_top);
    EXPECT_LT(mid, thumb_bottom_y);

    EXPECT_NEAR(ScrollFromThumbY(info, info.content_top), 0.0f, 0.01f);
    EXPECT_NEAR(ScrollFromThumbY(info, thumb_bottom_y), info.max_scroll, 0.01f);
    EXPECT_NEAR(ScrollFromThumbY(info, ComputeThumbY(info, 250.0f)), 250.0f, 0.01f);

    // 内容領域の外へ動かしても 0..max_scroll に収める。
    EXPECT_FLOAT_EQ(ScrollFromThumbY(info, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(ScrollFromThumbY(info, 99999.0f), info.max_scroll);
}

// ---- 追加エッジケース ----

TEST(PaneLayout, OversizedSavedWidthsKeepMdOnScreen)
{
    // 広いモニタで保存した side 幅 (合計がウィンドウ超過) で狭い画面に起動しても、
    // MD ペインとスプリッタは画面内に収まり、操作不能ロックアウトにならない。
    const float total = 700.0f;
    const float md_min = MD_PANE_MIN_WIDTH;
    auto layout = Layout({ .total_w = total, .total_h = 600.0f, .file_w = 2000.0f, .toc_w = 2000.0f });

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
    auto layout = Layout({ .total_w = total, .total_h = 600.0f, .file_w = 5000.0f, .toc_w = 0.0f, .show_toc = false });

    EXPECT_GE(layout.md_rect.width, md_min);
    const float splitter1_x = layout.file_rect.x + layout.file_rect.width;
    EXPECT_LT(splitter1_x, total);
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
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(pane_), 0.0f);
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
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(pane_), 0.0f);

    EXPECT_TRUE(panes_.ScrollSidePaneBy(pane_, 50.0f, 200.0f));
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(pane_), 50.0f);

    EXPECT_TRUE(panes_.ScrollSidePaneBy(pane_, 500.0f, 200.0f));
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(pane_), 200.0f);

    EXPECT_TRUE(panes_.ScrollSidePaneBy(pane_, -500.0f, 200.0f));
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(pane_), 0.0f);
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(other_), 0.0f);
}

// ═══════════════════════════════════════════════
// ペインスクロール
// ═══════════════════════════════════════════════

TEST_F(PaneControllerTest, ResetScrollStates)
{
    panes_.ScrollSidePaneBy(PaneTarget::File, 50.0f, 200.0f);
    panes_.ScrollSidePaneBy(PaneTarget::Toc, 30.0f, 100.0f);
    panes_.ResetScrollStates();
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(PaneTarget::File), 0.0f);
    EXPECT_FLOAT_EQ(panes_.SidePaneScroll(PaneTarget::Toc), 0.0f);
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
// スプリッタードラッグ — 表示幅が縮小されている状態 (保存幅 > ウィンドウ)
// ═══════════════════════════════════════════════

namespace {

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
    const auto layout = panes.ComputeLayout(c.window_w, WINDOW_H, SPLITTER_W, 0.0f);
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
                const auto before = panes->ComputeLayout(c.window_w, WINDOW_H, SPLITTER_W, 0.0f);
                shrunk_cases += (before.file_rect.width < c.file_w) ? 1 : 0;
                const auto& dragged = (target == PaneTarget::File) ? before.file_rect : before.toc_rect;
                SCOPED_TRACE(c.Describe(seed, step) + " " + SplitterName(target));

                panes->DragSplitterTo(target, dragged.x + dragged.width, c.window_w, SPLITTER_W);
                const auto after = panes->ComputeLayout(c.window_w, WINDOW_H, SPLITTER_W, 0.0f);
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
                const auto before = panes->ComputeLayout(c.window_w, WINDOW_H, SPLITTER_W, 0.0f);
                const auto other = (target == PaneTarget::File) ? PaneTarget::Toc : PaneTarget::File;
                SCOPED_TRACE(c.Describe(seed, step) + std::format(" {} dip_x={}", SplitterName(target), dip_x));

                panes->DragSplitterTo(target, dip_x, c.window_w, SPLITTER_W);
                const auto after = panes->ComputeLayout(c.window_w, WINDOW_H, SPLITTER_W, 0.0f);
                EXPECT_NEAR(after.Get(other).width, before.Get(other).width, 1e-3f);
                EXPECT_GE(after.md_rect.width, MD_PANE_MIN_WIDTH - 1e-3f);
                if (HasFailure()) {
                    return;
                }
            }
        }
    }
}
