#pragma once
#include <vector>
#include <wrl/client.h>
#include <d2d1.h>
#include <dwrite.h>

using Microsoft::WRL::ComPtr;

struct InlineCodeBg {
    float left, top, width, height;
};

struct NodeLayoutEntry {
    float y_position = 0.0f;
    float height = 0.0f;
    ComPtr<IDWriteTextLayout> text_layout;
    bool layout_dirty = true;
    bool effects_applied = false;
    std::vector<InlineCodeBg> inline_code_bgs;

    // Table layout data
    std::vector<std::vector<ComPtr<IDWriteTextLayout>>> cell_layouts; // [row][col]
    std::vector<float> col_widths;
    std::vector<float> row_heights;
};

struct DiagramEntry {
    ComPtr<ID2D1Bitmap> bitmap;
    float width = 0.0f;
    float height = 0.0f;
};

class LayoutCache {
public:
    void Resize(size_t node_count) {
        entries_.resize(node_count);
        diagrams_.resize(node_count);
    }

    // Clear all existing entries and resize to fresh defaults.
    // Use this when switching files to avoid stale layout data.
    void Reset(size_t node_count) {
        entries_.clear();
        entries_.resize(node_count);
        diagrams_.clear();
        diagrams_.resize(node_count);
    }

    size_t size() const noexcept { return entries_.size(); }

    NodeLayoutEntry& operator[](size_t i) noexcept { return entries_[i]; }
    const NodeLayoutEntry& operator[](size_t i) const noexcept { return entries_[i]; }

    DiagramEntry& GetDiagram(size_t i) noexcept { return diagrams_[i]; }
    const DiagramEntry& GetDiagram(size_t i) const noexcept { return diagrams_[i]; }

private:
    std::vector<NodeLayoutEntry> entries_;
    std::vector<DiagramEntry> diagrams_;
};

// Compute total content height from the last node's layout position.
// Returns 0 if node_count is 0, avoiding unsigned underflow on size() - 1.
inline float ComputeTotalContentHeight(const LayoutCache& cache, size_t node_count, float margin_top) noexcept {
    if (node_count == 0) return 0.0f;
    size_t last = node_count - 1;
    return cache[last].y_position + cache[last].height + margin_top;
}

// Binary search for the first node whose bottom edge is at or below viewport_top.
// Returns the index of the first potentially visible node, or node_count if none.
inline int FindFirstVisibleNodeIndex(const LayoutCache& cache, size_t node_count, float viewport_top) noexcept {
    int lo = 0, hi = static_cast<int>(node_count);
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (cache[mid].y_position + cache[mid].height <= viewport_top)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
