#include "toc.h"

void TableOfContents::BuildFromNodes(const std::vector<Node>& nodes) {
    entries_.clear();
    for (const auto& node : nodes) {
        if (node.type != NodeType::Heading) continue;

        TocEntry entry;
        entry.text = node.text;
        entry.anchor_id = node.anchor_id;
        entry.heading_level = node.heading_level;
        entries_.push_back(std::move(entry));
    }
}

int TableOfContents::HitTest(float local_y, float item_height) const {
    if (local_y < 0 || item_height <= 0) return -1;
    int index = static_cast<int>(local_y / item_height);
    if (index < 0 || index >= static_cast<int>(entries_.size())) return -1;
    return index;
}
