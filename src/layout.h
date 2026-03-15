#pragma once
#include "types.h"
#include "theme.h"
#include <dwrite.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// Compute column widths for a table given natural (measured) widths and available space.
// Returns the final column widths vector.
std::vector<float> ComputeColumnWidths(const std::vector<float>& natural_widths,
                                        float available_width, size_t col_count);

// Build linearized text from table rows (tab-separated cells, newline-separated rows).
// Used for text selection support.
std::wstring BuildLinearizedTableText(const std::vector<TableRow>& rows);

// Recompute Y positions and spacing for all nodes.
// Returns {total_height, has_dirty_nodes}.
struct YPositionResult {
    float total_height = 0.0f;
    bool has_dirty_nodes = false;
};
YPositionResult RecomputeYPositions(std::vector<RenderNode>& nodes, const Theme& theme);

class LayoutEngine {
public:
    bool Init(IDWriteFactory* dwrite_factory, const Theme& theme);
    void ComputeLayout(std::vector<RenderNode>& nodes, float viewport_width,
                       float viewport_top = -1.0f, float viewport_bottom = -1.0f);
    bool ProcessDirtyBatch(std::vector<RenderNode>& nodes, float viewport_width, int batch_size);
    bool HasDirtyNodes() const { return has_dirty_nodes_; }
    float GetTotalHeight() const { return total_height_; }

private:
    void CreateTextLayout(RenderNode& node, float max_width);
    void CreateTableLayout(RenderNode& node, float max_width);
    void ApplyCellRunFormatting(IDWriteTextLayout* layout, const std::vector<TextRun>& runs);
    IDWriteTextFormat* GetTextFormat(const RenderNode& node);

    IDWriteFactory* dwrite_ = nullptr;
    const Theme* theme_ = nullptr;

    ComPtr<IDWriteTextFormat> fmt_body_;
    ComPtr<IDWriteTextFormat> fmt_h1_;
    ComPtr<IDWriteTextFormat> fmt_h2_;
    ComPtr<IDWriteTextFormat> fmt_h3_;
    ComPtr<IDWriteTextFormat> fmt_h4_;
    ComPtr<IDWriteTextFormat> fmt_h5_;
    ComPtr<IDWriteTextFormat> fmt_h6_;
    ComPtr<IDWriteTextFormat> fmt_code_;

    float total_height_ = 0.0f;
    float last_viewport_width_ = 0.0f;
    bool has_dirty_nodes_ = false;
};
