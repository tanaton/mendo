#pragma once
#include "pane.h"

// Pane zone identifiers (which pane a point lies in)
enum class PaneZone : int {
    None,
    FilePane,
    Splitter1,
    TocPane,
    Splitter2,
    MdPane
};

// Computed pane layout positions
struct PaneLayout {
    PaneRect file_rect{};
    PaneRect toc_rect{};
    PaneRect md_rect{};
};

// Scroll info computed for a pane (used for scrollbar rendering and interaction)
struct PaneScrollInfo {
    float content_top = 0.0f;
    float content_height = 0.0f;
    float total_content = 0.0f;
    float max_scroll = 0.0f;
    float thumb_height = 0.0f;
};

// Compute pane layout given window dimensions and pane configuration.
PaneLayout ComputePaneLayout(float total_width, float total_height,
                              float file_pane_width, float toc_pane_width,
                              float splitter_width, bool show_file, bool show_toc,
                              float md_min_width = 200.0f) noexcept;

// Determine which pane zone a point (in DIP coordinates) falls within.
PaneZone DetectPaneZone(float dip_x, const PaneLayout& layout,
                         float splitter_width, bool show_file, bool show_toc) noexcept;

// Compute scroll info for a pane, used for scrollbar rendering and interaction.
PaneScrollInfo ComputeScrollInfo(const PaneRect& rect, float header_height,
                                  float total_content, float thumb_min = PANE_SCROLLBAR_THUMB_MIN) noexcept;

// Compute scrollbar thumb position for drag calculations.
// Returns the Y position of the top of the thumb.
float ComputeThumbY(const PaneScrollInfo& info, float scroll_y) noexcept;

// Compute new scroll position from a thumb drag.
// given the new thumb Y position.
float ScrollFromThumbY(const PaneScrollInfo& info, float thumb_y) noexcept;
