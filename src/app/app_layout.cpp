#include "app.h"
#include "app_constants.h"
#include "app_state_queries.h"
#include "pane_layout.h"
#include "profiler.h"

void App::EnsureScrollTarget()
{
    state_.view.viewport.EnsureScrollTarget(
        state_.document.layout_cache, state_.document.doc.GetNodes().size());
}

void App::ViewportLayout(float md_width, float md_height)
{
    layout_service_->ViewportLayout(state_.document.doc, state_.document.layout_cache, md_width, md_height);
}

void App::SyncMaxScroll(float md_height)
{
    state_.view.viewport.SyncMaxScroll(ScrollableContentHeight(), md_height);
}

float App::ScrollableContentHeight() const noexcept
{
    // WM_NCHITTEST は Init 前にも届く。
    return state_.theme ? MdScrollableContentHeight(state_) : 0.0f;
}

// Mermaid のキャッシュキーに入るため、全経路でこの値を使う (ずれるとキャッシュを外す)。
float App::MdContentWidth()
{
    return renderer_.GetTheme().ContentWidth(GetPaneLayout().md_rect.width);
}

void App::ScheduleDeferredLayoutIfNeeded()
{
    if (layout_service_->HasDirtyNodes()) {
        EmitEffect(effect::SetTimer{ app_timer::Id::DEFERRED_LAYOUT, app_timer::FRAME_INTERVAL_MS });
    }
}

void App::FinalizeLayout(float md_pane_height)
{
    resource_manager_.LoadImages();
    resource_manager_.RequestMermaidRenders();
    SyncMaxScroll(md_pane_height);
    Invalidate();
    ScheduleDeferredLayoutIfNeeded();
}

void App::InvalidateHitPositions()
{
    state_.interaction.hover_throttle.Reset();
    Dispatch(ClearTooltipAction{});
}

void App::SyncTocActiveAndAutoScroll(bool auto_scroll)
{
    if (!state_.view.panes.IsSidePaneVisible(PaneTarget::Toc)) {
        return;
    }
    const auto& theme = renderer_.GetTheme();
    const float toc_margin = GetPaneLayout().md_rect.y + theme.heading_spacing_above;
    const int new_active = state_.document.doc.GetToc().FindActiveIndex(
        state_.document.layout_cache, state_.view.viewport.GetScrollY(), toc_margin);
    if (new_active == state_.view.active_toc_index) {
        return;
    }
    state_.view.active_toc_index = new_active;
    renderer_.InvalidateSidePaneCache(PaneTarget::Toc);

    // auto_scroll=false (目次クリック由来) ではハイライト更新のみ。
    // ユーザーが操作中の目次ペインのスクロール位置を勝手に動かさない (issue#259)。
    if (!auto_scroll || new_active < 0) {
        return;
    }

    // アクティブ見出しを目次ペインの中央に保つように追従スクロールする。
    const auto ctx = GetSidePaneContext(state_, PaneTarget::Toc);
    if (ctx.info.content_height > 0.0f) {
        ctx.scroll.scroll_y = CenterPaneScrollOnItem(static_cast<size_t>(new_active), theme.pane_item_height, ctx.info);
    }
}

void App::OnResizeEnd()
{
    MENDO_PROFILE("OnResizeEnd");

    EmitEffect(effect::KillTimer{ app_timer::Id::DEFERRED_LAYOUT });

    const PaneRect md = GetPaneLayout().md_rect;

    EnsureScrollTarget();

    {
        MENDO_PROFILE("ViewportLayout(Resize)");
        ViewportLayout(md.width, md.height);
    }
    SyncMaxScroll(md.height);
    Invalidate();

    ScheduleDeferredLayoutIfNeeded();

    resource_manager_.ScheduleMermaidBatch();

    EmitEffect(effect::SyncTocActive{});
}

void App::RefreshPaneLayout()
{
    state_.pane_layout_cache.Invalidate();
    renderer_.InvalidateAllSidePaneCaches();
    EmitEffect(effect::PerformResizeEnd{});
}

void App::OnDeferredLayout()
{
    MENDO_PROFILE("OnDeferredLayout");

    // 1 回の遅延レイアウトで計測するノード数の上限。
    constexpr int kDeferredLayoutBatchNodes = 200;

    EnsureScrollTarget();

    const PaneRect md = GetPaneLayout().md_rect;
    bool more;
    {
        MENDO_PROFILE("ProcessDirtyBatch");
        more = layout_service_->ProcessDirtyBatch(
            state_.document.doc, state_.document.layout_cache, md.width, kDeferredLayoutBatchNodes,
            ResourceManager::BATCH_TIME_BUDGET_US, LayoutService::ViewportLimit{ md.height, ResourceManager::EVICT_BUFFER_SCREENS });
    }

    // 中間バッチでは SyncMaxScroll のクランプを遅延させる。
    // ビューポート後のノードが計測されると total_height が縮小し、中間的な
    // max_scroll に基づくクランプで scroll_y が不当に引き下げられるのを防ぐ。
    // スクロールバートラッキング中はユーザー操作を優先してクランプを反映する。
    if (state_.view.panes.GetDragTarget() == PaneController::DragTarget::MdScrollbar) {
        SyncMaxScroll(md.height);
    }

    if (more) {
        return;
    }
    EmitEffect(effect::KillTimer{ app_timer::Id::DEFERRED_LAYOUT });

    // 同期ディスク I/O + PNG デコードで UI スレッドを長時間ブロックしないよう、
    // Mermaid ファイルキャッシュの読み込みは時間予算付きバッチに回す。
    resource_manager_.ScheduleMermaidBatch();

    SyncMaxScroll(md.height);
    Invalidate();

    // layout_cache の text_top が確定したので目次アクティブ見出しを再同期する。
    EmitEffect(effect::SyncTocActive{});
}
