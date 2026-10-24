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
        Inst(t).show = v;
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
    // 一覧の項目 (文字・行の高さ) だけに掛かる倍率。ヘッダーとペイン幅は変えない。
    constexpr ZoomLevel& SidePaneZoom(PaneTarget t) noexcept
    {
        return Inst(t).zoom;
    }
    constexpr const ZoomLevel& SidePaneZoom(PaneTarget t) const noexcept
    {
        return Inst(t).zoom;
    }

    constexpr void ResetScrollStates() noexcept
    {
        for (auto& inst : instances_) {
            inst.scroll = {};
        }
    }

    // 変化した場合 true。
    bool ScrollSidePaneBy(PaneTarget t, float delta, float max_scroll) noexcept;

    // edge のペインの右端にあるスプリッタを dip_x へ動かす。
    void DragSplitterTo(PaneTarget edge, float dip_x, float total_width, float splitter_w) noexcept;

    PaneLayout ComputeLayout(float total_w, float total_h, float splitter_w, float top_offset = 0.0f) const noexcept;

    static constexpr float PANE_DEFAULT_WIDTH = 220.0f;
    static constexpr float PANE_MIN_WIDTH = 100.0f;

private:
    struct Instance {
        ScrollState scroll{};
        float width = PANE_DEFAULT_WIDTH;
        ZoomLevel zoom;
        bool show = true;
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
