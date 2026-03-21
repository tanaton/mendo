#include "app.h"
#include "pane_layout.h"
#include <windows.h>
#include <algorithm>
#include <cmath>

// ============================================================
// Scrollbar & Scroll
// ============================================================

void App::OnVScroll(WPARAM wParam) {
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
        UpdateScrollBar(page_size);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void App::UpdateScrollBar() {
    auto pane_layout = GetPaneLayout();
    UpdateScrollBar(pane_layout.md_rect.height);
}

void App::UpdateScrollBar(float md_pane_height) {
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    si.nMin = 0;
    si.nMax = static_cast<int>(layout_service_->GetTotalHeight());
    si.nPage = static_cast<UINT>(md_pane_height);
    si.nPos = static_cast<int>(viewport_.GetScrollY());
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
}

void App::ScrollTo(float position) {
    viewport_.ScrollTo(position);
    last_md_hit_pos_ = {LONG_MIN, LONG_MIN};
}

void App::SmoothScrollBy(float delta) {
    bool was_scrolling = viewport_.IsSmoothScrolling();
    viewport_.SmoothScrollBy(delta);

    if (!was_scrolling && viewport_.IsSmoothScrolling()) {
        SetTimer(hwnd_, TIMER_SMOOTH_SCROLL, 16, nullptr);
    }
}

void App::UpdateSmoothScroll() {
    bool still_active = viewport_.UpdateSmoothScroll();

    if (!still_active) {
        KillTimer(hwnd_, TIMER_SMOOTH_SCROLL);
    }

    UpdateScrollBar();
    InvalidateMdPane();
}

void App::InvalidateMdPane() {
    if (!renderer_.GetRenderTarget()) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    float scale = cached_dpi_scale_;
    auto layout = GetPaneLayout();
    RECT rc;
    rc.left = static_cast<LONG>(layout.md_rect.x * scale);
    rc.top = 0;
    rc.right = static_cast<LONG>((layout.md_rect.x + layout.md_rect.width) * scale) + 1;
    rc.bottom = static_cast<LONG>(layout.md_rect.height * scale) + 1;
    InvalidateRect(hwnd_, &rc, FALSE);
}

void App::StopSmoothScroll() {
    if (!viewport_.IsSmoothScrolling()) return;
    viewport_.StopSmoothScroll();
    KillTimer(hwnd_, TIMER_SMOOTH_SCROLL);
}

void App::SyncMaxScroll() {
    auto pane_layout = GetPaneLayout();
    float total = layout_service_->GetTotalHeight();
    viewport_.SyncMaxScroll(total, pane_layout.md_rect.height);
}

void App::SyncMaxScroll(float md_pane_height) {
    float total = layout_service_->GetTotalHeight();
    viewport_.SyncMaxScroll(total, md_pane_height);
}

int App::FindFirstVisibleNode() const {
    return viewport_.FindFirstVisibleNode(layout_cache_, doc_.GetNodes().size());
}

void App::AnchorCompensateScroll(int anchor_idx, float anchor_y_before) {
    viewport_.AnchorCompensateScroll(anchor_idx, anchor_y_before, layout_cache_);
    SyncMaxScroll();
}

void App::AnchorCompensateScroll(int anchor_idx, float anchor_y_before, float md_pane_height) {
    viewport_.AnchorCompensateScroll(anchor_idx, anchor_y_before, layout_cache_);
    SyncMaxScroll(md_pane_height);
}

// ============================================================
// Deferred layout
// ============================================================

void App::OnResizeEnd() {
    KillTimer(hwnd_, TIMER_DEFERRED_LAYOUT);

    auto pane_layout = GetPaneLayout();
    float md_width = pane_layout.md_rect.width;
    float md_height = pane_layout.md_rect.height;

    layout_service_->ViewportLayout(doc_, layout_cache_, md_width, md_height);

    SyncMaxScroll(md_height);
    UpdateScrollBar(md_height);
    InvalidateRect(hwnd_, nullptr, FALSE);

    if (layout_service_->HasDirtyNodes()) {
        SetTimer(hwnd_, TIMER_DEFERRED_LAYOUT, 16, nullptr);
    }

    RequestMermaidRenders();
}

void App::OnDeferredLayout() {
    int anchor_idx = FindFirstVisibleNode();
    float anchor_y_before = (anchor_idx >= 0) ? layout_cache_[anchor_idx].y_position : 0.0f;

    auto pane_layout = GetPaneLayout();
    float md_width = pane_layout.md_rect.width;
    float md_height = pane_layout.md_rect.height;
    bool more = layout_service_->ProcessDirtyBatch(doc_, layout_cache_, md_width, 200);

    if (!viewport_.IsScrollbarTracking()) {
        AnchorCompensateScroll(anchor_idx, anchor_y_before, md_height);
    } else {
        SyncMaxScroll(md_height);
    }

    if (!more) {
        KillTimer(hwnd_, TIMER_DEFERRED_LAYOUT);
        UpdateScrollBar(md_height);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void App::UpdateLayoutAndScroll(float desired_scroll) {
    auto pane_layout = GetPaneLayout();
    float md_width = pane_layout.md_rect.width;
    float md_height = pane_layout.md_rect.height;
    layout_service_->FullLayout(doc_, layout_cache_, md_width);

    viewport_.SetScrollY(desired_scroll);
    viewport_.SetScrollTarget(desired_scroll);
    SyncMaxScroll(md_height);

    UpdateScrollBar(md_height);
    InvalidateRect(hwnd_, nullptr, FALSE);
}
