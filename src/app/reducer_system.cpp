#include "reducer_internal.h"
#include "app_constants.h"
#include "ui_constants.h"
#include <utility>

void ReduceZoom(AppState& state, SideEffectList& effects, const ZoomAction& a)
{
    {
        bool changed = false;
        switch (a.direction) {
        case ZoomDirection::In:
            changed = state.view.viewport.ZoomIn();
            break;
        case ZoomDirection::Out:
            changed = state.view.viewport.ZoomOut();
            break;
        case ZoomDirection::Reset:
            changed = state.view.viewport.ZoomReset();
            break;
        }
        if (!changed) {
            return;
        }
    }
    const auto anchor = SnapshotVisibleTarget(state);
    state.pane_layout_cache.Invalidate();
    const float new_zoom = state.view.viewport.GetCurrentZoom();
    const float zoom_ratio = new_zoom / state.theme->zoom;
    state.view.panes.ApplyZoom(zoom_ratio);
    state.document.layout_cache.InvalidateAllLayouts();
    if (anchor.IsValid()) {
        // offset もズーム比でスケールしないと、ノード内の同じ位置が可視先頭に残らない。
        state.view.viewport.SetScrollTarget(anchor.node, anchor.offset * zoom_ratio);
    }
    PushEffect(
        effects,
        effect::ApplyThemeChange{
            .type = effect::ApplyThemeChange::Type::Zoom,
            .zoom_index = static_cast<uint8_t>(state.view.viewport.GetZoomIndex()),
        });
}

void ReduceToggleDarkMode(AppState& state, SideEffectList& effects)
{
    const auto anchor = SnapshotVisibleTarget(state);
    state.pane_layout_cache.Invalidate();
    // 色のみの変更なのでテキストレイアウトは維持し、effects と Mermaid bitmap のみ破棄する。
    state.document.layout_cache.InvalidateEffectsAndDiagramBitmaps(state.document.doc.GetNodes());
    if (anchor.IsValid()) {
        // Mermaid 再レンダリングで微小な高さ変化が起きるので target で追従する。
        state.view.viewport.SetScrollTarget(anchor.node, anchor.offset);
    }
    PushEffect(
        effects,
        effect::ApplyThemeChange{
            .type = effect::ApplyThemeChange::Type::DarkMode,
            .zoom_index = static_cast<uint8_t>(state.view.viewport.GetZoomIndex()),
        });
}

void ReduceActivate(AppState& state, SideEffectList& effects, const ActivateAction& a)
{
    if (state.window.window_active != a.active) {
        state.window.window_active = a.active;
        PushEffect(effects, effect::InvalidateTitleBar{});
    }
    if (!a.active) {
        ClearTooltip(state, effects);
    }
}

void ReduceResize(AppState& state, SideEffectList& effects, const ResizeAction& a)
{
    if (a.width == 0 || a.height == 0) {
        return;
    }
    state.pane_layout_cache.Invalidate();
    PushEffect(effects, effect::RendererResize{ a.width, a.height });
    const float window_w_dip = a.width / state.window.cached_dpi_scale;
    state.window.titlebar.UpdateLayout(window_w_dip);
    if (state.window.is_sizing) {
        PushEffect(effects, effect::PerformSizingUpdate{});
    }
    else {
        PushEffect(effects, effect::PerformResizeEnd{});
    }
}

void ReduceDpiChanged(AppState& state, SideEffectList& effects, const DpiChangedAction& a)
{
    state.window.cached_dpi_scale = DpiScaleFrom(static_cast<float>(a.dpi));
    state.pane_layout_cache.Invalidate();
    // DPI 変更では IDWriteTextLayout (DIP 単位) は不変。effects_generation のみ進める。
    state.document.layout_cache.NotifyDpiChanged();
    PushEffect(effects, effect::RendererSetDpi{ static_cast<float>(a.dpi) });
    PushEffect(effects, effect::ClearFileCache{});
    PushEffect(
        effects,
        effect::SetWindowPosition{
            static_cast<int>(a.suggested.left),
            static_cast<int>(a.suggested.top),
            static_cast<int>(a.suggested.right - a.suggested.left),
            static_cast<int>(a.suggested.bottom - a.suggested.top),
        });
}

void ReduceTimer(AppState& state, SideEffectList& effects, const TimerAction& a)
{
    switch (a.timer_id) {
    case app_timer::Id::TOAST:
        if (!state.interaction.toast.Tick()) {
            PushEffect(effects, effect::KillTimer{ app_timer::Id::TOAST });
        }
        PushEffect(effects, effect::InvalidateWindow{});
        return;
    case app_timer::Id::SEARCH_CARET:
        state.search.search_bar_ctrl.OnCaretBlinkTimer();
        return;
    case app_timer::Id::TOOLTIP:
        PushEffect(effects, effect::KillTimer{ app_timer::Id::TOOLTIP });
        state.interaction.tooltip.Show();
        return;
    case app_timer::Id::SEARCH_DEBOUNCE:
        state.search.search_bar_ctrl.OnDebounceTimer(state.document.doc.GetNodes());
        return;
    case app_timer::Id::SWIPE_OVERLAY: {
        const auto result = state.interaction.swipe_detector.Commit();
        PushEffect(effects, effect::KillTimer{ app_timer::Id::SWIPE_OVERLAY });
        switch (result) {
        case SwipeResult::None:
            return;
        case SwipeResult::Back:
            ReduceNavigateBack(state, effects);
            PushEffect(effects, effect::InvalidateWindow{});
            return;
        case SwipeResult::Forward:
            ReduceNavigateForward(state, effects);
            PushEffect(effects, effect::InvalidateWindow{});
            return;
        }
        std::unreachable();
    }
    case app_timer::Id::DEFERRED_LAYOUT:
        PushEffect(effects, effect::ProcessDeferredLayout{});
        return;
    case app_timer::Id::LOADING_ANIM:
        PushEffect(effects, effect::TickLoadingAnimation{});
        PushEffect(effects, effect::InvalidateWindow{});
        return;
    case app_timer::Id::MERMAID_BATCH:
        PushEffect(effects, effect::ProcessMermaidBatchTimer{});
        return;
    case app_timer::Id::BITMAP_MANAGE:
        PushEffect(effects, effect::ProcessBitmapManage{});
        return;
    case app_timer::Id::MERMAID_INIT_RETRY:
        PushEffect(effects, effect::MermaidInitRetry{});
        return;
    case app_timer::Id::FILE_RELOAD_DEBOUNCE:
        PushEffect(effects, effect::KillTimer{ app_timer::Id::FILE_RELOAD_DEBOUNCE });
        PushEffect(effects, effect::ReloadFile{});
        return;
    }
    std::unreachable();
}
