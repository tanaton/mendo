#include "app.h"
#include "pane_layout.h"
#include "profiler.h"
#include <windows.h>
#include <algorithm>

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
    hover_throttle_.Reset();
    ClearTooltip();
}

void App::ScrollTo(float position)
{
    scroll_restore_.pending_restore_scroll_y = -1;
    viewport_.ScrollTo(position);
    InvalidateHitPositions();
}

void App::InvalidateMdPane(const PaneRect& md_rect)
{
    if (!renderer_.GetRenderTarget()) {
        Invalidate();
        return;
    }
    const float scale = cached_dpi_scale_;
    RECT rc;
    rc.left = static_cast<LONG>(md_rect.x * scale);
    rc.top = 0;
    rc.right = static_cast<LONG>((md_rect.x + md_rect.width) * scale) + 1;
    rc.bottom = static_cast<LONG>(md_rect.height * scale) + 1;
    InvalidateRect(hwnd_, &rc, FALSE);
}

void App::SyncMaxScroll(float md_pane_height)
{
    const float total = layout_service_->GetTotalHeight();
    viewport_.SyncMaxScroll(total, md_pane_height);
}

int App::FindFirstVisibleNode() const noexcept
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
}

void App::HandleSidePaneScrollDrag(float dip_y, const PaneRect& rect,
    float total_content, ScrollState& scroll,
    void (Renderer::*invalidate)())
{
    const auto info = ComputePaneScrollInfo(rect, total_content);
    bool dirty = false;
    HandleScrollbarDrag(dip_y, info, scroll, dirty);
    if (dirty) {
        (renderer_.*invalidate)();
    }
}

// ============================================================
// 遅延レイアウト
// ============================================================

void App::ScheduleDeferredLayoutIfNeeded()
{
    if (layout_service_->HasDirtyNodes()) {
        SetTimer(hwnd_, app_timer::DEFERRED_LAYOUT, 16, nullptr);
    }
}

void App::OnResizeEnd()
{
    MENDO_PROFILE("OnResizeEnd");

    KillTimer(hwnd_, app_timer::DEFERRED_LAYOUT);

    const auto pane_layout = GetPaneLayout();
    const float md_width = pane_layout.md_rect.width;
    const float md_height = pane_layout.md_rect.height;

    {
        MENDO_PROFILE("ViewportLayout(Resize)");
        layout_service_->ViewportLayout(doc_, layout_cache_, md_width, md_height);
    }

    SyncMaxScroll(md_height);
    UpdateScrollBar();
    Invalidate();

    ScheduleDeferredLayoutIfNeeded();

    resource_manager_.RequestMermaidRenders();
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
    MENDO_PROFILE("OnDeferredLayout");

    const auto anchor = SaveAnchor();

    const auto pane_layout = GetPaneLayout();
    const float md_width = pane_layout.md_rect.width;
    const float md_height = pane_layout.md_rect.height;
    bool more;
    {
        MENDO_PROFILE("ProcessDirtyBatch");
        more = layout_service_->ProcessDirtyBatch(doc_, layout_cache_, md_width, 200, ResourceManager::BATCH_TIME_BUDGET_US, md_height, ResourceManager::EVICT_BUFFER_SCREENS);
    }

#if MENDO_PROFILE_ENABLED
    {
        wchar_t buf[128];
        _snwprintf_s(buf, std::ranges::size(buf), _TRUNCATE, L"[mendo-profile] DeferredLayout: more=%d dirty=%d\n",
            more ? 1 : 0, layout_service_->HasDirtyNodes() ? 1 : 0);
        OutputDebugStringW(buf);
    }
#endif

    if (!viewport_.IsScrollbarTracking()) {
        // 中間バッチではアンカー補償のみ行い、SyncMaxScrollのクランプを遅延させる。
        // ビューポート後のノードが計測されるとtotal_heightが縮小し、中間的な
        // max_scrollに基づくクランプでscroll_yが不当に引き下げられるのを防ぐ。
        // 最終バッチでもMermaidレンダリング後までクランプを遅延させる（後述）。
        viewport_.AnchorCompensateScroll(anchor.idx, anchor.y_before, layout_cache_);
    }
    else {
        SyncMaxScroll(md_height);
    }

    if (!more) {
        KillTimer(hwnd_, app_timer::DEFERRED_LAYOUT);

        // 遅延レイアウト完了: Mermaidファイルキャッシュからの読み込みを
        // 時間予算付きバッチで処理する。同期ディスクI/O + PNGデコードが
        // UIスレッドを長時間ブロックするのを防ぐ。
        resource_manager_.ScheduleMermaidBatch();

        // 全レイアウト確定後に前回セッションの生のscroll_yを適用する
        if (scroll_restore_.pending_restore_scroll_y >= 0) {
            viewport_.SetScrollY(static_cast<float>(scroll_restore_.pending_restore_scroll_y));
            scroll_restore_.pending_restore_scroll_y = -1;
        }

        SyncMaxScroll(md_height);

        UpdateScrollBar();
        Invalidate();
    }
}

