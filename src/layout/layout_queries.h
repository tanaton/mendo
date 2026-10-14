#pragma once
#include "document_types.h"
#include "toc.h"
#include <cstddef>
#include <memory_resource>
#include <string_view>

class LayoutCache;

// 文書位置と LayoutCache の Y 座標を突き合わせる問い合わせ。

// scroll_y + margin の位置にある見出し (目次の項目番号)。無ければ -1。
// margin は見出し上部余白で、画面上端に近い見出しを正しくアクティブにする。
int FindActiveTocIndex(const TableOfContents& toc, const LayoutCache& cache, float scroll_y, float margin = 0.0f) noexcept;

// diff 位置のノードに基づくスクロールY座標を計算する。
// diff_pos が npos (差分位置なし = スクロール維持) またはノードが見つからない場合は
// fallback_scroll を返す。
float CalcScrollYForDiff(
    const std::pmr::vector<Node>& nodes,
    const LayoutCache& cache,
    std::string_view content,
    size_t diff_pos,
    float viewport_height,
    float fallback_scroll) noexcept;
