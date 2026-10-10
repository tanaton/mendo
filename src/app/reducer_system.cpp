#include "reducer_internal.h"
#include "app_constants.h"
#include "ui_constants.h"
#include <utility>

namespace {

bool StepZoom(ViewportManager& viewport, ZoomDirection direction)
{
    switch (direction) {
    case ZoomDirection::In:
        return viewport.ZoomIn();
    case ZoomDirection::Out:
        return viewport.ZoomOut();
    case ZoomDirection::Reset:
        return viewport.ZoomReset();
    }
    std::unreachable();
}

} // namespace

void ReduceZoom(AppState& state, SideEffectList& effects, const ZoomAction& a)
{
    if (!StepZoom(state.view.viewport, a.direction)) {
        return;
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
    PushEffect(effects, effect::ApplyThemeChange{ effect::ApplyThemeChange::Type::Zoom });
}

void ReduceToggleDarkMode(AppState& state, SideEffectList& effects)
{
    const auto anchor = SnapshotVisibleTarget(state);
    state.pane_layout_cache.Invalidate();
    // 色のみの変更なのでテキストレイアウトは維持する。描画エフェクトの固定ブラシは SetColor で
    // 色だけ差し替わるので再適用不要で、テーマ色を焼き込んだ Mermaid bitmap のみ破棄する。
    state.document.layout_cache.InvalidateDiagramBitmaps(state.document.doc.GetNodes());
    if (anchor.IsValid()) {
        // Mermaid 再レンダリングで微小な高さ変化が起きるので target で追従する。
        state.view.viewport.SetScrollTarget(anchor.node, anchor.offset);
    }
    PushEffect(effects, effect::ApplyThemeChange{ effect::ApplyThemeChange::Type::DarkMode });
}

void ReduceActivate(AppState& state, SideEffectList& effects, const ActivateAction& a)
{
    if (state.window.window_active != a.active) {
        state.window.window_active = a.active;
        PushEffect(effects, effect::InvalidateWindow{});
        state.search.search_bar_ctrl.OnWindowActivate(a.active);
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
    state.window.size_changed_in_sizing = true;
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

void ReduceExitSizeMove(AppState& state, SideEffectList& effects)
{
    state.window.is_sizing = false;
    if (state.window.size_changed_in_sizing) {
        PushEffect(effects, effect::PerformResizeEnd{});
    }
}

void ReduceDpiChanged(AppState& state, SideEffectList& effects, const DpiChangedAction& a)
{
    state.window.cached_dpi_scale = DpiScaleFrom(static_cast<float>(a.dpi));
    // ピクセルサイズ不変でも DIP サイズが変わるため、移動ループ終了時のレイアウトを要求する。
    state.window.size_changed_in_sizing = true;
    state.pane_layout_cache.Invalidate();
    // DPI 変更では IDWriteTextLayout (DIP 単位) は不変。effects_generation のみ進める。
    state.document.layout_cache.NotifyDpiChanged();
    PushEffect(effects, effect::RendererSetDpi{ static_cast<float>(a.dpi) });
    PushEffect(effects, effect::ClearFileCache{});
    const auto& rc = a.suggested;
    PushEffect(effects, effect::SetWindowPosition{ rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top });
}

void ReduceTimer(AppState& state, SideEffectList& effects, const TimerAction& a)
{
    switch (a.timer_id) {
    case app_timer::Id::TOAST: {
        auto& toast = state.interaction.toast;
        // ホールド期間は描画 alpha が 1 に張り付くため、見た目が変わる tick だけ再描画する。
        const float before = toast.GetRenderAlpha();
        if (!toast.Tick()) {
            PushEffect(effects, effect::KillTimer{ app_timer::Id::TOAST });
            PushEffect(effects, effect::InvalidateWindow{});
        }
        else if (toast.GetRenderAlpha() != before) {
            PushEffect(effects, effect::InvalidateWindow{});
        }
        return;
    }
    case app_timer::Id::TOOLTIP:
        PushEffect(effects, effect::KillTimer{ app_timer::Id::TOOLTIP });
        state.interaction.tooltip.Show();
        return;
    case app_timer::Id::SEARCH_DEBOUNCE:
        ReduceSearchDebounce(state, effects);
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
    case app_timer::Id::MERMAID_IDLE:
        PushEffect(effects, effect::MermaidIdle{});
        return;
    case app_timer::Id::FILE_RELOAD_DEBOUNCE:
        PushEffect(effects, effect::KillTimer{ app_timer::Id::FILE_RELOAD_DEBOUNCE });
        PushEffect(effects, effect::ReloadFile{});
        return;
    }
    std::unreachable();
}
