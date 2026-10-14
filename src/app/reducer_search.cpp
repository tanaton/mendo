#include "reducer_internal.h"

// SearchBarController は ScrollToCurrentMatch で viewport を直接動かすため WithScrollEffects で包む。

void ReduceSearchStep(AppState& state, SideEffectList& effects, bool forward)
{
    auto& ss = state.search;
    if (!ss.search_state.IsVisible()) {
        ss.search_bar_ctrl.OnOpen(state.document.doc.GetNodes());
        return;
    }
    WithScrollEffects(state, effects, [&] {
        if (forward) {
            ss.search_bar_ctrl.OnNext();
        }
        else {
            ss.search_bar_ctrl.OnPrev();
        }
    });
}

void ReduceSearchTextChanged(AppState& state, SideEffectList& effects, const SearchTextChangedAction& a)
{
    const auto& doc = state.document.doc;
    WithScrollEffects(state, effects, [&] {
        state.search.search_bar_ctrl.OnTextChanged(a.text, doc.GetNodes(), doc.GetRawText().size());
    });
}

void ReduceSearchDebounce(AppState& state, SideEffectList& effects)
{
    WithScrollEffects(state, effects, [&] {
        state.search.search_bar_ctrl.OnDebounceTimer(state.document.doc.GetNodes());
    });
}
