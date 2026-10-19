#include "app_callbacks.h"
#include "app.h"
#include "pane_layout.h"

float AppCallbacksBase::get_md_pane_height()
{
    return app->GetPaneLayout().md_rect.height;
}

void AppResourceManagerCallbacks::set_timer(app_timer::Id id, UINT ms)
{
    app->SetTimer(id, ms);
}

void AppResourceManagerCallbacks::kill_timer(app_timer::Id id)
{
    app->KillTimer(id);
}

float AppResourceManagerCallbacks::get_content_width()
{
    return app->MdContentWidth();
}

const Theme& AppResourceManagerCallbacks::get_theme()
{
    return app->renderer_.GetTheme();
}

void AppResourceManagerCallbacks::recompute_layout(layout::HeightChangeRange changed)
{
    app->layout_service_->RecomputeAfterDiagram(
        app->state_.document.doc,
        app->state_.document.layout_cache,
        app->renderer_.GetTheme(),
        changed);
    // リフローで停止中のカーソル直下のノード/リンクが変わりうるため、ホバーの
    // 同一座標スキップを解除し次のマウス移動で再評価させる (カーソル形状の陳腐化対策)。
    app->InvalidateHitPositions();
    app->Invalidate();
}

void AppResourceManagerCallbacks::recompute_layout_anchored(layout::HeightChangeRange changed)
{
    app->EnsureScrollTarget();
    recompute_layout(changed);
    app->SyncMaxScroll();
}

template class ResourceManagerT<AppResourceManagerCallbacks>;

void AppSearchBarCallbacks::ensure_node_layout(int node_index)
{
    app->layout_service_->EnsureNodeLayout(
        app->state_.document.doc, app->state_.document.layout_cache,
        app->GetPaneLayout().md_rect.width, node_index);
}

template class SearchBarControllerT<AppSearchBarCallbacks>;
