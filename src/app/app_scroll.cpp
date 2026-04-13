#include "app.h"
#include "app_constants.h"
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
    state_.hover_throttle.Reset();
    ClearTooltip();
}

void App::ScrollTo(float position)
{
    state_.scroll_restore.pending_restore_scroll_y = -1;
    state_.viewport.ScrollTo(position);
    InvalidateHitPositions();
}

void App::InvalidateMdPane(const PaneRect& md_rect)
{
    if (!renderer_.GetRenderTarget()) {
        Invalidate();
        return;
    }
    // MDペインはタイトルバーを含む縦ストリップ全体を無効化する
    InvalidatePane(PaneRect{ md_rect.x, 0.0f, md_rect.width, md_rect.y + md_rect.height });
}

void App::SyncMaxScroll(float md_pane_height)
{
    const float total = layout_service_->GetTotalHeight();
    state_.cached_total_height = total;
    state_.viewport.SyncMaxScroll(total, md_pane_height);
}

int App::FindFirstVisibleNode() const noexcept
{
    return state_.viewport.FindFirstVisibleNode(state_.layout_cache, state_.doc.GetNodes().size());
}

AnchorState App::SaveAnchor() const
{
    return SaveAnchorFromState(state_);
}

void App::RestoreAnchor(const AnchorState& anchor, float md_pane_height)
{
    state_.viewport.AnchorCompensateScroll(anchor.idx, anchor.y_before, state_.layout_cache);
    SyncMaxScroll(md_pane_height);
}

void App::RestoreAnchorWithScale(const AnchorState& anchor, float offset_scale)
{
    if (anchor.idx >= 0 && anchor.idx < static_cast<int>(state_.doc.GetNodes().size())) {
        float anchor_y_after = state_.layout_cache[anchor.idx].y_position;
        state_.viewport.SetScrollY(anchor_y_after + anchor.offset * offset_scale);
    }
}

void App::HandleSidePaneScrollDrag(float dip_y, const PaneRect& rect,
    float total_content, ScrollState& scroll,
    void (Renderer::* invalidate)())
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
        SetTimer(hwnd_, app_timer::DEFERRED_LAYOUT, app_timer::FRAME_INTERVAL_MS, nullptr);
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
        layout_service_->ViewportLayout(state_.doc, state_.layout_cache, md_width, md_height);
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
        more = layout_service_->ProcessDirtyBatch(state_.doc, state_.layout_cache, md_width, 200, ResourceManager::BATCH_TIME_BUDGET_US, md_height, ResourceManager::EVICT_BUFFER_SCREENS);
    }

#if MENDO_PROFILE_ENABLED
    {
        wchar_t buf[128];
        _snwprintf_s(buf, std::ranges::size(buf), _TRUNCATE, L"[mendo-profile] DeferredLayout: more=%d dirty=%d\n",
            more ? 1 : 0, layout_service_->HasDirtyNodes() ? 1 : 0);
        OutputDebugStringW(buf);
    }
#endif

    if (!state_.viewport.IsScrollbarTracking()) {
        // 中間バッチではアンカー補償のみ行い、SyncMaxScrollのクランプを遅延させる。
        // ビューポート後のノードが計測されるとtotal_heightが縮小し、中間的な
        // max_scrollに基づくクランプでscroll_yが不当に引き下げられるのを防ぐ。
        // 最終バッチでもMermaidレンダリング後までクランプを遅延させる（後述）。
        state_.viewport.AnchorCompensateScroll(anchor.idx, anchor.y_before, state_.layout_cache);
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
        if (state_.scroll_restore.pending_restore_scroll_y >= 0.0f) {
            state_.viewport.SetScrollY(state_.scroll_restore.pending_restore_scroll_y);
            state_.scroll_restore.pending_restore_scroll_y = -1.0f;
        }

        SyncMaxScroll(md_height);

        UpdateScrollBar();
        Invalidate();
    }
}

