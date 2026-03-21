#pragma once
#include "types.h"
#include "layout_cache.h"
#include "theme.h"
#include "text_measurer.h"
#include <dwrite.h>
#include <wrl/client.h>
#include <memory_resource>

using Microsoft::WRL::ComPtr;

// Compute column widths for a table given natural (measured) widths and available space.
// Returns the final column widths vector.
std::pmr::vector<float> ComputeColumnWidths(const std::pmr::vector<float>& natural_widths,
                                        float available_width, size_t col_count);

// Build linearized text from table rows (tab-separated cells, newline-separated rows).
// Used for text selection support.
std::wstring BuildLinearizedTableText(const std::pmr::vector<TableRow>& rows);

// Recompute Y positions and spacing for all nodes starting from from_index.
// Returns {total_height, has_dirty_nodes}.
struct YPositionResult {
    float total_height = 0.0f;
    bool has_dirty_nodes = false;
};
YPositionResult RecomputeYPositions(std::pmr::vector<Node>& nodes, LayoutCache& cache, const Theme& theme,
                                    size_t from_index = 0, bool has_earlier_dirty = false);

class LayoutEngine {
public:
    bool Init(ITextMeasurer* measurer, const Theme& theme);
    void UpdateTheme(const Theme& theme) noexcept { theme_ = &theme; measurer_->UpdateTheme(theme); }
    // Recreate all text format objects (e.g. after zoom or theme change).
    bool RecreateFormats();
    void ComputeLayout(std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width,
                       float viewport_top = -1.0f, float viewport_bottom = -1.0f);
    void LayoutNodes(std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width);
    bool ProcessDirtyBatch(std::pmr::vector<Node>& nodes, LayoutCache& cache,
                           float viewport_width, int batch_size);
    bool EnsureVisibleLayout(std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width,
                             float viewport_top, float viewport_bottom);
    bool HasDirtyNodes() const noexcept { return has_dirty_nodes_; }
    float GetTotalHeight() const noexcept { return total_height_; }
    void SetTotalHeight(float h) noexcept { total_height_ = h; }

private:
    void CreateTextLayout(Node& node, NodeLayoutEntry& entry, float max_width);

    ITextMeasurer* measurer_ = nullptr;
    const Theme* theme_ = nullptr;

    float total_height_ = 0.0f;
    float last_viewport_width_ = 0.0f;
    bool has_dirty_nodes_ = false;
};
