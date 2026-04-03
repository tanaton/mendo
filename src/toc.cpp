#include "toc.h"
#include <algorithm>

void TableOfContents::BuildFromNodes(const std::pmr::vector<Node>& nodes)
{
    entries_.clear();
    entries_.reserve(std::ranges::count_if(nodes,
        [](const Node& n) static noexcept { return n.type == NodeType::Heading; }));
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].type != NodeType::Heading) {
            continue;
        }
        TocEntry entry;
        entry.text = nodes[i].text;
        entry.anchor_id = nodes[i].anchor_id;
        entry.heading_level = nodes[i].heading_level;
        entry.node_index = static_cast<int>(i);
        entries_.emplace_back(std::move(entry));
    }
}

int TableOfContents::HitTest(float local_y, float item_height) const noexcept
{
    if (local_y < 0 || item_height <= 0) {
        return -1;
    }
    const int index = static_cast<int>(local_y / item_height);
    if (index < 0 || index >= static_cast<int>(entries_.size())) {
        return -1;
    }
    return index;
}

int TableOfContents::FindActiveIndex(const LayoutCache& cache, float scroll_y, float margin) const noexcept
{
    const int n = static_cast<int>(entries_.size());
    if (n == 0) {
        return -1;
    }
    // 二分探索: cache[entries_[i].node_index].y_position <= scroll_y + margin を満たす最大の i を求める
    // margin はMDペイン上端オフセット＋見出し上部余白で、画面上端に近い見出しを正しくアクティブにする。
    const float threshold = scroll_y + margin;
    int lo = 0, hi = n;
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        const int ni = entries_[mid].node_index;
        if (ni >= 0 && ni < static_cast<int>(cache.size()) && cache[ni].y_position <= threshold) {
            lo = mid + 1;
        }
        else {
            hi = mid;
        }
    }
    return lo - 1;
}
