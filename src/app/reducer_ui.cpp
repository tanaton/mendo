#include "reducer_internal.h"
#include "document_utils.h"
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

void ReduceSelectWord(AppState& state, SideEffectList& effects, const SelectWordAction& a)
{
    const auto& nodes = state.document.doc.GetNodes();
    if (a.node_index < 0 || a.node_index >= static_cast<int>(nodes.size())) {
        return;
    }
    const std::string_view text = nodes[a.node_index].LinearizedText();
    if (text.empty()) {
        return;
    }
    const auto wb = FindWordBoundaries(text, a.text_pos);
    if (!wb.found) {
        return;
    }
    state.view.viewport.SetAnchor(a.node_index, wb.start);
    state.view.viewport.SetSelection(TextSelection::MakeOrdered(a.node_index, wb.start, a.node_index, wb.end));
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceClearSelection(AppState& state, SideEffectList& effects)
{
    if (state.search.search_state.IsVisible()) {
        state.search.search_bar_ctrl.OnClose();
    }
    else {
        // ClearSelection はドラッグ状態も落とすため、テキスト選択ドラッグ中だと後続の
        // LButtonUp が TextSelectionEnded に入らずキャプチャが残る。
        if (state.view.viewport.IsDragging()) {
            PushEffect(effects, effect::ReleaseCapture{});
        }
        state.view.viewport.ClearSelection();
    }
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceCopyClipboard(const AppState& state, SideEffectList& effects)
{
    const auto& sel = state.view.viewport.GetSelection();
    if (!sel.active) {
        return;
    }
    PushEffect(effects, effect::ClipboardWrite{ ExtractSelectedText(state.document.doc.GetNodes(), sel) });
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
