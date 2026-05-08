#include "layout_computer.h"
#include "memory_resource.h"
#include "profiler.h"
#include "ui_constants.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <numeric>
#include <ranges>

namespace mendo::layout {

namespace {
constexpr float MIN_COLUMN_WIDTH = 30.0f;
constexpr float COLUMN_WIDTH_PADDING = 4.0f;
// Y座標の早期終了判定用許容誤差（DIP単位）
constexpr float Y_POSITION_EPSILON = 0.01f;
} // namespace

float GetSpacingAbove(const Node& node, const Theme& theme) noexcept
{
    switch (node.type) {
    case NodeType::Heading:
        return theme.heading_spacing_above;
    case NodeType::CodeBlock:
    case NodeType::BlockQuote:
        return theme.code_block_spacing_above;
    case NodeType::Paragraph:
    case NodeType::HorizontalRule:
    case NodeType::ListItem:
    case NodeType::Table:
    case NodeType::TaskListItem:
    case NodeType::Image:
        return 0.0f;
    }
    std::unreachable();
}

float GetSpacingBelow(const Node& node, const Theme& theme) noexcept
{
    switch (node.type) {
    case NodeType::Heading:
        // h1/h2 は下線を描くため、下線と次行の余白を確保すべく大きめの値を返す。
        return (node.heading_level <= 2) ? theme.heading_spacing_below_h1h2 : theme.heading_spacing_below;
    case NodeType::CodeBlock:
    case NodeType::Image:
        return theme.paragraph_spacing + theme.code_block_spacing_above;
    case NodeType::ListItem:
    case NodeType::TaskListItem:
        return theme.list_item_spacing;
    case NodeType::HorizontalRule:
        return 0.0f;
    case NodeType::Table:
    case NodeType::Paragraph:
    case NodeType::BlockQuote:
        return theme.paragraph_spacing;
    }
    std::unreachable();
}

void ComputeColumnWidths(std::pmr::vector<float>& out, const std::pmr::vector<float>& natural_widths, float available_width, size_t col_count)
{
    out.resize(col_count);
    available_width = std::max(available_width, static_cast<float>(col_count) * MIN_COLUMN_WIDTH);

    const float total_natural = std::reduce(natural_widths.begin(), natural_widths.end(), 0.0f);

    if (total_natural > 0 && total_natural > available_width) {
        for (auto [w, nw] : std::views::zip(out, natural_widths) | std::views::take(col_count)) {
            w = std::max(MIN_COLUMN_WIDTH, available_width * nw / total_natural);
        }
    }
    else {
        const float even = available_width / static_cast<float>(col_count);
        for (auto [w, nw] : std::views::zip(out, natural_widths) | std::views::take(col_count)) {
            w = std::max(nw + COLUMN_WIDTH_PADDING, even);
        }
    }
}

float EstimateNodeHeight(const Node& node, const Theme& theme) noexcept
{
    const float line_height = theme.font_size_body * 1.5f;
    switch (node.type) {
    case NodeType::Heading: {
        const int level = std::clamp(static_cast<int>(node.heading_level), 1, 6) - 1;
        return theme.font_size_h[level] * 1.5f;
    }
    case NodeType::CodeBlock: {
        const int lines = 1 + node.line_count;
        const float h = theme.font_size_code * 1.3f * static_cast<float>(lines);
        return std::max(h, line_height);
    }
    case NodeType::HorizontalRule:
        return theme.paragraph_spacing + theme.hr_thickness;
    case NodeType::Table: {
        const size_t row_count = node.has_table() ? std::max<size_t>(1u, node.table_data()->row_count) : 1u;
        return line_height * 1.5f * static_cast<float>(row_count);
    }
    case NodeType::Image:
        return std::max(MIN_DIAGRAM_PLACEHOLDER_HEIGHT, theme.font_size_body * 3.0f);
    case NodeType::Paragraph:
    case NodeType::ListItem:
    case NodeType::BlockQuote:
    case NodeType::TaskListItem:
        if (!node.HasText()) {
            return theme.paragraph_spacing;
        }
        return line_height * static_cast<float>(1 + node.line_count);
    }
    std::unreachable();
}

void EstimateNodeHeights(const std::pmr::vector<Node>& nodes, LayoutCache& cache, const Theme& theme)
{
    MENDO_PROFILE("EstimateNodeHeights");
    // ノードの種類に応じた既定の高さを割り当て、Y座標を累積計算する。
    // DirectWriteを一切呼ばないため、数千ノードでも数百マイクロ秒で完了する。
    // layout_dirtyフラグは変更しない（後続のViewportLayoutが正しく計測できるようにする）。
    assert(cache.size() == nodes.size());
    const auto node_count = nodes.size();
    MENDO_PLOT("layout.estimate.node_count", static_cast<int64_t>(node_count));

    StackArena<4096> arena;
    std::pmr::vector<float> block_heights(arena.resource());
    block_heights.reserve(node_count);

    float y = theme.margin_top;
    for (size_t i = 0; i < node_count; i++) {
        const auto& node = nodes[i];
        const float h = EstimateNodeHeight(node, theme);
        const float sa = GetSpacingAbove(node, theme);
        const float sb = GetSpacingBelow(node, theme);

        y += sa;
        cache[i].height = h;
        cache[i].text_top = y;
        y += h;
        y += sb;

        block_heights.push_back(sa + h + sb);
    }
    cache.BuildBlockHeights(block_heights);
}

bool EstimateInvisibleNodeHeight(const Node& node, NodeLayoutEntry& entry, const Theme& theme, float node_width) noexcept
{
    // ダイアグラム系コードブロックの高さは描画完了時にビットマップ実寸で確定する。
    // テキスト基準の EstimateNodeHeight で上書きすると描画時に bitmap が後続ノードへ
    // はみ出すため既存値を維持する。
    if (node.type == NodeType::CodeBlock && IsDiagramLanguage(node.code_language)) {
        return false;
    }
    // 同じ幅での実測キャッシュがあればそれを使い、無ければ推定値で成長させる。
    constexpr float kCachedWidthEpsilon = 0.5f;
    const bool cache_hit = entry.cached_width > 0.0f &&
                           std::abs(entry.cached_width - node_width) < kCachedWidthEpsilon &&
                           entry.cached_height > 0.0f;
    const float fallback = cache_hit ? entry.cached_height : EstimateNodeHeight(node, theme);
    // テーブル/画像/折り返しが多い段落では推定値が実測値を大きく下回るため、
    // シュリンク方向の更新は後続ノードと重なる原因になる。よって既存値より小さくはしない。
    if (entry.height >= fallback) {
        return false;
    }
    entry.height = fallback;
    // 推定で成長させた場合、テーブル幾何 (row_heights/col_widths) は旧値のままなので
    // clear して MeasureNode の lazy 復元経路を通させる。
    if (node.type == NodeType::Table && entry.has_table_layout() && !cache_hit) {
        entry.table_layout->col_widths.clear();
        entry.table_layout->cached_table_width = 0.0f;
    }
    return true;
}

YPositionResult RecomputeYPositions(std::pmr::vector<Node>& nodes, LayoutCache& cache, const Theme& theme,
                                    size_t from_index, bool has_earlier_dirty, size_t safe_exit_after) noexcept
{
    MENDO_PROFILE("RecomputeYPositions");
    YPositionResult result;
    result.has_dirty_nodes = has_earlier_dirty;
    const auto node_count = nodes.size();
    float y = theme.margin_top;

    if (from_index > 0 && from_index < node_count) {
        auto& prev = cache[from_index - 1];
        y = prev.text_top + prev.height;
        y += GetSpacingBelow(nodes[from_index - 1], theme);
    }

    // BuildBlockHeights バルクロード経路 (O(N)) は完走確定時のみ採用する。
    // safe_exit_after < node_count なら早期終了の可能性があるため、最初から per-element
    // SetBlockHeight に倒し、Fenwick を逐次同期させて中間 flush を不要にする。
    const bool can_bulk_build = (from_index == 0) && (safe_exit_after >= node_count);
    StackArena<4096> arena;
    std::pmr::vector<float> block_heights(arena.resource());
    if (can_bulk_build) {
        block_heights.reserve(node_count);
    }

    for (size_t i = from_index; i < node_count; i++) {
        auto& entry = cache[i];
        if (entry.layout_dirty) {
            result.has_dirty_nodes = true;
        }

        const float sa = GetSpacingAbove(nodes[i], theme);
        const float sb = GetSpacingBelow(nodes[i], theme);

        y += sa;

        // safe_exit_after 以降で Y 位置が変わらないと判明したら早期終了する。
        // この経路は can_bulk_build=false が保証される (上の条件分岐) ため、
        // [from_index, i) の block_height は per-element SetBlockHeight で既に Fenwick に反映済。
        if (i > safe_exit_after && std::abs(entry.text_top - y) < Y_POSITION_EPSILON) {
            if (!result.has_dirty_nodes) {
                result.has_dirty_nodes = std::ranges::any_of(
                    std::views::iota(i, node_count),
                    [&cache](size_t j) { return cache[j].layout_dirty; });
            }
            const size_t last_idx = node_count - 1;
            result.total_height = cache[last_idx].text_top + cache[last_idx].height + GetSpacingBelow(nodes[last_idx], theme) + theme.margin_top;
            return result;
        }

        entry.text_top = y;
        y += entry.height;
        y += sb;

        const float block_height = sa + entry.height + sb;
        if (can_bulk_build) {
            block_heights.push_back(block_height);
        }
        else {
            cache.SetBlockHeight(i, block_height);
        }
    }

    if (can_bulk_build) {
        cache.BuildBlockHeights(block_heights);
    }

    result.total_height = y + theme.margin_top;
    return result;
}

} // namespace mendo::layout
