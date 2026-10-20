#include "layout_queries.h"
#include "layout_cache.h"
#include "reload.h"
#include <algorithm>
#include <cassert>
#include <iterator>
#include <ranges>

namespace {

// node_index より後ろで node_start を超える最初の source offset。見つからなければ fallback。
// 途中に offset 無しノード (HR や空のルーズリスト等) が連続しても取り逃さないよう探索するが、
// 末尾まで線形走査すると大規模ファイルのリロードが重くなるため上限を設ける
// (通常は最初の有効 offset で見つかるため上限には届かない)。
size_t NextSourceOffset(const std::pmr::vector<Node>& nodes, int node_index, size_t node_start, size_t fallback) noexcept
{
    constexpr int kMaxOffsetProbe = 4096;
    const auto probe = nodes | std::views::drop(node_index + 1) | std::views::take(kMaxOffsetProbe);
    const auto it = std::ranges::find_if(probe, [node_start](const Node& n) noexcept { return n.HasSourceOffset() && n.SourceOffset() > node_start; });
    return it != probe.end() ? it->SourceOffset() : fallback;
}

} // namespace

float CalcScrollYForDiff(
    const std::pmr::vector<Node>& nodes,
    const LayoutCache& cache,
    std::string_view content,
    size_t diff_pos,
    float viewport_height,
    float fallback_scroll) noexcept
{
    if (diff_pos == std::string_view::npos) {
        return fallback_scroll;
    }
    assert(cache.size() == nodes.size());
    const auto changed_node = FindNodeBySourceOffset(nodes, diff_pos);
    if (changed_node < 0) {
        return fallback_scroll;
    }

    float node_y = cache.Top(static_cast<size_t>(changed_node));
    const float node_h = cache[changed_node].height;

    // FindNodeBySourceOffset はソース位置を持つノードだけを返す。
    const auto node_start = nodes[changed_node].SourceOffset();
    const size_t next_start = NextSourceOffset(nodes, changed_node, node_start, content.size());
    if (next_start > node_start) {
        const float fraction = static_cast<float>(diff_pos - node_start) / static_cast<float>(next_start - node_start);
        node_y += node_h * std::min(fraction, 1.0f);
    }

    const float margin = viewport_height * 0.2f;
    return std::max(0.0f, node_y - margin);
}

int FindActiveTocIndex(std::span<const TocEntry> entries, const LayoutCache& cache, float scroll_y, float margin) noexcept
{
    const int n = static_cast<int>(entries.size());
    if (n == 0) {
        return -1;
    }
    // cache[entries[i].node_index].text_top <= scroll_y + margin を満たす最大の i を求める。
    const float threshold = scroll_y + margin;
    const auto it = std::ranges::partition_point(entries, [&](const TocEntry& e) noexcept {
        return e.node_index >= 0 && cache.Top(static_cast<size_t>(e.node_index)) <= threshold;
    });
    return (it != entries.begin()) ? static_cast<int>(std::prev(it) - entries.begin()) : -1;
}
