#include <gtest/gtest.h>
#include "pane_layout.h"

// ============================================================
// ComputePaneLayout
// ============================================================

TEST(PaneLayout, BothPanesVisible) {
    auto layout = ComputePaneLayout(1200.0f, 700.0f, 220.0f, 220.0f, 4.0f, true, true);

    // File pane at x=0
    EXPECT_FLOAT_EQ(layout.file_rect.x, 0.0f);
    EXPECT_FLOAT_EQ(layout.file_rect.width, 220.0f);
    EXPECT_FLOAT_EQ(layout.file_rect.height, 700.0f);

    // TOC pane after file pane + splitter
    EXPECT_FLOAT_EQ(layout.toc_rect.x, 224.0f);  // 220 + 4
    EXPECT_FLOAT_EQ(layout.toc_rect.width, 220.0f);

    // MD pane after TOC pane + splitter
    EXPECT_FLOAT_EQ(layout.md_rect.x, 448.0f);  // 224 + 220 + 4
    EXPECT_FLOAT_EQ(layout.md_rect.width, 1200.0f - 448.0f);
}

TEST(PaneLayout, NoPanesVisible) {
    auto layout = ComputePaneLayout(1200.0f, 700.0f, 220.0f, 220.0f, 4.0f, false, false);

    // MD pane takes full width
    EXPECT_FLOAT_EQ(layout.md_rect.x, 0.0f);
    EXPECT_FLOAT_EQ(layout.md_rect.width, 1200.0f);
}

TEST(PaneLayout, OnlyFilePaneVisible) {
    auto layout = ComputePaneLayout(1200.0f, 700.0f, 220.0f, 220.0f, 4.0f, true, false);

    EXPECT_FLOAT_EQ(layout.file_rect.x, 0.0f);
    EXPECT_FLOAT_EQ(layout.file_rect.width, 220.0f);

    // TOC pane should be at default (0,0)
    EXPECT_FLOAT_EQ(layout.toc_rect.width, 0.0f);

    // MD pane after file pane + splitter
    EXPECT_FLOAT_EQ(layout.md_rect.x, 224.0f);
}

TEST(PaneLayout, OnlyTocPaneVisible) {
    auto layout = ComputePaneLayout(1200.0f, 700.0f, 220.0f, 220.0f, 4.0f, false, true);

    EXPECT_FLOAT_EQ(layout.file_rect.width, 0.0f);
    EXPECT_FLOAT_EQ(layout.toc_rect.x, 0.0f);
    EXPECT_FLOAT_EQ(layout.toc_rect.width, 220.0f);
    EXPECT_FLOAT_EQ(layout.md_rect.x, 224.0f);
}

TEST(PaneLayout, MdPaneMinWidth) {
    // Total width too small: MD pane should be at least md_min_width
    auto layout = ComputePaneLayout(400.0f, 700.0f, 220.0f, 220.0f, 4.0f, true, true, 200.0f);

    EXPECT_GE(layout.md_rect.width, 200.0f);
}

TEST(PaneLayout, HeightPassedThrough) {
    auto layout = ComputePaneLayout(1200.0f, 500.0f, 220.0f, 220.0f, 4.0f, true, true);

    EXPECT_FLOAT_EQ(layout.file_rect.height, 500.0f);
    EXPECT_FLOAT_EQ(layout.toc_rect.height, 500.0f);
    EXPECT_FLOAT_EQ(layout.md_rect.height, 500.0f);
}

TEST(PaneLayout, DifferentPaneWidths) {
    auto layout = ComputePaneLayout(1200.0f, 700.0f, 150.0f, 300.0f, 4.0f, true, true);

    EXPECT_FLOAT_EQ(layout.file_rect.width, 150.0f);
    EXPECT_FLOAT_EQ(layout.toc_rect.width, 300.0f);
    EXPECT_FLOAT_EQ(layout.toc_rect.x, 154.0f);  // 150 + 4
    EXPECT_FLOAT_EQ(layout.md_rect.x, 458.0f);  // 154 + 300 + 4
}

// ============================================================
// DetectPaneZone
// ============================================================

TEST(DetectPaneZone, BothPanes) {
    auto layout = ComputePaneLayout(1200.0f, 700.0f, 220.0f, 220.0f, 4.0f, true, true);

    // In file pane
    EXPECT_EQ(DetectPaneZone(100.0f, layout, 4.0f, true, true), PaneZone::FilePane);
    // On splitter 1
    EXPECT_EQ(DetectPaneZone(221.0f, layout, 4.0f, true, true), PaneZone::Splitter1);
    // In TOC pane
    EXPECT_EQ(DetectPaneZone(300.0f, layout, 4.0f, true, true), PaneZone::TocPane);
    // On splitter 2
    EXPECT_EQ(DetectPaneZone(445.0f, layout, 4.0f, true, true), PaneZone::Splitter2);
    // In MD pane
    EXPECT_EQ(DetectPaneZone(500.0f, layout, 4.0f, true, true), PaneZone::MdPane);
}

TEST(DetectPaneZone, NoPanes) {
    auto layout = ComputePaneLayout(1200.0f, 700.0f, 220.0f, 220.0f, 4.0f, false, false);

    EXPECT_EQ(DetectPaneZone(0.0f, layout, 4.0f, false, false), PaneZone::MdPane);
    EXPECT_EQ(DetectPaneZone(600.0f, layout, 4.0f, false, false), PaneZone::MdPane);
}

TEST(DetectPaneZone, OnlyFilePaneHidesToc) {
    auto layout = ComputePaneLayout(1200.0f, 700.0f, 220.0f, 220.0f, 4.0f, true, false);

    EXPECT_EQ(DetectPaneZone(100.0f, layout, 4.0f, true, false), PaneZone::FilePane);
    EXPECT_EQ(DetectPaneZone(221.0f, layout, 4.0f, true, false), PaneZone::Splitter1);
    EXPECT_EQ(DetectPaneZone(500.0f, layout, 4.0f, true, false), PaneZone::MdPane);
}

TEST(DetectPaneZone, NegativeX) {
    auto layout = ComputePaneLayout(1200.0f, 700.0f, 220.0f, 220.0f, 4.0f, true, true);

    EXPECT_EQ(DetectPaneZone(-10.0f, layout, 4.0f, true, true), PaneZone::None);
}

// ============================================================
// ComputeScrollInfo
// ============================================================

TEST(ComputeScrollInfo, ContentFits) {
    PaneRect rect{0, 0, 220, 500};
    auto info = ComputeScrollInfo(rect, 32.0f, 100.0f);

    EXPECT_FLOAT_EQ(info.content_top, 32.0f);
    EXPECT_FLOAT_EQ(info.content_height, 468.0f);
    EXPECT_FLOAT_EQ(info.total_content, 100.0f);
    EXPECT_FLOAT_EQ(info.max_scroll, 0.0f);  // Content fits, no scroll needed
}

TEST(ComputeScrollInfo, ContentOverflows) {
    PaneRect rect{0, 0, 220, 500};
    auto info = ComputeScrollInfo(rect, 32.0f, 1000.0f);

    EXPECT_FLOAT_EQ(info.content_top, 32.0f);
    EXPECT_FLOAT_EQ(info.content_height, 468.0f);
    EXPECT_FLOAT_EQ(info.max_scroll, 532.0f);  // 1000 - 468
    EXPECT_GT(info.thumb_height, 0.0f);
}

TEST(ComputeScrollInfo, ThumbMinimumHeight) {
    PaneRect rect{0, 0, 220, 500};
    // Very large content - thumb should be at minimum
    auto info = ComputeScrollInfo(rect, 32.0f, 100000.0f, 24.0f);

    EXPECT_GE(info.thumb_height, 24.0f);
}

TEST(ComputeScrollInfo, ZeroContent) {
    PaneRect rect{0, 0, 220, 500};
    auto info = ComputeScrollInfo(rect, 32.0f, 0.0f);

    EXPECT_FLOAT_EQ(info.max_scroll, 0.0f);
}

// ============================================================
// ComputeThumbY
// ============================================================

TEST(ComputeThumbY, AtTop) {
    PaneRect rect{0, 0, 220, 500};
    auto info = ComputeScrollInfo(rect, 32.0f, 1000.0f);

    float thumb_y = ComputeThumbY(info, 0.0f);
    EXPECT_FLOAT_EQ(thumb_y, info.content_top);
}

TEST(ComputeThumbY, AtBottom) {
    PaneRect rect{0, 0, 220, 500};
    auto info = ComputeScrollInfo(rect, 32.0f, 1000.0f);

    float thumb_y = ComputeThumbY(info, info.max_scroll);
    // Thumb bottom should align with content bottom
    EXPECT_NEAR(thumb_y + info.thumb_height, info.content_top + info.content_height, 0.01f);
}

TEST(ComputeThumbY, AtMiddle) {
    PaneRect rect{0, 0, 220, 500};
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

TEST(ScrollFromThumbY, AtTop) {
    PaneRect rect{0, 0, 220, 500};
    auto info = ComputeScrollInfo(rect, 32.0f, 1000.0f);

    float scroll = ScrollFromThumbY(info, info.content_top);
    EXPECT_NEAR(scroll, 0.0f, 0.01f);
}

TEST(ScrollFromThumbY, AtBottom) {
    PaneRect rect{0, 0, 220, 500};
    auto info = ComputeScrollInfo(rect, 32.0f, 1000.0f);

    float thumb_bottom_y = info.content_top + info.content_height - info.thumb_height;
    float scroll = ScrollFromThumbY(info, thumb_bottom_y);
    EXPECT_NEAR(scroll, info.max_scroll, 0.01f);
}

TEST(ScrollFromThumbY, RoundTrip) {
    PaneRect rect{0, 0, 220, 500};
    auto info = ComputeScrollInfo(rect, 32.0f, 1000.0f);

    // Scroll -> thumb Y -> scroll should round-trip
    float original_scroll = 250.0f;
    float thumb_y = ComputeThumbY(info, original_scroll);
    float recovered_scroll = ScrollFromThumbY(info, thumb_y);
    EXPECT_NEAR(recovered_scroll, original_scroll, 0.01f);
}

TEST(ScrollFromThumbY, ClampsBelowContent) {
    PaneRect rect{0, 0, 220, 500};
    auto info = ComputeScrollInfo(rect, 32.0f, 1000.0f);

    // Thumb Y above content_top should clamp to 0
    float scroll = ScrollFromThumbY(info, 0.0f);
    EXPECT_FLOAT_EQ(scroll, 0.0f);
}

TEST(ScrollFromThumbY, ClampsAboveContent) {
    PaneRect rect{0, 0, 220, 500};
    auto info = ComputeScrollInfo(rect, 32.0f, 1000.0f);

    // Thumb Y way below should clamp to max_scroll
    float scroll = ScrollFromThumbY(info, 99999.0f);
    EXPECT_FLOAT_EQ(scroll, info.max_scroll);
}
