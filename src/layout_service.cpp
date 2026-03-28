#include "layout_service.h"

void LayoutService::FullLayout(Document& doc, LayoutCache& cache, float width)
{
    engine_.ComputeLayout(doc.GetNodesMut(), cache, width);
}

void LayoutService::ViewportLayout(Document& doc, LayoutCache& cache,
    float width, float height)
{
    float viewport_top = viewport_.GetScrollY();
    float viewport_bottom = viewport_.GetScrollY() + height;
    engine_.ComputeLayout(doc.GetNodesMut(), cache, width, viewport_top, viewport_bottom);
}

bool LayoutService::ProcessDirtyBatch(Document& doc, LayoutCache& cache,
    float width, int batch_size)
{
    return engine_.ProcessDirtyBatch(doc.GetNodesMut(), cache, width, batch_size);
}

bool LayoutService::EnsureVisibleLayout(Document& doc, LayoutCache& cache,
    float width, float height)
{
    float viewport_top = viewport_.GetScrollY();
    float viewport_bottom = viewport_.GetScrollY() + height;
    return engine_.EnsureVisibleLayout(doc.GetNodesMut(), cache, width, viewport_top, viewport_bottom);
}

void LayoutService::RecomputeAfterDiagram(Document& doc, LayoutCache& cache,
    const Theme& theme)
{
    auto result = RecomputeYPositions(doc.GetNodesMut(), cache, theme);
    engine_.SetTotalHeight(result.total_height);
}
