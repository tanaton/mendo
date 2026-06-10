#include "reducer_internal.h"
#include "selection_html.h"

void ReduceTogglePane(AppState& state, SideEffectList& effects, const TogglePaneAction& a)
{
    state.view.panes.ToggleSidePane(a.target);
    state.pane_layout_cache.Invalidate();
    PushEffect(effects, effect::RefreshPaneLayout{});
    if (a.target == PaneTarget::Toc) {
        PushEffect(effects, effect::SyncTocActive{});
    }
}

void ReduceSelectAll(AppState& state, SideEffectList& effects)
{
    state.view.viewport.SelectAll(state.document.doc.GetNodes());
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceClearSelection(AppState& state, SideEffectList& effects)
{
    if (state.search.search_state.IsVisible()) {
        state.search.search_bar_ctrl.OnClose();
    }
    else {
        state.view.viewport.ClearSelection();
    }
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceCopyClipboard(const AppState& state, SideEffectList& effects)
{
    if (state.view.viewport.GetSelection().active) {
        PushEffect(effects, effect::ClipboardWrite{ ExtractSelectedText(state.document.doc.GetNodes(), state.view.viewport.GetSelection()) });
    }
}

void ReduceCopyFormattedClipboard(const AppState& state, SideEffectList& effects)
{
    const auto& sel = state.view.viewport.GetSelection();
    if (!sel.active) {
        return;
    }
    const auto& nodes = state.document.doc.GetNodes();
    PushEffect(effects, effect::ClipboardWriteHtml{ ExtractSelectedTextAsHtml(nodes, sel, state.theme->IsDark()), ExtractSelectedText(nodes, sel) });
}
