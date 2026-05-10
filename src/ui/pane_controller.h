#pragma once
#include "ui_types.h"
#include "pane_layout.h"
#include <algorithm>
#include <cstddef>

// ペイン関連の全状態を管理: 幅、表示/非表示、スクロール、ホバー、ドラッグ。
// File / TOC ペインの対称な状態は 2 要素配列で持ち、PaneTarget で添字アクセスする。
// Win32非依存 — 完全にテスト可能。
class PaneController {
public:
    enum class DragTarget : uint8_t {
        None,
        Splitter1,
        Splitter2,
        FileScrollbar,
        TocScrollbar,
        MdScrollbar
    };

    constexpr bool IsSidePaneVisible(PaneTarget t) const noexcept
    {
        return Inst(t).show;
    }
    constexpr void SetSidePaneVisible(PaneTarget t, bool v) noexcept
    {
        auto& s = Inst(t);
        if (s.show != v) {
            s.show = v;
            s.hovered_index = -1;
            s.close_hovered = false;
            s.refresh_hovered = false;
        }
    }
    constexpr void ToggleSidePane(PaneTarget t) noexcept
    {
        SetSidePaneVisible(t, !Inst(t).show);
    }

    constexpr float GetSidePaneWidth(PaneTarget t) const noexcept
    {
        return Width(t);
    }
    constexpr void SetSidePaneWidth(PaneTarget t, float w) noexcept
    {
        Width(t) = std::max(w, PANE_MIN_WIDTH);
    }

    constexpr ScrollState& SidePaneScroll(PaneTarget t) noexcept
    {
        return Inst(t).scroll;
    }
    constexpr const ScrollState& SidePaneScroll(PaneTarget t) const noexcept
    {
        return Inst(t).scroll;
    }
    constexpr void ResetScrollStates() noexcept
    {
        instances_[0].scroll = {};
        instances_[1].scroll = {};
    }

    // 実際にスクロール位置が変化した場合 true を返す。
    bool ScrollSidePaneBy(PaneTarget t, float delta, float max_scroll) noexcept;

    constexpr int GetHoveredSideIndex(PaneTarget t) const noexcept
    {
        return Inst(t).hovered_index;
    }
    // 値が変化した場合 true を返す。
    bool SetHoveredSideIndex(PaneTarget t, int idx) noexcept;

    constexpr bool IsSideCloseHovered(PaneTarget t) const noexcept
    {
        return Inst(t).close_hovered;
    }
    bool SetSideCloseHovered(PaneTarget t, bool h) noexcept;

    constexpr bool IsSideRefreshHovered(PaneTarget t) const noexcept
    {
        return Inst(t).refresh_hovered;
    }
    bool SetSideRefreshHovered(PaneTarget t, bool h) noexcept;

public:
    constexpr DragTarget GetDragTarget() const noexcept
    {
        return drag_target_;
    }
    constexpr void StartDrag(DragTarget t) noexcept
    {
        drag_target_ = t;
    }
    constexpr void EndDrag() noexcept
    {
        drag_target_ = DragTarget::None;
    }
    constexpr float GetDragScrollOffset() const noexcept
    {
        return drag_scroll_offset_;
    }
    constexpr void SetDragScrollOffset(float off) noexcept
    {
        drag_scroll_offset_ = off;
    }

    // スプリッター1の位置を制約する（ファイルペインの右端）。
    void DragSplitter1To(float dip_x, float total_width, float splitter_w) noexcept;
    // スプリッター2の位置を制約する（目次ペインの右端）。
    void DragSplitter2To(float dip_x, float total_width, float splitter_w) noexcept;

    void ApplyZoom(float ratio) noexcept;

    PaneLayout ComputeLayout(float total_w, float total_h, float splitter_w, float top_offset = 0.0f) const noexcept;
    PaneZone DetectZone(float dip_x, float total_w, float total_h, float splitter_w) const noexcept;

    static constexpr float PANE_DEFAULT_WIDTH = 220.0f;
    static constexpr float PANE_MIN_WIDTH = 100.0f;

private:
    struct Instance {
        ScrollState scroll{};
        int hovered_index = -1;
        bool show = true;
        bool close_hovered = false;
        bool refresh_hovered = false;
    };
    Instance instances_[2];
    float widths_[2] = { PANE_DEFAULT_WIDTH, PANE_DEFAULT_WIDTH };

    DragTarget drag_target_ = DragTarget::None;
    float drag_scroll_offset_ = 0.0f;

    constexpr Instance& Inst(PaneTarget t) noexcept
    {
        return instances_[static_cast<size_t>(t)];
    }
    constexpr const Instance& Inst(PaneTarget t) const noexcept
    {
        return instances_[static_cast<size_t>(t)];
    }
    constexpr float& Width(PaneTarget t) noexcept
    {
        return widths_[static_cast<size_t>(t)];
    }
    constexpr float Width(PaneTarget t) const noexcept
    {
        return widths_[static_cast<size_t>(t)];
    }

    static bool ScrollPaneBy(ScrollState& state, float delta, float max_scroll) noexcept;
    static bool SetFlag(bool& current, bool value) noexcept;
    static float ConstrainSplitterWidth(float requested_width, float total_width,
                                        float splitter_w, float other_width,
                                        bool other_visible) noexcept;
};
