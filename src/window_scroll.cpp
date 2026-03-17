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

    float old_pos = viewport_.GetScrollY();
    auto pane_layout = GetPaneLayout();
    float page_size = pane_layout.md_rect.height;

    switch (LOWORD(wParam)) {
        case SB_LINEUP:    ScrollTo(viewport_.GetScrollY() - 40.0f); break;
        case SB_LINEDOWN:  ScrollTo(viewport_.GetScrollY() + 40.0f); break;
        case SB_PAGEUP:    ScrollTo(viewport_.GetScrollY() - page_size); break;
        case SB_PAGEDOWN:  ScrollTo(viewport_.GetScrollY() + page_size); break;
        case SB_THUMBTRACK:
            viewport_.SetScrollbarTracking(true);
            ScrollTo(static_cast<float>(si.nTrackPos));
            break;
        case SB_THUMBPOSITION:
            viewport_.SetScrollbarTracking(false);
            ScrollTo(static_cast<float>(si.nTrackPos));
            break;
        case SB_ENDSCROLL:
            viewport_.SetScrollbarTracking(false);
            break;
        case SB_TOP:       ScrollTo(0.0f); break;
        case SB_BOTTOM:    ScrollTo(viewport_.GetMaxScroll()); break;
    }

    if (viewport_.GetScrollY() != old_pos) {
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
    si.nPos = static_cast<int>(viewport_.GetScrollY());
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
}

void MainWindow::ScrollTo(float position) {
    viewport_.ScrollTo(position);
    last_md_hit_pos_ = {LONG_MIN, LONG_MIN}; // Invalidate cursor cache
}

void MainWindow::SmoothScrollBy(float delta) {
    bool was_scrolling = viewport_.IsSmoothScrolling();
    viewport_.SmoothScrollBy(delta);

    if (!was_scrolling && viewport_.IsSmoothScrolling()) {
        SetTimer(hwnd_, TIMER_SMOOTH_SCROLL, 16, nullptr);
    }
}

void MainWindow::UpdateSmoothScroll() {
    bool still_active = viewport_.UpdateSmoothScroll();

    if (!still_active) {
        KillTimer(hwnd_, TIMER_SMOOTH_SCROLL);
    }

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
    if (!viewport_.IsSmoothScrolling()) return;
    viewport_.StopSmoothScroll();
    KillTimer(hwnd_, TIMER_SMOOTH_SCROLL);
}

void MainWindow::SyncMaxScroll() {
    auto pane_layout = GetPaneLayout();
    float total = renderer_.GetLayout().GetTotalHeight();
    viewport_.SyncMaxScroll(total, pane_layout.md_rect.height);
}

int MainWindow::FindFirstVisibleNode() const {
    return viewport_.FindFirstVisibleNode(layout_cache_, nodes_.size());
}

void MainWindow::AnchorCompensateScroll(int anchor_idx, float anchor_y_before) {
    viewport_.AnchorCompensateScroll(anchor_idx, anchor_y_before, layout_cache_);
    SyncMaxScroll();
}

// ---- Deferred layout ----

void MainWindow::OnResizeEnd() {
    KillTimer(hwnd_, TIMER_DEFERRED_LAYOUT);

    auto pane_layout = GetPaneLayout();
    float md_width = pane_layout.md_rect.width;
    float viewport_top = viewport_.GetScrollY();
    float viewport_bottom = viewport_.GetScrollY() + pane_layout.md_rect.height;

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

    if (!viewport_.IsScrollbarTracking()) {
        AnchorCompensateScroll(anchor_idx, anchor_y_before);
    } else {
        SyncMaxScroll();
    }

    if (!more) {
        KillTimer(hwnd_, TIMER_DEFERRED_LAYOUT);
        UpdateScrollBar();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::UpdateLayoutAndScroll(float desired_scroll) {
    float md_width = GetMarkdownPaneWidth();
    renderer_.GetLayout().ComputeLayout(nodes_, layout_cache_, md_width);

    viewport_.SetScrollY(desired_scroll);
    viewport_.SetScrollTarget(desired_scroll);
    SyncMaxScroll();

    UpdateScrollBar();
    InvalidateRect(hwnd_, nullptr, FALSE);
}
