#pragma once
#include "pane.h"
#include "pane_layout.h"
#include <algorithm>

// Manages all pane-related state: widths, visibility, scroll, hover, drag.
// No Win32 dependency — fully testable.
class PaneController {
public:
    // ---- Drag target ----
    enum class DragTarget { None, Splitter1, Splitter2, FileScrollbar, TocScrollbar };

    // ---- Visibility ----
    bool IsFilePaneVisible() const noexcept { return show_file_; }
    bool IsTocPaneVisible() const noexcept { return show_toc_; }
    void ToggleFilePane() noexcept { show_file_ = !show_file_; }
    void ToggleTocPane() noexcept { show_toc_ = !show_toc_; }

    // ---- Widths ----
    float GetFilePaneWidth() const noexcept { return file_width_; }
    float GetTocPaneWidth() const noexcept { return toc_width_; }
    void SetFilePaneWidth(float w) noexcept { file_width_ = std::max(w, PANE_MIN_WIDTH); }
    void SetTocPaneWidth(float w) noexcept { toc_width_ = std::max(w, PANE_MIN_WIDTH); }

    // ---- Scroll ----
    ScrollState& FileScroll() noexcept { return file_scroll_; }
    ScrollState& TocScroll() noexcept { return toc_scroll_; }
    const ScrollState& FileScroll() const noexcept { return file_scroll_; }
    const ScrollState& TocScroll() const noexcept { return toc_scroll_; }
    void ResetScrollStates() noexcept { file_scroll_ = {}; toc_scroll_ = {}; }

    // Scroll a pane by delta, return true if scroll actually changed
    bool ScrollFilePaneBy(float delta, float max_scroll) noexcept;
    bool ScrollTocPaneBy(float delta, float max_scroll) noexcept;

    // ---- Hover ----
    int GetHoveredFileIndex() const noexcept { return hovered_file_; }
    int GetHoveredTocIndex() const noexcept { return hovered_toc_; }
    // Returns true if the value changed
    bool SetHoveredFileIndex(int idx) noexcept;
    bool SetHoveredTocIndex(int idx) noexcept;

private:
    static bool ScrollPaneBy(ScrollState& state, float delta, float max_scroll) noexcept;
    static bool SetHoveredIndex(int& current, int idx) noexcept;
public:

    // ---- Drag ----
    DragTarget GetDragTarget() const noexcept { return drag_target_; }
    void StartDrag(DragTarget t) noexcept { drag_target_ = t; }
    void EndDrag() noexcept { drag_target_ = DragTarget::None; }
    float GetDragScrollOffset() const noexcept { return drag_scroll_offset_; }
    void SetDragScrollOffset(float off) noexcept { drag_scroll_offset_ = off; }

    // Constrain splitter1 position (file pane right edge)
    void DragSplitter1To(float dip_x, float total_width, float splitter_w) noexcept;
    // Constrain splitter2 position (toc pane right edge)
    void DragSplitter2To(float dip_x, float total_width, float splitter_w) noexcept;

    // ---- Zoom ----
    void ApplyZoom(float ratio) noexcept;

    // ---- Layout ----
    PaneLayout ComputeLayout(float total_w, float total_h, float splitter_w) const noexcept;
    PaneZone DetectZone(float dip_x, float total_w, float total_h, float splitter_w) const noexcept;

    // ---- Constants ----
    static constexpr float PANE_MIN_WIDTH = 100.0f;
    static constexpr float MD_PANE_MIN_WIDTH = 200.0f;

private:
    float file_width_ = 220.0f;
    float toc_width_ = 220.0f;
    bool show_file_ = true;
    bool show_toc_ = true;

    ScrollState file_scroll_{};
    ScrollState toc_scroll_{};

    int hovered_file_ = -1;
    int hovered_toc_ = -1;

    DragTarget drag_target_ = DragTarget::None;
    float drag_scroll_offset_ = 0.0f;
};
