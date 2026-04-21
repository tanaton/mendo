#include "layout_service.h"

void LayoutService::ViewportLayout(Document& doc, LayoutCache& cache, float width, float height)
{
    const float scroll_y = viewport_.GetScrollY();
    engine_.ComputeLayout(doc.GetNodesMut(), cache, width, scroll_y, scroll_y + height);
    viewport_.ApplyScrollTarget(cache);
}

bool LayoutService::ProcessDirtyBatch(Document& doc, LayoutCache& cache, float width, int batch_size, int time_budget_us,
    float viewport_height, float buffer_screens)
{
    bool more;
    if (viewport_height > 0.0f) {
        const float vp_top = viewport_.GetScrollY();
        more = engine_.ProcessDirtyBatch(doc.GetNodesMut(), cache, width, batch_size, time_budget_us, vp_top, viewport_height, buffer_screens);
    }
    else {
        more = engine_.ProcessDirtyBatch(doc.GetNodesMut(), cache, width, batch_size, time_budget_us);
    }
    viewport_.ApplyScrollTarget(cache);
    return more;
}

bool LayoutService::EnsureVisibleLayout(Document& doc, LayoutCache& cache, float width, float height)
{
    const float scroll_y = viewport_.GetScrollY();
    const bool updated = engine_.EnsureVisibleLayout(doc.GetNodesMut(), cache, width, scroll_y, scroll_y + height);
    if (updated) {
        viewport_.ApplyScrollTarget(cache);
    }
    return updated;
}

void LayoutService::RecomputeAfterDiagram(Document& doc, LayoutCache& cache, const Theme& theme) noexcept
{
    const auto result = RecomputeYPositions(doc.GetNodesMut(), cache, theme);
    engine_.SetTotalHeight(result.total_height);
    viewport_.ApplyScrollTarget(cache);
}
