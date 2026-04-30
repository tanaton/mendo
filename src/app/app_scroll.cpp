#include "app.h"
#include "app_constants.h"
#include "app_events.h"
#include "pane_layout.h"
#include "profiler.h"
#include "toc.h"
#include <windows.h>
#include <algorithm>

// ============================================================
// スクロールバー・スクロール
// ============================================================

void App::InvalidateHitPositions()
{
    state_.interaction.hover_throttle.Reset();
    Dispatch(ClearTooltipAction{});
}

void App::ScrollTo(float position)
{
    state_.view.viewport.ScrollTo(position);
    InvalidateHitPositions();
    EmitEffect(effect::SyncTocActive{});
}

void App::SyncTocActiveAndAutoScroll()
{
    if (!state_.view.panes.IsTocPaneVisible()) {
        return;
    }
    const auto& layout = GetPaneLayout();
    const auto& theme = renderer_.GetTheme();
    const float toc_margin = layout.md_rect.y + theme.heading_spacing_above;
    const int new_active = state_.document.doc.GetToc().FindActiveIndex(
        state_.document.layout_cache, state_.view.viewport.GetScrollY(), toc_margin);
    if (new_active == state_.active_toc_index) {
        return;
    }
    state_.active_toc_index = new_active;
    renderer_.InvalidateTocPaneCache();

    if (new_active < 0) {
        return;
    }

    // アクティブ見出しが目次ペインの表示範囲外なら自動スクロール
    const float item_y = static_cast<float>(new_active) * theme.pane_item_height;
    const float total = static_cast<float>(state_.document.doc.GetToc().GetEntries().size()) * theme.pane_item_height;
    const auto info = ComputePaneScrollInfo(layout.toc_rect, total);
    auto& toc_scroll = state_.view.panes.TocScroll();
    toc_scroll.max_scroll = info.max_scroll;
    float& sy = toc_scroll.scroll_y;
    sy = std::clamp(sy, 0.0f, info.max_scroll);
    if (info.content_height > 0.0f) {
        // フォーカスを表示領域の5等分中、区画2〜4(1/5〜4/5)に留める
        const float zone_upper = info.content_height * (1.0f / 5.0f);
        const float zone_lower = info.content_height * (4.0f / 5.0f);
        if (item_y < sy + zone_upper) {
            sy = std::clamp(item_y - zone_upper, 0.0f, info.max_scroll);
        }
        else if (item_y + theme.pane_item_height > sy + zone_lower) {
            sy = std::clamp(item_y + theme.pane_item_height - zone_lower, 0.0f, info.max_scroll);
        }
    }
}

void App::InvalidateMdPane(const PaneRect& md_rect)
{
    if (!IsRenderReady()) {
        Invalidate();
        return;
    }
    // MDペインは本文領域のみ無効化する。タイトルバーは別途 InvalidateTitleBar() を使う。
    InvalidatePane(md_rect);
}

int App::FindFirstVisibleNode() const noexcept
{
    return state_.view.viewport.FindFirstVisibleNode(state_.document.layout_cache, state_.document.doc.GetNodes().size());
}

void App::EnsureScrollTarget()
{
    state_.view.viewport.EnsureScrollTarget(
        state_.document.layout_cache, state_.document.doc.GetNodes().size());
}

// ============================================================
// 遅延レイアウト
// ============================================================

void App::ScheduleDeferredLayoutIfNeeded()
{
    if (layout_service_->HasDirtyNodes()) {
        EmitEffect(effect::SetTimer{ app_timer::DEFERRED_LAYOUT, app_timer::FRAME_INTERVAL_MS });
    }
}

void App::OnResizeEnd()
{
    MENDO_PROFILE("OnResizeEnd");

    EmitEffect(effect::KillTimer{ app_timer::DEFERRED_LAYOUT });

    const auto pane_layout = GetPaneLayout();
    const float md_width = pane_layout.md_rect.width;
    const float md_height = pane_layout.md_rect.height;

    EnsureScrollTarget();

    {
        MENDO_PROFILE("ViewportLayout(Resize)");
        EmitEffect(effect::ViewportLayout{ md_width, md_height });
    }

    EmitEffect(effect::SyncMaxScroll{ md_height });
    Invalidate();

    ScheduleDeferredLayoutIfNeeded();

    resource_manager_.ScheduleMermaidBatch();

    EmitEffect(effect::SyncTocActive{});
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

    EnsureScrollTarget();

    const auto pane_layout = GetPaneLayout();
    const float md_width = pane_layout.md_rect.width;
    const float md_height = pane_layout.md_rect.height;
    bool more;
    {
        MENDO_PROFILE("ProcessDirtyBatch");
        more = layout_service_->ProcessDirtyBatch(state_.document.doc, state_.document.layout_cache, md_width, 200, ResourceManager::BATCH_TIME_BUDGET_US, md_height, ResourceManager::EVICT_BUFFER_SCREENS);
    }

#if MENDO_PROFILE_ENABLED
    {
        wchar_t buf[128];
        _snwprintf_s(buf, std::ranges::size(buf), _TRUNCATE, L"[mendo-profile] DeferredLayout: more=%d dirty=%d\n",
                     more ? 1 : 0, layout_service_->HasDirtyNodes() ? 1 : 0);
        OutputDebugStringW(buf);
    }
#endif

    // 中間バッチでは SyncMaxScroll のクランプを遅延させる。
    // ビューポート後のノードが計測されると total_height が縮小し、中間的な
    // max_scroll に基づくクランプで scroll_y が不当に引き下げられるのを防ぐ。
    // スクロールバートラッキング中はユーザー操作を優先してクランプを反映する。
    if (state_.view.viewport.IsScrollbarTracking()) {
        EmitEffect(effect::SyncMaxScroll{ md_height });
    }

    if (!more) {
        EmitEffect(effect::KillTimer{ app_timer::DEFERRED_LAYOUT });

        // 遅延レイアウト完了: Mermaidファイルキャッシュからの読み込みを
        // 時間予算付きバッチで処理する。同期ディスクI/O + PNGデコードが
        // UIスレッドを長時間ブロックするのを防ぐ。
        resource_manager_.ScheduleMermaidBatch();

        EmitEffect(effect::SyncMaxScroll{ md_height });
        Invalidate();

        // 遅延レイアウト確定で layout_cache の y_position が安定したので
        // 目次アクティブ見出しを再同期する。
        EmitEffect(effect::SyncTocActive{});
    }
}
