#include <gtest/gtest.h>
#include "pane_layout.h"

// ============================================================
// ComputePaneLayout
// ============================================================

TEST(PaneLayout, BothPanesVisible)
{
    auto layout = ComputePaneLayout(1200.0f, 700.0f, 220.0f, 220.0f, 4.0f, true, true);

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
    auto layout = ComputePaneLayout(1200.0f, 700.0f, 220.0f, 220.0f, 4.0f, false, false);

    // MDペインが全幅を占める
    EXPECT_FLOAT_EQ(layout.md_rect.x, 0.0f);
    EXPECT_FLOAT_EQ(layout.md_rect.width, 1200.0f);
}

TEST(PaneLayout, OnlyFilePaneVisible)
{
    auto layout = ComputePaneLayout(1200.0f, 700.0f, 220.0f, 220.0f, 4.0f, true, false);

    EXPECT_FLOAT_EQ(layout.file_rect.x, 0.0f);
    EXPECT_FLOAT_EQ(layout.file_rect.width, 220.0f);

    // 目次ペインはデフォルト(0,0)にあるべき
    EXPECT_FLOAT_EQ(layout.toc_rect.width, 0.0f);

    // MDペインはファイルペイン + スプリッターの後
    EXPECT_FLOAT_EQ(layout.md_rect.x, 224.0f);
}

TEST(PaneLayout, OnlyTocPaneVisible)
{
    auto layout = ComputePaneLayout(1200.0f, 700.0f, 220.0f, 220.0f, 4.0f, false, true);

    EXPECT_FLOAT_EQ(layout.file_rect.width, 0.0f);
    EXPECT_FLOAT_EQ(layout.toc_rect.x, 0.0f);
    EXPECT_FLOAT_EQ(layout.toc_rect.width, 220.0f);
    EXPECT_FLOAT_EQ(layout.md_rect.x, 224.0f);
}

TEST(PaneLayout, MdPaneMinWidth)
{
    // 全幅が小さすぎる: MDペインは少なくともmd_min_widthであるべき
    auto layout = ComputePaneLayout(400.0f, 700.0f, 220.0f, 220.0f, 4.0f, true, true, 200.0f);

    EXPECT_GE(layout.md_rect.width, 200.0f);
}

TEST(PaneLayout, HeightPassedThrough)
{
    auto layout = ComputePaneLayout(1200.0f, 500.0f, 220.0f, 220.0f, 4.0f, true, true);

    EXPECT_FLOAT_EQ(layout.file_rect.height, 500.0f);
    EXPECT_FLOAT_EQ(layout.toc_rect.height, 500.0f);
    EXPECT_FLOAT_EQ(layout.md_rect.height, 500.0f);
}

TEST(PaneLayout, DifferentPaneWidths)
{
    auto layout = ComputePaneLayout(1200.0f, 700.0f, 150.0f, 300.0f, 4.0f, true, true);

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
    auto layout = ComputePaneLayout(1200.0f, 700.0f, 220.0f, 220.0f, 4.0f, true, true);

    // ファイルペイン内
    EXPECT_EQ(DetectPaneZone(100.0f, layout, 4.0f, true, true), PaneZone::FilePane);
    // スプリッター1上
    EXPECT_EQ(DetectPaneZone(221.0f, layout, 4.0f, true, true), PaneZone::Splitter1);
    // 目次ペイン内
    EXPECT_EQ(DetectPaneZone(300.0f, layout, 4.0f, true, true), PaneZone::TocPane);
    // スプリッター2上
    EXPECT_EQ(DetectPaneZone(445.0f, layout, 4.0f, true, true), PaneZone::Splitter2);
    // MDペイン内
    EXPECT_EQ(DetectPaneZone(500.0f, layout, 4.0f, true, true), PaneZone::MdPane);
}

TEST(DetectPaneZone, NoPanes)
{
    auto layout = ComputePaneLayout(1200.0f, 700.0f, 220.0f, 220.0f, 4.0f, false, false);

    EXPECT_EQ(DetectPaneZone(0.0f, layout, 4.0f, false, false), PaneZone::MdPane);
    EXPECT_EQ(DetectPaneZone(600.0f, layout, 4.0f, false, false), PaneZone::MdPane);
}

TEST(DetectPaneZone, OnlyFilePaneHidesToc)
{
    auto layout = ComputePaneLayout(1200.0f, 700.0f, 220.0f, 220.0f, 4.0f, true, false);

    EXPECT_EQ(DetectPaneZone(100.0f, layout, 4.0f, true, false), PaneZone::FilePane);
    EXPECT_EQ(DetectPaneZone(221.0f, layout, 4.0f, true, false), PaneZone::Splitter1);
    EXPECT_EQ(DetectPaneZone(500.0f, layout, 4.0f, true, false), PaneZone::MdPane);
}

TEST(DetectPaneZone, NegativeX)
{
    auto layout = ComputePaneLayout(1200.0f, 700.0f, 220.0f, 220.0f, 4.0f, true, true);

    EXPECT_EQ(DetectPaneZone(-10.0f, layout, 4.0f, true, true), PaneZone::None);
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
    auto info = ComputeScrollInfo(rect, 32.0f, 100000.0f, 24.0f);

    EXPECT_GE(info.thumb_height, 24.0f);
}

TEST(ComputeScrollInfo, ZeroContent)
{
    PaneRect rect{ 0, 0, 220, 500 };
    auto info = ComputeScrollInfo(rect, 32.0f, 0.0f);

    EXPECT_FLOAT_EQ(info.max_scroll, 0.0f);
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

TEST(ComputePaneLayout, ZeroWidth)
{
    auto layout = ComputePaneLayout(0.0f, 600.0f, 220.0f, 220.0f, 4.0f, true, true);
    // MDペインはmd_min_widthを使用すべき
    EXPECT_GE(layout.md_rect.width, 200.0f);
}

TEST(ComputePaneLayout, VeryNarrowWindow)
{
    auto layout = ComputePaneLayout(100.0f, 600.0f, 220.0f, 220.0f, 4.0f, true, true);
    // MDペインは最小幅を下回らないべき
    EXPECT_GE(layout.md_rect.width, 200.0f);
}

TEST(ComputePaneLayout, OversizedSavedWidthsKeepMdOnScreen)
{
    // 広いモニタで保存した side 幅 (合計がウィンドウ超過) で狭い画面に起動しても、
    // MD ペインとスプリッタは画面内に収まり、操作不能ロックアウトにならない。
    const float total = 700.0f;
    const float md_min = 200.0f;
    auto layout = ComputePaneLayout(total, 600.0f, 2000.0f, 2000.0f, 4.0f, true, true, md_min);

    EXPECT_GE(layout.md_rect.width, md_min);
    // MD ペイン左端が画面内に収まる (md_min 幅を確保できる位置)
    EXPECT_LE(layout.md_rect.x, total - md_min);
    // 両スプリッタが画面内に残る (ドラッグで回復可能)
    const float splitter1_x = layout.file_rect.x + layout.file_rect.width;
    const float splitter2_x = layout.toc_rect.x + layout.toc_rect.width;
    EXPECT_LT(splitter1_x, total);
    EXPECT_LT(splitter2_x, total);
}

TEST(ComputePaneLayout, SingleOversizedPaneKeepsSplitterOnScreen)
{
    // 片ペインの保存幅だけでウィンドウを超える場合でも、そのスプリッタは画面内に残る。
    const float total = 600.0f;
    const float md_min = 200.0f;
    auto layout = ComputePaneLayout(total, 600.0f, 5000.0f, 0.0f, 4.0f, true, false, md_min);

    EXPECT_GE(layout.md_rect.width, md_min);
    const float splitter1_x = layout.file_rect.x + layout.file_rect.width;
    EXPECT_LT(splitter1_x, total);
}

TEST(DetectPaneZone, ExactlySplitter1Edge)
{
    auto layout = ComputePaneLayout(1200.0f, 600.0f, 220.0f, 220.0f, 4.0f, true, true);
    float splitter1_x = layout.file_rect.x + layout.file_rect.width;
    EXPECT_EQ(DetectPaneZone(splitter1_x, layout, 4.0f, true, true), PaneZone::Splitter1);
    EXPECT_EQ(DetectPaneZone(splitter1_x + 3.9f, layout, 4.0f, true, true), PaneZone::Splitter1);
    EXPECT_EQ(DetectPaneZone(splitter1_x + 4.0f, layout, 4.0f, true, true), PaneZone::TocPane);
}

TEST(DetectPaneZone, ExactlySplitter2Edge)
{
    auto layout = ComputePaneLayout(1200.0f, 600.0f, 220.0f, 220.0f, 4.0f, true, true);
    float splitter2_x = layout.toc_rect.x + layout.toc_rect.width;
    EXPECT_EQ(DetectPaneZone(splitter2_x, layout, 4.0f, true, true), PaneZone::Splitter2);
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
