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

    size_t size() const { return entries_.size(); }

    NodeLayoutEntry& operator[](size_t i) { return entries_[i]; }
    const NodeLayoutEntry& operator[](size_t i) const { return entries_[i]; }

    DiagramEntry& GetDiagram(size_t i) { return diagrams_[i]; }
    const DiagramEntry& GetDiagram(size_t i) const { return diagrams_[i]; }

private:
    std::vector<NodeLayoutEntry> entries_;
    std::vector<DiagramEntry> diagrams_;
};
