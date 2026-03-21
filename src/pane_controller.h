#pragma once
#include "pane.h"
#include "pane_layout.h"
#include <algorithm>

// ペイン関連の全状態を管理: 幅、表示/非表示、スクロール、ホバー、ドラッグ。
// Win32非依存 — 完全にテスト可能。
class PaneController {
public:
    // ---- ドラッグ対象 ----
    enum class DragTarget { None, Splitter1, Splitter2, FileScrollbar, TocScrollbar };

    // ---- 表示/非表示 ----
    constexpr bool IsFilePaneVisible() const noexcept { return show_file_; }
    constexpr bool IsTocPaneVisible() const noexcept { return show_toc_; }
    constexpr void ToggleFilePane() noexcept { show_file_ = !show_file_; }
    constexpr void ToggleTocPane() noexcept { show_toc_ = !show_toc_; }

    // ---- 幅 ----
    constexpr float GetFilePaneWidth() const noexcept { return file_width_; }
    constexpr float GetTocPaneWidth() const noexcept { return toc_width_; }
    constexpr void SetFilePaneWidth(float w) noexcept { file_width_ = std::max(w, PANE_MIN_WIDTH); }
    constexpr void SetTocPaneWidth(float w) noexcept { toc_width_ = std::max(w, PANE_MIN_WIDTH); }

    // ---- スクロール ----
    constexpr ScrollState& FileScroll() noexcept { return file_scroll_; }
    constexpr ScrollState& TocScroll() noexcept { return toc_scroll_; }
    constexpr const ScrollState& FileScroll() const noexcept { return file_scroll_; }
    constexpr const ScrollState& TocScroll() const noexcept { return toc_scroll_; }
    constexpr void ResetScrollStates() noexcept { file_scroll_ = {}; toc_scroll_ = {}; }

    // ペインをdelta分スクロールし、実際にスクロール位置が変化した場合trueを返す
    bool ScrollFilePaneBy(float delta, float max_scroll) noexcept;
    bool ScrollTocPaneBy(float delta, float max_scroll) noexcept;

    // ---- ホバー ----
    constexpr int GetHoveredFileIndex() const noexcept { return hovered_file_; }
    constexpr int GetHoveredTocIndex() const noexcept { return hovered_toc_; }
    // 値が変化した場合trueを返す
    bool SetHoveredFileIndex(int idx) noexcept;
    bool SetHoveredTocIndex(int idx) noexcept;

private:
    static bool ScrollPaneBy(ScrollState& state, float delta, float max_scroll) noexcept;
    static bool SetHoveredIndex(int& current, int idx) noexcept;
public:

    // ---- ドラッグ ----
    constexpr DragTarget GetDragTarget() const noexcept { return drag_target_; }
    constexpr void StartDrag(DragTarget t) noexcept { drag_target_ = t; }
    constexpr void EndDrag() noexcept { drag_target_ = DragTarget::None; }
    constexpr float GetDragScrollOffset() const noexcept { return drag_scroll_offset_; }
    constexpr void SetDragScrollOffset(float off) noexcept { drag_scroll_offset_ = off; }

    // スプリッター1の位置を制約する（ファイルペインの右端）
    void DragSplitter1To(float dip_x, float total_width, float splitter_w) noexcept;
    // スプリッター2の位置を制約する（目次ペインの右端）
    void DragSplitter2To(float dip_x, float total_width, float splitter_w) noexcept;

    // ---- ズーム ----
    void ApplyZoom(float ratio) noexcept;

    // ---- レイアウト ----
    PaneLayout ComputeLayout(float total_w, float total_h, float splitter_w) const noexcept;
    PaneZone DetectZone(float dip_x, float total_w, float total_h, float splitter_w) const noexcept;

    // ---- 定数 ----
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
