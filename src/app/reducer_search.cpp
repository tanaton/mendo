#include "reducer_internal.h"

void ReduceSearchStep(AppState& state, bool forward)
{
    auto& ss = state.search;
    if (ss.search_state.IsVisible()) {
        if (forward) {
            ss.search_bar_ctrl.OnNext();
        }
        else {
            ss.search_bar_ctrl.OnPrev();
        }
    }
    else {
        ss.search_bar_ctrl.OnOpen(state.document.doc.GetNodes());
    }
}
