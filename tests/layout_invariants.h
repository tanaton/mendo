#pragma once
#include <gtest/gtest.h>
#include <cmath>
#include <memory_resource>
#include "document_types.h"
#include "layout_cache.h"
#include "layout_computer.h"
#include "theme.h"

// Y 連鎖 Top(0) = margin_top + sa(0)、Top(i+1) = Bottom(i) + sb(i) + sa(i+1) が保たれているか。
// RecomputeYPositions は 0.01 未満のシフトを省くため、その分と float 誤差を eps で許す。
inline ::testing::AssertionResult YChainConsistent(
    const std::pmr::vector<Node>& nodes, const LayoutCache& cache, const Theme& theme, float eps = 0.05f)
{
    using mendo::layout::GetSpacingAbove;
    using mendo::layout::GetSpacingBelow;
    if (nodes.empty()) {
        return ::testing::AssertionSuccess();
    }
    const float top0 = theme.margin_top + GetSpacingAbove(nodes[0], theme);
    if (std::abs(cache.Top(0) - top0) > eps) {
        return ::testing::AssertionFailure() << "Top(0)=" << cache.Top(0) << " expected=" << top0;
    }
    for (size_t i = 0; i + 1 < nodes.size(); ++i) {
        const float expected = cache.Bottom(i) + GetSpacingBelow(nodes[i], theme) + GetSpacingAbove(nodes[i + 1], theme);
        if (std::abs(cache.Top(i + 1) - expected) > eps) {
            return ::testing::AssertionFailure() << "Top(" << (i + 1) << ")=" << cache.Top(i + 1) << " expected=" << expected
                                                 << " (height(" << i << ")=" << cache[i].height << ")";
        }
    }
    return ::testing::AssertionSuccess();
}
