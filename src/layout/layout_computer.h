#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "theme.h"
#include "ui_constants.h"
#include <algorithm>
#include <limits>
#include <memory_resource>
#include <stop_token>

namespace mendo::layout {

inline float NodeIndent(const Node& node, const Theme& theme) noexcept
{
    return node.indent_level * theme.indent_width;
}

inline float NodeTextXOffset(const Node& node, const Theme& theme) noexcept
{
    return (node.type == NodeType::CodeBlock) ? theme.code_block_padding : 0.0f;
}

// ダイアグラム/画像ノードのビットマップ未確定時に使うプレースホルダー高さ。
inline float PlaceholderHeight(const Theme& theme) noexcept
{
    return std::max(MIN_DIAGRAM_PLACEHOLDER_HEIGHT, theme.font_size_body * 3.0f);
}

// 画像は max_width を超える場合のみアスペクト比を保って縮小する (拡大はしない)。
constexpr float ImageDisplayHeight(float width, float height, float max_width) noexcept
{
    return (width > max_width && width > 0.0f) ? height * (max_width / width) : height;
}

float GetSpacingAbove(const Node& node, const Theme& theme) noexcept;
float GetSpacingBelow(const Node& node, const Theme& theme) noexcept;

// 1 ノード分の Y 進行。戻り値はテキスト上端。加算順序 (above → height → below) は大規模ファイルでの
// catastrophic cancellation 回避のため既存の累積順序を厳密に保持する。
inline float AdvanceNodeY(float& y, float spacing_above, float height, float spacing_below) noexcept
{
    y += spacing_above;
    const float text_top = y;
    y += height;
    y += spacing_below;
    return text_top;
}

void ComputeColumnWidths(
    std::pmr::vector<float>& out,
    const std::pmr::vector<float>& natural_widths,
    float available_width, size_t col_count);

float EstimateNodeHeight(const Node& node, const Theme& theme) noexcept;

// stop_token が stop_requested になると途中 return する (cache は中間状態のまま)。
void EstimateNodeHeights(const std::pmr::vector<Node>& nodes, LayoutCache& cache, const Theme& theme,
                         std::stop_token stop_token = {});

// 不可視ノードに対し、現在の高さを下回らない範囲で推定値で更新する。
// 型別の touch/no-touch ポリシー (Diagram は触らない、Table は推定で成長させた場合のみ
// table_layout を invalidate) を内部に閉じ込める。戻り値: 高さが更新されたら true。
bool EstimateInvisibleNodeHeight(const Node& node, NodeLayoutEntry& entry, const Theme& theme, float node_width) noexcept;

struct YPositionResult {
    bool has_dirty_nodes = false;
};

// 高さが変わったノードの閉区間 [first, last]。以降のノードは一定量のシフトで済むため、
// 再計算を先頭からの全件ではなくこの範囲に限定できる。既定値は空。
struct HeightChangeRange {
    size_t first = std::numeric_limits<size_t>::max();
    size_t last = 0;

    constexpr bool empty() const noexcept
    {
        return first > last;
    }
    constexpr void Add(size_t i) noexcept
    {
        if (empty()) {
            first = last = i;
            return;
        }
        first = std::min(first, i);
        last = std::max(last, i);
    }
};

YPositionResult RecomputeYPositions(
    std::pmr::vector<Node>& nodes, LayoutCache& cache, const Theme& theme,
    size_t from_index = 0, bool has_earlier_dirty = false,
    size_t safe_exit_after = std::numeric_limits<size_t>::max()) noexcept;

} // namespace mendo::layout
