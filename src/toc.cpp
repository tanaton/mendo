#include "toc.h"

struct NodeTypeHeadingFilter {
    static constexpr bool operator()(const Node& node) noexcept {
        return node.type == NodeType::Heading;
    }
};

void TableOfContents::BuildFromNodes(const std::pmr::vector<Node>& nodes) {
    entries_.clear();
    size_t heading_count = 0;
    for (const auto& node : nodes | std::views::filter(NodeTypeHeadingFilter{})) {
        heading_count++;
    }
    entries_.reserve(heading_count);
    for (const auto& node : nodes | std::views::filter(NodeTypeHeadingFilter{})) {
        TocEntry entry;
        entry.text = std::wstring_view{node.text};
        entry.anchor_id = std::wstring_view{node.anchor_id};
        entry.heading_level = node.heading_level;
        entries_.push_back(std::move(entry));
    }
}

int TableOfContents::HitTest(float local_y, float item_height) const noexcept {
    if (local_y < 0 || item_height <= 0) return -1;
    int index = static_cast<int>(local_y / item_height);
    if (index < 0 || index >= static_cast<int>(entries_.size())) return -1;
    return index;
}
