#include "reload.h"
#include "layout_cache.h"
#include "layout_computer.h"
#include "theme.h"
#include <algorithm>
#include <cstdint>
#include <ranges>

size_t FindFirstDifference(mendo::doc_string_view old_text, mendo::doc_string_view new_text) noexcept
{
    const auto [it_old, it_new] = std::ranges::mismatch(old_text, new_text);
    if (it_old == old_text.end() && it_new == new_text.end()) {
        return mendo::doc_string_view::npos;
    }
    return static_cast<size_t>(it_old - old_text.begin());
}

ReloadDecision AnalyzeReloadDiff(mendo::doc_string_view old_text, mendo::doc_string_view new_text) noexcept
{
    const size_t diff_pos = FindFirstDifference(old_text, new_text);
    if (diff_pos == mendo::doc_string_view::npos) {
        return { ReloadOp::NoChange, mendo::doc_string_view::npos };
    }
    if (IsPrefixOnlyDiff(diff_pos, old_text.size(), new_text.size())) {
        if (new_text.size() < old_text.size()) {
            return { ReloadOp::DeferPrefixShrink, diff_pos };
        }
        return { ReloadOp::PrefixGrowth, diff_pos };
    }
    return { ReloadOp::FullReload, diff_pos };
}

int FindNodeBySourceOffset(const std::pmr::vector<Node>& nodes, uint32_t diff_offset) noexcept
{
    // source_offset はパース順で基本的に単調増加するため二分探索を使用。
    // 未設定ノードに当たった場合は左に有効ノードを探してから判定する。
    int lo = 0, hi = static_cast<int>(nodes.size()) - 1;
    int result = -1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const uint32_t offset = nodes[mid].source_offset;
        if (offset == kUnsetSourceOffset) {
            int probe = mid - 1;
            while (probe >= lo && nodes[probe].source_offset == kUnsetSourceOffset) {
                probe--;
            }
            if (probe < lo) {
                lo = mid + 1;
            }
            else if (nodes[probe].source_offset <= diff_offset) {
                result = probe;
                lo = mid + 1;
            }
            else {
                hi = probe - 1;
            }
            continue;
        }
        if (offset <= diff_offset) {
            result = mid;
            lo = mid + 1;
        }
        else {
            hi = mid - 1;
        }
    }
    return result;
}

float CalcScrollYForDiff(
    const std::pmr::vector<Node>& nodes,
    const LayoutCache& cache,
    const Theme& theme,
    mendo::doc_string_view content,
    size_t diff_pos,
    float viewport_height,
    float fallback_scroll) noexcept
{
    const int changed_node = FindNodeBySourceOffset(nodes, static_cast<uint32_t>(diff_pos));
    if (changed_node < 0 || changed_node >= static_cast<int>(cache.size())) {
        return fallback_scroll;
    }

    float node_y = mendo::layout::TextTopOf(cache, static_cast<size_t>(changed_node), nodes[changed_node], theme);
    const float node_h = cache[changed_node].height;

    const uint32_t node_start = nodes[changed_node].source_offset;
    if (node_start != kUnsetSourceOffset) {
        uint32_t next_start = static_cast<uint32_t>(content.size());
        const auto node_count = static_cast<int>(nodes.size());
        for (int i = changed_node + 1; i < node_count; ++i) {
            if (nodes[i].source_offset != kUnsetSourceOffset && nodes[i].source_offset > node_start) {
                next_start = nodes[i].source_offset;
                break;
            }
        }
        if (next_start > node_start) {
            const float fraction = static_cast<float>(diff_pos - node_start) / static_cast<float>(next_start - node_start);
            node_y += node_h * std::min(fraction, 1.0f);
        }
    }

    const float margin = viewport_height * 0.2f;
    return std::max(0.0f, node_y - margin);
}
