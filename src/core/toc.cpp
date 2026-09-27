#include "toc.h"
#include "pane_layout.h"
#include <algorithm>

void TableOfContents::AddEntry(const Node& node, int node_index)
{
    entries_.emplace_back(node_index, node.heading_level());
}

int TableOfContents::HitTest(float local_y, float item_height) const noexcept
{
    return HitTestUniformList(local_y, item_height, entries_.size());
}

int TableOfContents::FindActiveIndex(const LayoutCache& cache, float scroll_y, float margin) const noexcept
{
    const int n = static_cast<int>(entries_.size());
    if (n == 0) {
        return -1;
    }
    // cache[entries_[i].node_index].text_top <= scroll_y + margin を満たす最大の i を求める。
    // margin はMDペイン上端オフセット＋見出し上部余白で、画面上端に近い見出しを正しくアクティブにする。
    const float threshold = scroll_y + margin;
    const auto it = std::ranges::partition_point(entries_, [&](const TocEntry& e) noexcept {
        return e.node_index >= 0 && e.node_index < static_cast<int>(cache.size()) && cache.Top(static_cast<size_t>(e.node_index)) <= threshold;
    });
    return (it != entries_.begin()) ? static_cast<int>(std::prev(it) - entries_.begin()) : -1;
}
