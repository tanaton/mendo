#pragma once
#include "types.h"
#include <string>
#include <vector>

struct TocEntry {
    std::wstring text;
    std::wstring anchor_id;
    int heading_level = 1;
};

class TableOfContents {
public:
    void BuildFromNodes(const std::vector<Node>& nodes);
    const std::vector<TocEntry>& GetEntries() const { return entries_; }
    int HitTest(float local_y, float item_height) const;

private:
    std::vector<TocEntry> entries_;
};
