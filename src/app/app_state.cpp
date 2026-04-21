#include "app_state.h"

AnchorState SaveAnchorFromState(const AppState& state) noexcept
{
    const auto& cache = state.document.layout_cache;
    AnchorState a;
    a.idx = state.view.viewport.FindFirstVisibleNode(cache, cache.size());
    a.y_before = (a.idx >= 0) ? cache[a.idx].y_position : 0.0f;
    a.offset = state.view.viewport.GetScrollY() - a.y_before;
    return a;
}

NavEntry CurrentNavEntry(const AppState& state)
{
    const AnchorState a = SaveAnchorFromState(state);
    return NavEntry{ state.document.doc.GetFilePath(), a.idx, a.offset };
}

void PushCurrentNavEntry(AppState& state)
{
    state.view.nav_history.Push(CurrentNavEntry(state));
}
