#include "app_callbacks.h"
#include "app.h"
#include "pane_layout.h"

void AppCallbacksBase::set_timer(app_timer::Id id, UINT ms)
{
    app->SetTimer(id, ms);
}

void AppCallbacksBase::kill_timer(app_timer::Id id)
{
    app->KillTimer(id);
}

float AppCallbacksBase::get_md_pane_height()
{
    return app->GetPaneLayout().md_rect.height;
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
    app->SyncMaxScroll(get_md_pane_height());
}

template class ResourceManagerT<AppResourceManagerCallbacks>;

void AppSearchBarCallbacks::invalidate()
{
    app->Invalidate();
}

void AppSearchBarCallbacks::focus_select_all()
{
    app->Execute(effect::SearchFocus{});
}

void AppSearchBarCallbacks::unfocus()
{
    app->Execute(effect::SearchUnfocus{});
}

void AppSearchBarCallbacks::ensure_node_layout(int node_index)
{
    app->layout_service_->EnsureNodeLayout(
        app->state_.document.doc, app->state_.document.layout_cache,
        app->GetPaneLayout().md_rect.width, node_index);
}

void AppSearchBarCallbacks::on_wrap_around()
{
    MessageBeep(MB_OK);
}

template class SearchBarControllerT<AppSearchBarCallbacks>;
