#include "pane_controller.h"

bool PaneController::ScrollPaneBy(ScrollState& state, float delta, float max_scroll) noexcept {
    float old = state.scroll_y;
    state.scroll_y = std::clamp(state.scroll_y + delta, 0.0f, max_scroll);
    state.max_scroll = max_scroll;
    return state.scroll_y != old;
}

bool PaneController::ScrollFilePaneBy(float delta, float max_scroll) noexcept {
    return ScrollPaneBy(file_scroll_, delta, max_scroll);
}

bool PaneController::ScrollTocPaneBy(float delta, float max_scroll) noexcept {
    return ScrollPaneBy(toc_scroll_, delta, max_scroll);
}

bool PaneController::SetHoveredIndex(int& current, int idx) noexcept {
    bool changed = current != idx;
    current = idx;
    return changed;
}

bool PaneController::SetHoveredFileIndex(int idx) noexcept {
    return SetHoveredIndex(hovered_file_, idx);
}

bool PaneController::SetHoveredTocIndex(int idx) noexcept {
    return SetHoveredIndex(hovered_toc_, idx);
}

void PaneController::DragSplitter1To(float dip_x, float total_width, float splitter_w) noexcept {
    file_width_ = std::clamp(dip_x, PANE_MIN_WIDTH, total_width);

    float used = file_width_ + splitter_w;
    if (show_toc_) used += toc_width_ + splitter_w;
    if (total_width - used < MD_PANE_MIN_WIDTH) {
        file_width_ = total_width - MD_PANE_MIN_WIDTH - splitter_w;
        if (show_toc_) file_width_ -= toc_width_ + splitter_w;
        file_width_ = std::max(PANE_MIN_WIDTH, file_width_);
    }
}

void PaneController::DragSplitter2To(float dip_x, float total_width, float splitter_w) noexcept {
    // toc_left is known from layout; dip_x is the new right edge
    auto layout = ComputeLayout(total_width, 0.0f, splitter_w);
    float toc_left = layout.toc_rect.x;
    float new_width = dip_x - toc_left;
    toc_width_ = std::max(PANE_MIN_WIDTH, new_width);

    float used = splitter_w;
    if (show_file_) used += file_width_ + splitter_w;
    used += toc_width_;
    if (total_width - used < MD_PANE_MIN_WIDTH) {
        toc_width_ = total_width - MD_PANE_MIN_WIDTH - splitter_w;
        if (show_file_) toc_width_ -= file_width_ + splitter_w;
        toc_width_ = std::max(PANE_MIN_WIDTH, toc_width_);
    }
}

void PaneController::ApplyZoom(float ratio) noexcept {
    file_width_ *= ratio;
    toc_width_ *= ratio;
    file_scroll_.scroll_y *= ratio;
    file_scroll_.max_scroll *= ratio;
    toc_scroll_.scroll_y *= ratio;
    toc_scroll_.max_scroll *= ratio;
}

PaneLayout PaneController::ComputeLayout(float total_w, float total_h, float splitter_w) const noexcept {
    return ComputePaneLayout(total_w, total_h, file_width_, toc_width_,
                             splitter_w, show_file_, show_toc_, MD_PANE_MIN_WIDTH);
}

PaneZone PaneController::DetectZone(float dip_x, float total_w, float total_h, float splitter_w) const noexcept {
    auto layout = ComputeLayout(total_w, total_h, splitter_w);
    return DetectPaneZone(dip_x, layout, splitter_w, show_file_, show_toc_);
}
