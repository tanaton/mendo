#include "app_state.h"

ScrollTarget SnapshotVisibleTarget(const AppState& state) noexcept
{
    const auto& cache = state.document.layout_cache;
    const int node = state.view.viewport.FindFirstVisibleNode(cache, cache.size());
    const float y_before = (node >= 0) ? cache[node].y_position : 0.0f;
    return { node, state.view.viewport.GetScrollY() - y_before };
}

NavEntry CurrentNavEntry(const AppState& state)
{
    const ScrollTarget t = SnapshotVisibleTarget(state);
    return NavEntry{ state.document.doc.GetFilePath(), t.node, t.offset };
}

void PushCurrentNavEntry(AppState& state)
{
    state.view.nav_history.Push(CurrentNavEntry(state));
}
