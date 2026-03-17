#include "pane_controller.h"

bool PaneController::ScrollFilePaneBy(float delta, float max_scroll) {
    float old = file_scroll_.scroll_y;
    file_scroll_.scroll_y = std::clamp(file_scroll_.scroll_y + delta, 0.0f, max_scroll);
    file_scroll_.max_scroll = max_scroll;
    return file_scroll_.scroll_y != old;
}

bool PaneController::ScrollTocPaneBy(float delta, float max_scroll) {
    float old = toc_scroll_.scroll_y;
    toc_scroll_.scroll_y = std::clamp(toc_scroll_.scroll_y + delta, 0.0f, max_scroll);
    toc_scroll_.max_scroll = max_scroll;
    return toc_scroll_.scroll_y != old;
}

bool PaneController::SetHoveredFileIndex(int idx) {
    bool changed = hovered_file_ != idx;
    hovered_file_ = idx;
    return changed;
}

bool PaneController::SetHoveredTocIndex(int idx) {
    bool changed = hovered_toc_ != idx;
    hovered_toc_ = idx;
    return changed;
}

void PaneController::DragSplitter1To(float dip_x, float total_width, float splitter_w) {
    file_width_ = std::clamp(dip_x, PANE_MIN_WIDTH, total_width);

    float used = file_width_ + splitter_w;
    if (show_toc_) used += toc_width_ + splitter_w;
    if (total_width - used < MD_PANE_MIN_WIDTH) {
        file_width_ = total_width - MD_PANE_MIN_WIDTH - splitter_w;
        if (show_toc_) file_width_ -= toc_width_ + splitter_w;
        file_width_ = std::max(PANE_MIN_WIDTH, file_width_);
    }
}

void PaneController::DragSplitter2To(float dip_x, float total_width, float splitter_w) {
    // toc_left is known from layout; dip_x is the new right edge
    auto layout = ComputeLayout(total_width, 0.0f, splitter_w);
    float toc_left = layout.toc_rect.x;
    float new_width = dip_x - toc_left;
    toc_width_ = std::clamp(new_width, PANE_MIN_WIDTH, new_width);

    float used = splitter_w;
    if (show_file_) used += file_width_ + splitter_w;
    used += toc_width_;
    if (total_width - used < MD_PANE_MIN_WIDTH) {
        toc_width_ = total_width - MD_PANE_MIN_WIDTH - splitter_w;
        if (show_file_) toc_width_ -= file_width_ + splitter_w;
        toc_width_ = std::max(PANE_MIN_WIDTH, toc_width_);
    }
}

void PaneController::ApplyZoom(float ratio) {
    file_width_ *= ratio;
    toc_width_ *= ratio;
    file_scroll_.scroll_y *= ratio;
    file_scroll_.max_scroll *= ratio;
    toc_scroll_.scroll_y *= ratio;
    toc_scroll_.max_scroll *= ratio;
}

PaneLayout PaneController::ComputeLayout(float total_w, float total_h, float splitter_w) const {
    return ComputePaneLayout(total_w, total_h, file_width_, toc_width_,
                             splitter_w, show_file_, show_toc_, MD_PANE_MIN_WIDTH);
}

PaneZone PaneController::DetectZone(float dip_x, float total_w, float total_h, float splitter_w) const {
    auto layout = ComputeLayout(total_w, total_h, splitter_w);
    return DetectPaneZone(dip_x, layout, splitter_w, show_file_, show_toc_);
}
