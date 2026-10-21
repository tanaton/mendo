#include "app_callbacks.h"
#include "app.h"
#include "pane_layout.h"

void AppHost::SetTimer(app_timer::Id id, UINT ms)
{
    app_.SetTimer(id, ms);
}

void AppHost::KillTimer(app_timer::Id id)
{
    app_.KillTimer(id);
}

float AppHost::MdContentWidth()
{
    return app_.MdContentWidth();
}

float AppHost::MdPaneHeight()
{
    return app_.GetPaneLayout().md_rect.height;
}

float AppHost::MdMaxScroll()
{
    return app_.MdMaxScroll();
}

const Theme& AppHost::GetTheme()
{
    return app_.renderer_.GetTheme();
}

void AppHost::RecomputeLayout(layout::HeightChangeRange changed)
{
    // 画面より上の画像・図の高さが変わっても、見ている内容が動かないようにする。
    app_.EnsureScrollTarget();
    app_.layout_service_.RecomputeAfterDiagram(
        app_.state_.document.doc,
        app_.state_.document.layout_cache,
        app_.renderer_.GetTheme(),
        changed);
    app_.ClampScrollToContent();
    app_.Invalidate();
}

void AppHost::EnsureNodeLayout(int node_index)
{
    app_.layout_service_.EnsureNodeLayout(
        app_.state_.document.doc, app_.state_.document.layout_cache,
        app_.GetPaneLayout().md_rect.width, node_index);
}
