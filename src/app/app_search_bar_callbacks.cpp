#include "search_bar_controller.h"
#include "app_search_bar_callbacks.h"
#include "app.h"
#include "pane_layout.h"

void AppSearchBarCallbacks::invalidate()
{
    app->Invalidate();
}

void AppSearchBarCallbacks::invalidate_search_bar()
{
    const auto& layout = app->GetPaneLayout();
    const auto& r = layout.md_rect;
    const PaneRect search_area{ r.x, r.y + r.height - SEARCH_BAR_HEIGHT, r.width, SEARCH_BAR_HEIGHT };
    app->InvalidatePane(search_area);
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

void AppSearchBarCallbacks::on_scroll_changed(float md_pane_height)
{
    app->EmitEffect(effect::SyncMaxScroll{ md_pane_height });
    app->InvalidateHitPositions();
    app->resource_manager_.ScheduleBitmapManage();
}

void AppSearchBarCallbacks::on_wrap_around()
{
    MessageBeep(MB_OK);
}

template class SearchBarControllerT<AppSearchBarCallbacks>;
