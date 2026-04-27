#include "reload_diff.h"
#include <algorithm>
#include <ranges>

size_t FindFirstDifference(std::string_view old_text, std::string_view new_text) noexcept
{
    const auto [it_old, it_new] = std::ranges::mismatch(old_text, new_text);
    if (it_old == old_text.end() && it_new == new_text.end()) {
        return std::string_view::npos;
    }
    return static_cast<size_t>(it_old - old_text.begin());
}

ReloadDecision AnalyzeReloadDiff(std::string_view old_utf8, std::string_view new_utf8) noexcept
{
    const size_t diff_pos = FindFirstDifference(old_utf8, new_utf8);
    if (diff_pos == std::string_view::npos) {
        return { ReloadOp::NoChange, std::string_view::npos };
    }
    if (IsPrefixOnlyDiff(diff_pos, old_utf8.size(), new_utf8.size())) {
        if (new_utf8.size() < old_utf8.size()) {
            return { ReloadOp::DeferPrefixShrink, diff_pos };
        }
        return { ReloadOp::PrefixGrowth, diff_pos };
    }
    return { ReloadOp::FullReload, diff_pos };
}
