#include "search_bar_controller.h"
#include "app_search_bar_callbacks.h"
#include "app.h"
#include "pane_layout.h"

void AppSearchBarCallbacks::invalidate()
{
    app->Invalidate();
}

void AppSearchBarCallbacks::set_timer(app_timer::Id id, UINT ms)
{
    app->win32_host_.SetTimer(id, ms);
}

void AppSearchBarCallbacks::kill_timer(app_timer::Id id)
{
    app->win32_host_.KillTimer(id);
}

void AppSearchBarCallbacks::focus_select_all()
{
    app->EmitEffect(effect::SearchFocus{});
}

void AppSearchBarCallbacks::unfocus()
{
    app->EmitEffect(effect::SearchUnfocus{});
}

float AppSearchBarCallbacks::get_md_pane_height()
{
    return app->GetPaneLayout().md_rect.height;
}

void AppSearchBarCallbacks::ensure_node_layout(int node_index)
{
    app->layout_service_->EnsureNodeLayout(app->state_.document.doc, app->state_.document.layout_cache,
                                           app->GetPaneLayout().md_rect.width, node_index);
}

void AppSearchBarCallbacks::on_wrap_around()
{
    MessageBeep(MB_OK);
}

template class SearchBarControllerT<AppSearchBarCallbacks>;
