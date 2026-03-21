#pragma once
#include "types.h"
#include <string>
#include <vector>
#include <memory_resource>
#include <ranges>

struct TocEntry {
    std::pmr::wstring text;
    std::pmr::wstring anchor_id;
    int heading_level = 1;
};

class TableOfContents {
public:
    void BuildFromNodes(const std::pmr::vector<Node>& nodes);
    const std::pmr::vector<TocEntry>& GetEntries() const noexcept { return entries_; }
    int HitTest(float local_y, float item_height) const noexcept;

private:
    std::pmr::vector<TocEntry> entries_;
};
