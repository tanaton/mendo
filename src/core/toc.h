#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include <string>
#include <vector>
#include <memory_resource>
#include <ranges>

struct TocEntry {
    int node_index = -1;
    int heading_level = 1;
};

class TableOfContents {
public:
    const std::pmr::vector<TocEntry>& GetEntries() const noexcept { return entries_; }
    int HitTest(float local_y, float item_height) const noexcept;

    // ビューポート先頭のスクロール位置から、現在表示中の見出しに対応するTOCエントリのインデックスを返す。
    // 見つからない場合は -1 を返す。
    int FindActiveIndex(const LayoutCache& cache, float scroll_y, float margin = 0.0f) const noexcept;

    void Clear() noexcept { entries_.clear(); }
    void Reserve(size_t n) { entries_.reserve(n); }
    void AddEntry(const Node& node, int node_index);

private:
    std::pmr::vector<TocEntry> entries_;
};
