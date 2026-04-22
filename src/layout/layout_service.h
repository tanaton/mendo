#pragma once
#include "layout.h"
#include "viewport_manager.h"
#include "document.h"
#include "layout_cache.h"

class LayoutService {
public:
    LayoutService(LayoutEngine& engine, ViewportManager& viewport) noexcept
        : engine_(engine), viewport_(viewport)
    {
    }

    void ViewportLayout(Document& doc, LayoutCache& cache, float width, float height);
    bool ProcessDirtyBatch(Document& doc, LayoutCache& cache, float width, int batch_size, int time_budget_us = 0,
        float viewport_height = -1.0f, float buffer_screens = 5.0f);
    bool EnsureVisibleLayout(Document& doc, LayoutCache& cache, float width, float height);
    void RecomputeAfterDiagram(Document& doc, LayoutCache& cache, const Theme& theme) noexcept;

    constexpr bool HasDirtyNodes() const noexcept { return engine_.HasDirtyNodes(); }
    constexpr float GetTotalHeight() const noexcept { return engine_.GetTotalHeight(); }
    constexpr void SetTotalHeight(float h) noexcept { engine_.SetTotalHeight(h); }

private:
    LayoutEngine& engine_;
    ViewportManager& viewport_;
};
