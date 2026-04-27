#pragma once
// reload_diff.{h,cpp} の補助。差分位置 (UTF-8 バイトオフセット) を
// 対応するノード / スクロールY座標へ写像するルックアップを提供する。
#include "document_types.h"
#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <vector>

class LayoutCache;

// source_offset が diff_offset 以下の最後のノードを返す。該当なしの場合は -1。
[[nodiscard]] int FindNodeBySourceOffset(const std::pmr::vector<Node>& nodes, uint32_t diff_offset) noexcept;

// diff 位置のノードに基づくスクロールY座標を計算する。
// ノードが見つからない場合は fallback_scroll を返す。
[[nodiscard]] float CalcScrollYForDiff(
    const std::pmr::vector<Node>& nodes,
    const LayoutCache& cache,
    std::string_view content,
    size_t diff_pos,
    float viewport_height,
    float fallback_scroll) noexcept;
