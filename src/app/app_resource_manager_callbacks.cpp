#include "resource_manager_impl.h"
#include "app_resource_manager_callbacks.h"
#include "app.h"
#include "pane_layout.h"

void AppResourceManagerCallbacks::invalidate()
{
    app->Invalidate();
}

void AppResourceManagerCallbacks::set_timer(app_timer::Id id, UINT ms)
{
    app->win32_host_.SetTimer(id, ms);
}

void AppResourceManagerCallbacks::kill_timer(app_timer::Id id)
{
    app->win32_host_.KillTimer(id);
}

float AppResourceManagerCallbacks::get_content_width()
{
    return app->renderer_.GetTheme().ContentWidth(app->GetMarkdownPaneWidth());
}

float AppResourceManagerCallbacks::get_viewport_height()
{
    return app->GetPaneLayout().md_rect.height;
}

float AppResourceManagerCallbacks::get_indent_width()
{
    return app->renderer_.GetTheme().indent_width;
}

void AppResourceManagerCallbacks::recompute_layout()
{
    app->layout_service_->RecomputeAfterDiagram(
        app->state_.document.doc,
        app->state_.document.layout_cache,
        app->renderer_.GetTheme());
}

void AppResourceManagerCallbacks::recompute_layout_anchored()
{
    app->EnsureScrollTarget();
    app->layout_service_->RecomputeAfterDiagram(
        app->state_.document.doc,
        app->state_.document.layout_cache,
        app->renderer_.GetTheme());
    const auto layout = app->GetPaneLayout();
    app->EmitEffect(effect::SyncMaxScroll{ layout.md_rect.height });
    app->Invalidate();
}

template class ResourceManagerT<AppResourceManagerCallbacks>;
