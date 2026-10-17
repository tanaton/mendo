#pragma once
#include "ui_types.h"
#include "pane_layout.h"
#include <algorithm>
#include <cstddef>

class PaneController {
public:
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
            s.hovered_button = PaneHeaderButton::None;
        }
    }
    constexpr void ToggleSidePane(PaneTarget t) noexcept
    {
        SetSidePaneVisible(t, !Inst(t).show);
    }

    constexpr float GetSidePaneWidth(PaneTarget t) const noexcept
    {
        return Inst(t).width;
    }
    constexpr void SetSidePaneWidth(PaneTarget t, float w) noexcept
    {
        Inst(t).width = std::max(w, PANE_MIN_WIDTH);
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
        for (auto& inst : instances_) {
            inst.scroll = {};
        }
    }

    // 変化した場合 true。
    bool ScrollSidePaneBy(PaneTarget t, float delta, float max_scroll) noexcept;

    constexpr int GetHoveredSideIndex(PaneTarget t) const noexcept
    {
        return Inst(t).hovered_index;
    }
    // 変化した場合 true。
    bool SetHoveredSideIndex(PaneTarget t, int idx) noexcept;

    constexpr PaneHeaderButton GetSideHoveredButton(PaneTarget t) const noexcept
    {
        return Inst(t).hovered_button;
    }
    // 変化した場合 true。
    bool SetSideHoveredButton(PaneTarget t, PaneHeaderButton button) noexcept;

    // edge のペインの右端にあるスプリッタを dip_x へ動かす。
    void DragSplitterTo(PaneTarget edge, float dip_x, float total_width, float splitter_w) noexcept;

    void ApplyZoom(float ratio) noexcept;

    PaneLayout ComputeLayout(float total_w, float total_h, float splitter_w, float top_offset = 0.0f) const noexcept;

    static constexpr float PANE_DEFAULT_WIDTH = 220.0f;
    static constexpr float PANE_MIN_WIDTH = 100.0f;

private:
    struct Instance {
        ScrollState scroll{};
        float width = PANE_DEFAULT_WIDTH;
        int hovered_index = -1;
        bool show = true;
        PaneHeaderButton hovered_button = PaneHeaderButton::None;
    };
    Instance instances_[2];

    constexpr Instance& Inst(PaneTarget t) noexcept
    {
        return instances_[static_cast<size_t>(t)];
    }
    constexpr const Instance& Inst(PaneTarget t) const noexcept
    {
        return instances_[static_cast<size_t>(t)];
    }
};
