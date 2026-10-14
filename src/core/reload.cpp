#include "reload.h"
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
