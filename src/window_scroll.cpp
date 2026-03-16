#include "window.h"
#include "pane_layout.h"
#include <algorithm>
#include <cmath>

// ---- Scrollbar & Scroll ----

void MainWindow::OnVScroll(WPARAM wParam) {
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    GetScrollInfo(hwnd_, SB_VERT, &si);

    float old_pos = scroll_y_;
    auto pane_layout = GetPaneLayout();
    float page_size = pane_layout.md_rect.height;

    switch (LOWORD(wParam)) {
        case SB_LINEUP:    ScrollTo(scroll_y_ - 40.0f); break;
        case SB_LINEDOWN:  ScrollTo(scroll_y_ + 40.0f); break;
        case SB_PAGEUP:    ScrollTo(scroll_y_ - page_size); break;
        case SB_PAGEDOWN:  ScrollTo(scroll_y_ + page_size); break;
        case SB_THUMBTRACK:
            is_scrollbar_tracking_ = true;
            ScrollTo(static_cast<float>(si.nTrackPos));
            break;
        case SB_THUMBPOSITION:
            is_scrollbar_tracking_ = false;
            ScrollTo(static_cast<float>(si.nTrackPos));
            break;
        case SB_ENDSCROLL:
            is_scrollbar_tracking_ = false;
            break;
        case SB_TOP:       ScrollTo(0.0f); break;
        case SB_BOTTOM:    ScrollTo(max_scroll_); break;
    }

    if (scroll_y_ != old_pos) {
        UpdateScrollBar();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::UpdateScrollBar() {
    auto pane_layout = GetPaneLayout();
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    si.nMin = 0;
    si.nMax = static_cast<int>(renderer_.GetLayout().GetTotalHeight());
    si.nPage = static_cast<UINT>(pane_layout.md_rect.height);
    si.nPos = static_cast<int>(scroll_y_);
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
}

void MainWindow::ScrollTo(float position) {
    scroll_y_ = std::clamp(position, 0.0f, max_scroll_);
    scroll_target_ = scroll_y_;
}

void MainWindow::SmoothScrollBy(float delta) {
    scroll_target_ = std::clamp(scroll_target_ + delta, 0.0f, max_scroll_);

    if (!smooth_scrolling_) {
        smooth_scrolling_ = true;
        SetTimer(hwnd_, TIMER_SMOOTH_SCROLL, 16, nullptr);  // ~60fps
    }
}

void MainWindow::UpdateSmoothScroll() {
    float diff = scroll_target_ - scroll_y_;

    if (std::abs(diff) < SCROLL_EPSILON) {
        scroll_y_ = scroll_target_;
        smooth_scrolling_ = false;
        KillTimer(hwnd_, TIMER_SMOOTH_SCROLL);
    } else {
        scroll_y_ += diff * SCROLL_SPEED;
    }

    scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll_);
    UpdateScrollBar();
    InvalidateMdPane();
}

void MainWindow::InvalidateMdPane() {
    auto* rt = renderer_.GetRenderTarget();
    if (!rt) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    float dpi_x, dpi_y;
    rt->GetDpi(&dpi_x, &dpi_y);
    float scale = dpi_x / 96.0f;
    auto layout = GetPaneLayout();
    RECT rc;
    rc.left = static_cast<LONG>(layout.md_rect.x * scale);
    rc.top = 0;
    rc.right = static_cast<LONG>((layout.md_rect.x + layout.md_rect.width) * scale) + 1;
    rc.bottom = static_cast<LONG>(layout.md_rect.height * scale) + 1;
    InvalidateRect(hwnd_, &rc, FALSE);
}

void MainWindow::StopSmoothScroll() {
    if (!smooth_scrolling_) return;
    scroll_y_ = scroll_target_;
    smooth_scrolling_ = false;
    KillTimer(hwnd_, TIMER_SMOOTH_SCROLL);
}

void MainWindow::SyncMaxScroll() {
    auto pane_layout = GetPaneLayout();
    float total = renderer_.GetLayout().GetTotalHeight();
    max_scroll_ = std::max(0.0f, total - pane_layout.md_rect.height);
    scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll_);
    scroll_target_ = scroll_y_;
}

int MainWindow::FindFirstVisibleNode() const {
    int lo = 0, hi = static_cast<int>(nodes_.size());
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (layout_cache_[mid].y_position + layout_cache_[mid].height <= scroll_y_)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < static_cast<int>(nodes_.size()) ? lo : -1;
}

void MainWindow::AnchorCompensateScroll(int anchor_idx, float anchor_y_before) {
    if (anchor_idx < 0) return;
    float shift = layout_cache_[anchor_idx].y_position - anchor_y_before;
    scroll_y_ += shift;
    scroll_target_ += shift;
    SyncMaxScroll();
}

// ---- Deferred layout ----

void MainWindow::OnResizeEnd() {
    KillTimer(hwnd_, TIMER_DEFERRED_LAYOUT);

    auto pane_layout = GetPaneLayout();
    float md_width = pane_layout.md_rect.width;
    float viewport_top = scroll_y_;
    float viewport_bottom = scroll_y_ + pane_layout.md_rect.height;

    renderer_.GetLayout().ComputeLayout(nodes_, layout_cache_, md_width, viewport_top, viewport_bottom);

    SyncMaxScroll();
    UpdateScrollBar();
    InvalidateRect(hwnd_, nullptr, FALSE);

    if (renderer_.GetLayout().HasDirtyNodes()) {
        SetTimer(hwnd_, TIMER_DEFERRED_LAYOUT, 16, nullptr);
    }

    RequestMermaidRenders();
}

void MainWindow::OnDeferredLayout() {
    int anchor_idx = FindFirstVisibleNode();
    float anchor_y_before = (anchor_idx >= 0) ? layout_cache_[anchor_idx].y_position : 0.0f;

    float md_width = GetMarkdownPaneWidth();
    bool more = renderer_.GetLayout().ProcessDirtyBatch(nodes_, layout_cache_, md_width, 200);

    // Compensate scroll to keep visible content at the same screen position,
    // but skip during active scrollbar drag to avoid fighting with user input
    if (!is_scrollbar_tracking_) {
        AnchorCompensateScroll(anchor_idx, anchor_y_before);
    } else {
        SyncMaxScroll();
    }

    if (!more) {
        // Only repaint on the final batch; intermediate batches only affect
        // off-screen nodes, so repainting would just cause sub-pixel jitter.
        KillTimer(hwnd_, TIMER_DEFERRED_LAYOUT);
        UpdateScrollBar();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::UpdateLayoutAndScroll(float desired_scroll) {
    float md_width = GetMarkdownPaneWidth();
    renderer_.GetLayout().ComputeLayout(nodes_, layout_cache_, md_width);

    scroll_y_ = desired_scroll;
    scroll_target_ = desired_scroll;
    SyncMaxScroll();

    UpdateScrollBar();
    InvalidateRect(hwnd_, nullptr, FALSE);
}
