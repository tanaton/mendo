#include "pane_controller.h"

bool PaneController::ScrollPaneBy(ScrollState& state, float delta, float max_scroll) noexcept
{
    const float old = state.scroll_y;
    state.scroll_y = std::clamp(state.scroll_y + delta, 0.0f, max_scroll);
    state.max_scroll = max_scroll;
    return state.scroll_y != old;
}

bool PaneController::ScrollFilePaneBy(float delta, float max_scroll) noexcept
{
    return ScrollPaneBy(file_scroll_, delta, max_scroll);
}

bool PaneController::ScrollTocPaneBy(float delta, float max_scroll) noexcept
{
    return ScrollPaneBy(toc_scroll_, delta, max_scroll);
}

bool PaneController::SetHoveredIndex(int& current, int idx) noexcept
{
    const bool changed = current != idx;
    current = idx;
    return changed;
}

bool PaneController::SetHoveredFileIndex(int idx) noexcept
{
    return SetHoveredIndex(hovered_file_, idx);
}

bool PaneController::SetHoveredTocIndex(int idx) noexcept
{
    return SetHoveredIndex(hovered_toc_, idx);
}

bool PaneController::SetFlag(bool& current, bool value) noexcept
{
    const bool changed = current != value;
    current = value;
    return changed;
}

bool PaneController::SetFileCloseHovered(bool h) noexcept
{
    return SetFlag(file_close_hovered_, h);
}

bool PaneController::SetTocCloseHovered(bool h) noexcept
{
    return SetFlag(toc_close_hovered_, h);
}

bool PaneController::SetFileRefreshHovered(bool h) noexcept
{
    return SetFlag(file_refresh_hovered_, h);
}

float PaneController::ConstrainSplitterWidth(float requested_width, float total_width,
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
    file_width_ = ConstrainSplitterWidth(dip_x, total_width, splitter_w, toc_width_, show_toc_);
}

void PaneController::DragSplitter2To(float dip_x, float total_width, float splitter_w) noexcept
{
    // toc_leftはレイアウトから既知; dip_xは新しい右端
    const auto layout = ComputeLayout(total_width, 0.0f, splitter_w);
    const float new_width = dip_x - layout.toc_rect.x;
    toc_width_ = ConstrainSplitterWidth(new_width, total_width, splitter_w, file_width_, show_file_);
}

void PaneController::ApplyZoom(float ratio) noexcept
{
    file_width_ *= ratio;
    toc_width_ *= ratio;
    file_scroll_.scroll_y *= ratio;
    file_scroll_.max_scroll *= ratio;
    toc_scroll_.scroll_y *= ratio;
    toc_scroll_.max_scroll *= ratio;
}

PaneLayout PaneController::ComputeLayout(float total_w, float total_h, float splitter_w, float top_offset) const noexcept
{
    return ComputePaneLayout(
        total_w,
        total_h,
        file_width_,
        toc_width_,
        splitter_w,
        show_file_,
        show_toc_,
        MD_PANE_MIN_WIDTH,
        top_offset);
}

PaneZone PaneController::DetectZone(float dip_x, float total_w, float total_h, float splitter_w) const noexcept
{
    const auto layout = ComputeLayout(total_w, total_h, splitter_w);
    return DetectPaneZone(dip_x, layout, splitter_w, show_file_, show_toc_);
}
