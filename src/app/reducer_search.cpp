#include "reducer_internal.h"
#include <concepts>
#include <utility>

namespace {

// SearchBarController は ScrollToCurrentMatch で viewport を直接動かすため、
// 呼び出し前後の scroll_y を比べて通常スクロールと同じ副作用を積む。
template <std::invocable F>
void RunSearchWithScrollEffects(AppState& state, SideEffectList& effects, F&& f)
{
    const float old_scroll = state.view.viewport.GetScrollY();
    std::forward<F>(f)();
    EmitScrollEffects(state, effects, old_scroll);
}

} // namespace

void ReduceSearchStep(AppState& state, SideEffectList& effects, bool forward)
{
    auto& ss = state.search;
    if (!ss.search_state.IsVisible()) {
        ss.search_bar_ctrl.OnOpen(state.document.doc.GetNodes());
        return;
    }
    RunSearchWithScrollEffects(state, effects, [&] {
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
    RunSearchWithScrollEffects(state, effects, [&] {
        state.search.search_bar_ctrl.OnTextChanged(a.text, doc.GetNodes(), doc.GetRawText().size());
    });
}

void ReduceSearchDebounce(AppState& state, SideEffectList& effects)
{
    RunSearchWithScrollEffects(state, effects, [&] {
        state.search.search_bar_ctrl.OnDebounceTimer(state.document.doc.GetNodes());
    });
}
