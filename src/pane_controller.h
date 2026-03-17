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
    bool IsFilePaneVisible() const { return show_file_; }
    bool IsTocPaneVisible() const { return show_toc_; }
    void ToggleFilePane() { show_file_ = !show_file_; }
    void ToggleTocPane() { show_toc_ = !show_toc_; }

    // ---- Widths ----
    float GetFilePaneWidth() const { return file_width_; }
    float GetTocPaneWidth() const { return toc_width_; }
    void SetFilePaneWidth(float w) { file_width_ = std::max(w, PANE_MIN_WIDTH); }
    void SetTocPaneWidth(float w) { toc_width_ = std::max(w, PANE_MIN_WIDTH); }

    // ---- Scroll ----
    ScrollState& FileScroll() { return file_scroll_; }
    ScrollState& TocScroll() { return toc_scroll_; }
    const ScrollState& FileScroll() const { return file_scroll_; }
    const ScrollState& TocScroll() const { return toc_scroll_; }
    void ResetScrollStates() { file_scroll_ = {}; toc_scroll_ = {}; }

    // Scroll a pane by delta, return true if scroll actually changed
    bool ScrollFilePaneBy(float delta, float max_scroll);
    bool ScrollTocPaneBy(float delta, float max_scroll);

    // ---- Hover ----
    int GetHoveredFileIndex() const { return hovered_file_; }
    int GetHoveredTocIndex() const { return hovered_toc_; }
    // Returns true if the value changed
    bool SetHoveredFileIndex(int idx);
    bool SetHoveredTocIndex(int idx);

    // ---- Drag ----
    DragTarget GetDragTarget() const { return drag_target_; }
    void StartDrag(DragTarget t) { drag_target_ = t; }
    void EndDrag() { drag_target_ = DragTarget::None; }
    float GetDragScrollOffset() const { return drag_scroll_offset_; }
    void SetDragScrollOffset(float off) { drag_scroll_offset_ = off; }

    // Constrain splitter1 position (file pane right edge)
    void DragSplitter1To(float dip_x, float total_width, float splitter_w);
    // Constrain splitter2 position (toc pane right edge)
    void DragSplitter2To(float dip_x, float total_width, float splitter_w);

    // ---- Zoom ----
    void ApplyZoom(float ratio);

    // ---- Layout ----
    PaneLayout ComputeLayout(float total_w, float total_h, float splitter_w) const;
    PaneZone DetectZone(float dip_x, float total_w, float total_h, float splitter_w) const;

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
