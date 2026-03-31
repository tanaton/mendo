#include "app.h"
#include "pane_layout.h"
#include <windows.h>
#include <algorithm>
#include <cmath>

// ============================================================
// スクロールバー・スクロール
// ============================================================

void App::UpdateScrollBar()
{
    // カスタムスクロールバーはRenderer側で描画するため、再描画をトリガーするのみ
    Invalidate();
}

void App::InvalidateHitPositions() noexcept
{
    last_md_hit_pos_ = { LONG_MIN, LONG_MIN };
    last_copy_hit_pos_ = { LONG_MIN, LONG_MIN };
}

void App::ScrollTo(float position)
{
    viewport_.ScrollTo(position);
    InvalidateHitPositions();
}

void App::SmoothScrollBy(float delta)
{
    bool was_scrolling = viewport_.IsSmoothScrolling();
    viewport_.SmoothScrollBy(delta);

    if (!was_scrolling && viewport_.IsSmoothScrolling()) {
        last_scroll_time_ = std::chrono::steady_clock::now();
        InvalidateHitPositions();
    }
    // WM_PAINTループでスクロールを駆動するため再描画を要求
    if (viewport_.IsSmoothScrolling()) {
        auto layout = GetPaneLayout();
        InvalidateMdPane(layout.md_rect);
    }
}

void App::UpdateSmoothScroll()
{
    auto now = std::chrono::steady_clock::now();
    float dt_ms = std::chrono::duration<float, std::milli>(now - last_scroll_time_).count();
    last_scroll_time_ = now;

    viewport_.UpdateSmoothScroll(dt_ms);
}

void App::InvalidateMdPane(const PaneRect& md_rect)
{
    if (!renderer_.GetRenderTarget()) {
        Invalidate();
        return;
    }
    float scale = cached_dpi_scale_;
    RECT rc;
    rc.left = static_cast<LONG>(md_rect.x * scale);
    rc.top = 0;
    rc.right = static_cast<LONG>((md_rect.x + md_rect.width) * scale) + 1;
    rc.bottom = static_cast<LONG>(md_rect.height * scale) + 1;
    InvalidateRect(hwnd_, &rc, FALSE);
}

void App::StopSmoothScroll()
{
    if (!viewport_.IsSmoothScrolling()) {
        return;
    }
    viewport_.StopSmoothScroll();
}

void App::SyncMaxScroll(float md_pane_height)
{
    float total = layout_service_->GetTotalHeight();
    viewport_.SyncMaxScroll(total, md_pane_height);
}

int App::FindFirstVisibleNode() const
{
    return viewport_.FindFirstVisibleNode(layout_cache_, doc_.GetNodes().size());
}

App::AnchorState App::SaveAnchor() const
{
    AnchorState a;
    a.idx = FindFirstVisibleNode();
    a.y_before = (a.idx >= 0) ? layout_cache_[a.idx].y_position : 0.0f;
    a.offset = viewport_.GetScrollY() - a.y_before;
    return a;
}

void App::RestoreAnchor(const AnchorState& anchor, float md_pane_height)
{
    viewport_.AnchorCompensateScroll(anchor.idx, anchor.y_before, layout_cache_);
    SyncMaxScroll(md_pane_height);
}

void App::RestoreAnchorWithScale(const AnchorState& anchor, float offset_scale)
{
    if (anchor.idx >= 0 && anchor.idx < static_cast<int>(doc_.GetNodes().size())) {
        float anchor_y_after = layout_cache_[anchor.idx].y_position;
        viewport_.SetScrollY(anchor_y_after + anchor.offset * offset_scale);
    }
    viewport_.SetScrollTarget(viewport_.GetScrollY());
}

// ============================================================
// 遅延レイアウト
// ============================================================

void App::ScheduleDeferredLayoutIfNeeded()
{
    if (layout_service_->HasDirtyNodes()) {
        SetTimer(hwnd_, TIMER_DEFERRED_LAYOUT, 16, nullptr);
    }
}

void App::OnResizeEnd()
{
    KillTimer(hwnd_, TIMER_DEFERRED_LAYOUT);

    auto pane_layout = GetPaneLayout();
    float md_width = pane_layout.md_rect.width;
    float md_height = pane_layout.md_rect.height;

    layout_service_->ViewportLayout(doc_, layout_cache_, md_width, md_height);

    SyncMaxScroll(md_height);
    UpdateScrollBar();
    Invalidate();

    ScheduleDeferredLayoutIfNeeded();

    RequestMermaidRenders();
}

void App::RefreshPaneLayout()
{
    InvalidatePaneLayoutCache();
    renderer_.InvalidateFilePaneCache();
    renderer_.InvalidateTocPaneCache();
    OnResizeEnd();
}

void App::OnDeferredLayout()
{
    auto anchor = SaveAnchor();

    auto pane_layout = GetPaneLayout();
    float md_width = pane_layout.md_rect.width;
    float md_height = pane_layout.md_rect.height;
    bool more = layout_service_->ProcessDirtyBatch(doc_, layout_cache_, md_width, 200);

    if (!viewport_.IsScrollbarTracking()) {
        RestoreAnchor(anchor, md_height);
    }
    else {
        SyncMaxScroll(md_height);
    }

    if (!more) {
        KillTimer(hwnd_, TIMER_DEFERRED_LAYOUT);
        UpdateScrollBar();
        Invalidate();
    }
}

void App::UpdateLayoutAndScroll(float desired_scroll)
{
    auto pane_layout = GetPaneLayout();
    float md_width = pane_layout.md_rect.width;
    float md_height = pane_layout.md_rect.height;
    layout_service_->FullLayout(doc_, layout_cache_, md_width);

    viewport_.SetScrollY(desired_scroll);
    viewport_.SetScrollTarget(desired_scroll);
    SyncMaxScroll(md_height);

    UpdateScrollBar();
    Invalidate();
}
