#include "layout_service.h"

void LayoutService::ViewportLayout(Document& doc, LayoutCache& cache, float width, float height)
{
    const float viewport_top = viewport_.GetScrollY();
    const float viewport_bottom = viewport_.GetScrollY() + height;
    engine_.ComputeLayout(doc.GetNodesMut(), cache, width, viewport_top, viewport_bottom);
}

bool LayoutService::ProcessDirtyBatch(Document& doc, LayoutCache& cache, float width, int batch_size, int time_budget_us)
{
    return engine_.ProcessDirtyBatch(doc.GetNodesMut(), cache, width, batch_size, time_budget_us);
}

bool LayoutService::EnsureVisibleLayout(Document& doc, LayoutCache& cache, float width, float height)
{
    const float viewport_top = viewport_.GetScrollY();
    const float viewport_bottom = viewport_.GetScrollY() + height;
    return engine_.EnsureVisibleLayout(doc.GetNodesMut(), cache, width, viewport_top, viewport_bottom);
}

void LayoutService::RecomputeAfterDiagram(Document& doc, LayoutCache& cache, const Theme& theme) noexcept
{
    const auto result = RecomputeYPositions(doc.GetNodesMut(), cache, theme);
    engine_.SetTotalHeight(result.total_height);
}
