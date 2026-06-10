#include "app.h"
#include "app_constants.h"
#include "app_events.h"
#include "pane_layout.h"
#include "profiler.h"
#include "toc.h"
#include <windows.h>
#include <algorithm>

void App::InvalidateHitPositions()
{
    state_.interaction.hover_throttle.Reset();
    Dispatch(ClearTooltipAction{});
}

void App::SyncTocActiveAndAutoScroll()
{
    if (!state_.view.panes.IsSidePaneVisible(PaneTarget::Toc)) {
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
    renderer_.InvalidateSidePaneCache(PaneTarget::Toc);

    if (new_active < 0) {
        return;
    }

    // アクティブ見出しが目次ペインの表示範囲から外れていたら、自動スクロールで追従する。
    const float item_y = static_cast<float>(new_active) * theme.pane_item_height;
    const float total = SidePaneContentHeight(state_.document.doc.GetToc().GetEntries().size(), theme.pane_item_height);
    const auto info = ComputePaneScrollInfo(layout.toc_rect, total);
    auto& toc_scroll = state_.view.panes.SidePaneScroll(PaneTarget::Toc);
    float& sy = toc_scroll.scroll_y;
    sy = std::clamp(sy, 0.0f, info.max_scroll);
    if (info.content_height > 0.0f) {
        // フォーカスを表示領域の 1/5〜4/5 帯に留める（端ぴったりだと前後が見えにくい）。
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
    // MD ペインの本文領域のみ無効化（タイトルバーは別途 InvalidateTitleBar() で扱う）。
    InvalidatePane(md_rect);
}

void App::EnsureScrollTarget()
{
    state_.view.viewport.EnsureScrollTarget(
        state_.document.layout_cache, state_.document.doc.GetNodes().size());
}

void App::ScheduleDeferredLayoutIfNeeded()
{
    if (layout_service_->HasDirtyNodes()) {
        EmitEffect(effect::SetTimer{ app_timer::Id::DEFERRED_LAYOUT, app_timer::FRAME_INTERVAL_MS });
    }
}

void App::OnResizeEnd()
{
    MENDO_PROFILE("OnResizeEnd");

    EmitEffect(effect::KillTimer{ app_timer::Id::DEFERRED_LAYOUT });

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
    renderer_.InvalidateAllSidePaneCaches();
    EmitEffect(effect::PerformResizeEnd{});
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
        more = layout_service_->ProcessDirtyBatch(state_.document.doc, state_.document.layout_cache, md_width, 200, ResourceManager::BATCH_TIME_BUDGET_US, LayoutService::ViewportLimit{ md_height, ResourceManager::EVICT_BUFFER_SCREENS });
    }

    // 中間バッチでは SyncMaxScroll のクランプを遅延させる。
    // ビューポート後のノードが計測されると total_height が縮小し、中間的な
    // max_scroll に基づくクランプで scroll_y が不当に引き下げられるのを防ぐ。
    // スクロールバートラッキング中はユーザー操作を優先してクランプを反映する。
    if (state_.view.viewport.IsScrollbarTracking()) {
        EmitEffect(effect::SyncMaxScroll{ md_height });
    }

    if (!more) {
        EmitEffect(effect::KillTimer{ app_timer::Id::DEFERRED_LAYOUT });

        // 遅延レイアウト完了後、Mermaid ファイルキャッシュからの読み込みを時間予算付き
        // バッチで処理する。同期ディスク I/O + PNG デコードが UI スレッドを長時間
        // ブロックするのを防ぐ。
        resource_manager_.ScheduleMermaidBatch();

        EmitEffect(effect::SyncMaxScroll{ md_height });
        Invalidate();

        // 遅延レイアウト確定で layout_cache の text_top が安定したので
        // 目次アクティブ見出しを再同期する。
        EmitEffect(effect::SyncTocActive{});
    }
}
