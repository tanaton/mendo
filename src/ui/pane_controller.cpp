#include "pane_controller.h"

bool PaneController::ScrollPaneBy(ScrollState& state, float delta, float max_scroll) noexcept
{
    const float old = state.scroll_y;
    state.scroll_y = std::clamp(state.scroll_y + delta, 0.0f, max_scroll);
    state.max_scroll = max_scroll;
    return state.scroll_y != old;
}

bool PaneController::ScrollSidePaneBy(PaneTarget t, float delta, float max_scroll) noexcept
{
    return ScrollPaneBy(Inst(t).scroll, delta, max_scroll);
}

bool PaneController::SetHoveredSideIndex(PaneTarget t, int idx) noexcept
{
    auto& cur = Inst(t).hovered_index;
    const bool changed = cur != idx;
    cur = idx;
    return changed;
}

bool PaneController::SetFlag(bool& current, bool value) noexcept
{
    const bool changed = current != value;
    current = value;
    return changed;
}

bool PaneController::SetSideCloseHovered(PaneTarget t, bool h) noexcept
{
    return SetFlag(Inst(t).close_hovered, h);
}

bool PaneController::SetSideRefreshHovered(PaneTarget t, bool h) noexcept
{
    return SetFlag(Inst(t).refresh_hovered, h);
}

float PaneController::ConstrainSplitterWidth(
    float requested_width, float total_width,
    float splitter_w, float other_width,
    bool other_visible) noexcept
{
    float w = std::max(PANE_MIN_WIDTH, requested_width);
    float used = w + splitter_w;
    if (other_visible) {
        used += other_width + splitter_w;
    }
    if (total_width - used < MD_PANE_MIN_WIDTH) {
        w = total_width - MD_PANE_MIN_WIDTH - splitter_w;
        if (other_visible) {
            w -= other_width + splitter_w;
        }
        w = std::max(PANE_MIN_WIDTH, w);
    }
    return w;
}

void PaneController::DragSplitter1To(float dip_x, float total_width, float splitter_w) noexcept
{
    widths_[0] = ConstrainSplitterWidth(
        dip_x, total_width, splitter_w, widths_[1], instances_[1].show);
}

void PaneController::DragSplitter2To(float dip_x, float total_width, float splitter_w) noexcept
{
    // toc_leftはレイアウトから既知; dip_xは新しい右端
    const auto layout = ComputeLayout(total_width, 0.0f, splitter_w);
    const float new_width = dip_x - layout.toc_rect.x;
    widths_[1] = ConstrainSplitterWidth(
        new_width, total_width, splitter_w, widths_[0], instances_[0].show);
}

void PaneController::ApplyZoom(float ratio) noexcept
{
    for (int i = 0; i < 2; ++i) {
        widths_[i] *= ratio;
        instances_[i].scroll.scroll_y *= ratio;
        instances_[i].scroll.max_scroll *= ratio;
    }
}

PaneLayout PaneController::ComputeLayout(float total_w, float total_h, float splitter_w, float top_offset) const noexcept
{
    return ComputePaneLayout(
        total_w,
        total_h,
        widths_[0],
        widths_[1],
        splitter_w,
        instances_[0].show,
        instances_[1].show,
        MD_PANE_MIN_WIDTH,
        top_offset);
}

PaneZone PaneController::DetectZone(float dip_x, float total_w, float total_h, float splitter_w) const noexcept
{
    const auto layout = ComputeLayout(total_w, total_h, splitter_w);
    return DetectPaneZone(dip_x, layout, splitter_w, instances_[0].show, instances_[1].show);
}
