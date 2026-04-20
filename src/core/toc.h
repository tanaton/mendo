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
    void BuildFromNodes(const std::pmr::vector<Node>& nodes);
    const std::pmr::vector<TocEntry>& GetEntries() const noexcept { return entries_; }
    int HitTest(float local_y, float item_height) const noexcept;

    // ビューポート先頭のスクロール位置から、現在表示中の見出しに対応するTOCエントリのインデックスを返す。
    // margin: MDペイン上端までのオフセット＋見出し上部余白（y_position <= scroll_y + margin で判定）。
    // 見つからない場合は -1 を返す。
    int FindActiveIndex(const LayoutCache& cache, float scroll_y, float margin = 0.0f) const noexcept;

    void Clear() noexcept { entries_.clear(); }
    void AddEntry(const Node& node, int node_index);

private:
    std::pmr::vector<TocEntry> entries_;
};
