#include "app_state.h"

AnchorState SaveAnchorFromState(const AppState& state) noexcept
{
    AnchorState a;
    a.idx = state.view.viewport.FindFirstVisibleNode(state.document.layout_cache, state.document.doc.GetNodes().size());
    a.y_before = (a.idx >= 0) ? state.document.layout_cache[a.idx].y_position : 0.0f;
    a.offset = state.view.viewport.GetScrollY() - a.y_before;
    return a;
}

void PushCurrentNavEntry(AppState& state)
{
    state.view.nav_history.Push(NavEntry{ state.document.doc.GetFilePath(), state.view.viewport.GetScrollY() });
}
