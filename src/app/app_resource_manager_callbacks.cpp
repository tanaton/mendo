#include "resource_manager.h"
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
    return app->MdContentWidth();
}

float AppResourceManagerCallbacks::get_viewport_height()
{
    return app->GetPaneLayout().md_rect.height;
}

float AppResourceManagerCallbacks::get_indent_width()
{
    return app->renderer_.GetTheme().indent_width;
}

void AppResourceManagerCallbacks::recompute_layout(mendo::layout::HeightChangeRange changed)
{
    app->layout_service_->RecomputeAfterDiagram(
        app->state_.document.doc,
        app->state_.document.layout_cache,
        app->renderer_.GetTheme(),
        changed);
    // リフローで停止中のカーソル直下のノード/リンクが変わりうるため、ホバーの
    // ヒットキャッシュを無効化し次のマウス移動で再評価させる (カーソル形状の陳腐化対策)。
    app->InvalidateHitPositions();
}

void AppResourceManagerCallbacks::recompute_layout_anchored(mendo::layout::HeightChangeRange changed)
{
    app->EnsureScrollTarget();
    recompute_layout(changed);
    app->SyncMaxScroll(app->GetPaneLayout().md_rect.height);
    app->Invalidate();
}

template class ResourceManagerT<AppResourceManagerCallbacks>;
