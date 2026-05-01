#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "theme.h"
#include "text_measurer.h"
#include "viewport_manager.h"
#include <dwrite.h>
#include <limits>
#include <memory_resource>

class Document;

inline float NodeIndent(const Node& node, const Theme& theme) noexcept
{
    return node.indent_level * theme.indent_width;
}

inline float NodeTextXOffset(const Node& node, const Theme& theme) noexcept
{
    return (node.type == NodeType::CodeBlock) ? theme.code_block_padding : 0.0f;
}

void ComputeColumnWidths(std::pmr::vector<float>& out,
                         const std::pmr::vector<float>& natural_widths,
                         float available_width, size_t col_count);

std::pmr::wstring BuildLinearizedTableText(const std::pmr::vector<TableRow>& rows);

struct YPositionResult {
    float total_height = 0.0f;
    bool has_dirty_nodes = false;
};

YPositionResult RecomputeYPositions(std::pmr::vector<Node>& nodes, LayoutCache& cache, const Theme& theme,
                                    size_t from_index = 0, bool has_earlier_dirty = false,
                                    size_t safe_exit_after = std::numeric_limits<size_t>::max()) noexcept;

float EstimateNodeHeight(const Node& node, const Theme& theme) noexcept;

void EstimateNodeHeights(const std::pmr::vector<Node>& nodes, LayoutCache& cache, const Theme& theme) noexcept;

class LayoutEngine {
public:
    bool Init(ITextMeasurer* measurer, const Theme& theme);
    void UpdateTheme(const Theme& theme) noexcept
    {
        theme_ = &theme;
        measurer_->UpdateTheme(theme);
    }
    bool RecreateFormats();
    void ComputeLayout(std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width,
                       float viewport_top = -1.0f, float viewport_bottom = -1.0f);
    void LayoutNodes(std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width);
    bool ProcessDirtyBatch(std::pmr::vector<Node>& nodes, LayoutCache& cache,
                           float viewport_width, int batch_size, int time_budget_us = 0,
                           float viewport_top = -1.0f, float viewport_height = -1.0f,
                           float buffer_screens = 5.0f);
    bool EnsureVisibleLayout(std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width,
                             float viewport_top, float viewport_bottom);
    constexpr bool HasDirtyNodes() const noexcept
    {
        return has_dirty_nodes_;
    }
    constexpr float GetTotalHeight() const noexcept
    {
        return total_height_;
    }
    constexpr void SetTotalHeight(float h) noexcept
    {
        total_height_ = h;
    }

private:
    ITextMeasurer* measurer_ = nullptr;
    const Theme* theme_ = nullptr;

    float total_height_ = 0.0f;
    float last_viewport_width_ = 0.0f;
    bool has_dirty_nodes_ = false;
};

// LayoutEngine + ViewportManager の組み合わせを薄くラップし、
// スクロール target 管理付きのレイアウト操作を提供する。
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

    constexpr bool HasDirtyNodes() const noexcept
    {
        return engine_.HasDirtyNodes();
    }
    constexpr float GetTotalHeight() const noexcept
    {
        return engine_.GetTotalHeight();
    }
    constexpr void SetTotalHeight(float h) noexcept
    {
        engine_.SetTotalHeight(h);
    }

private:
    LayoutEngine& engine_;
    ViewportManager& viewport_;
};
