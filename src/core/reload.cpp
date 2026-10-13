#include "reload.h"
#include "layout_cache.h"
#include <algorithm>

size_t FindFirstDifference(std::string_view old_text, std::string_view new_text) noexcept
{
    // MSVC STL の mismatch は 1 バイト要素をベクトル化する。
    const size_t pos = static_cast<size_t>(std::ranges::mismatch(old_text, new_text).in1 - old_text.begin());
    if (pos == old_text.size() && pos == new_text.size()) {
        return std::string_view::npos;
    }
    return pos;
}

ReloadDecision AnalyzeReloadDiff(std::string_view old_text, std::string_view new_text) noexcept
{
    const size_t diff_pos = FindFirstDifference(old_text, new_text);
    if (diff_pos == std::string_view::npos) {
        return { ReloadOp::NoChange, std::string_view::npos };
    }
    if (new_text.size() < old_text.size() && IsPrefixOnlyDiff(diff_pos, old_text.size(), new_text.size())) {
        return { ReloadOp::DeferPrefixShrink, diff_pos };
    }
    return { ReloadOp::Replace, diff_pos };
}

int FindNodeBySourceOffset(const std::pmr::vector<Node>& nodes, size_t diff_offset) noexcept
{
    // source_offset はパース順で基本的に単調増加するため二分探索を使用。
    // 未設定ノードに当たった場合は左に有効ノードを探してから判定する。
    int lo = 0;
    int hi = static_cast<int>(nodes.size()) - 1;
    int result = -1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        int probe = mid;
        while (probe >= lo && !nodes[probe].HasSourceOffset()) {
            probe--;
        }
        if (probe < lo) {
            lo = mid + 1;
            continue;
        }
        if (nodes[probe].SourceOffset() <= diff_offset) {
            result = probe;
            lo = mid + 1;
        }
        else {
            hi = probe - 1;
        }
    }
    return result;
}

namespace {

// node_index より後ろで node_start を超える最初の source offset。見つからなければ fallback。
// 途中に offset 無しノード (HR や空のルーズリスト等) が連続しても取り逃さないよう探索するが、
// 末尾まで線形走査すると大規模ファイルのリロードが重くなるため上限を設ける
// (通常は最初の有効 offset で見つかるため上限には届かない)。
size_t NextSourceOffset(const std::pmr::vector<Node>& nodes, int node_index, size_t node_start, size_t fallback) noexcept
{
    constexpr int kMaxOffsetProbe = 4096;
    const int probe_limit = std::min(static_cast<int>(nodes.size()), node_index + 1 + kMaxOffsetProbe);
    for (int i = node_index + 1; i < probe_limit; ++i) {
        if (nodes[i].HasSourceOffset() && nodes[i].SourceOffset() > node_start) {
            return nodes[i].SourceOffset();
        }
    }
    return fallback;
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
    const auto changed_node = FindNodeBySourceOffset(nodes, diff_pos);
    if (changed_node < 0 || changed_node >= static_cast<int>(cache.size())) {
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
