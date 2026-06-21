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
// 列幅計算の下限。半角 2-3 文字相当（Theme の body フォントサイズ ~14pt 基準）。
// 短いセルが極端に潰れないようにするための実用下限で、フォントサイズを変えても
// 視覚的に違和感が出にくい固定値として採用している。
constexpr float MIN_COLUMN_WIDTH = 30.0f;
// 列幅計算で実測幅に上乗せする余白。テーブル罫線とパディングを合わせた境界 (DIP)。
// TABLE_CELL_PADDING (8.0f) より小さいのは、列幅は左右の合計ではなく片側相当で扱うため。
constexpr float COLUMN_WIDTH_PADDING = 4.0f;
// Y座標の早期終了判定用許容誤差（DIP単位）。物理ピクセル境界 0.01 DIP は 96 DPI 換算で
// 約 1/100 px 未満で、視覚差を生まない閾値。これ未満の差分は再計算不要として break する。
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
        return (node.heading_level() <= 2) ? theme.heading_spacing_below_h1h2 : theme.heading_spacing_below;
    case NodeType::CodeBlock:
    case NodeType::Image:
        return theme.paragraph_spacing + theme.code_block_spacing_above;
    case NodeType::ListItem:
    case NodeType::TaskListItem:
        // 空 LI に sb を入れると直下 P の text_top が下ズレし bullet と分離する (issue#237)。
        return IsEmptyListItemContainer(node) ? 0.0f : theme.list_item_spacing;
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
    if (col_count == 0) {
        return;
    }
    const float effective_available = std::max(available_width, static_cast<float>(col_count) * MIN_COLUMN_WIDTH);

    const float total_natural = std::reduce(natural_widths.begin(), natural_widths.end(), 0.0f);

    if (total_natural > 0 && total_natural > effective_available) {
        // 比例圧縮で最も狭い列が MIN_COLUMN_WIDTH を割る場合は、無理に押し込めず
        // 自然幅で出して横スクロールに任せる方が読みやすい。
        const float smallest = *std::ranges::min_element(natural_widths | std::views::take(col_count));
        if (smallest * effective_available < MIN_COLUMN_WIDTH * total_natural) {
            std::ranges::copy(natural_widths | std::views::take(col_count), out.begin());
            return;
        }
        for (auto [w, nw] : std::views::zip(out, natural_widths) | std::views::take(col_count)) {
            w = effective_available * nw / total_natural;
        }
    }
    else {
        const float even = effective_available / static_cast<float>(col_count);
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
        const int level = std::clamp(static_cast<int>(node.heading_level()), 1, 6) - 1;
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
        return PlaceholderHeight(theme);
    case NodeType::ListItem:
    case NodeType::TaskListItem:
        // 空 LI に高さを与えると bullet と直下 P の文字 Y が分離する (issue#237)。
        if (IsEmptyListItemContainer(node)) {
            return 0.0f;
        }
        return line_height * static_cast<float>(1 + node.line_count);
    case NodeType::Paragraph:
    case NodeType::BlockQuote:
        if (!node.HasText()) {
            return theme.paragraph_spacing;
        }
        return line_height * static_cast<float>(1 + node.line_count);
    }
    std::unreachable();
}

void EstimateNodeHeights(const std::pmr::vector<Node>& nodes, LayoutCache& cache, const Theme& theme, std::stop_token stop_token)
{
    MENDO_PROFILE("EstimateNodeHeights");
    // ノードの種類に応じた既定の高さを割り当て、Y座標を累積計算する。
    // DirectWriteを一切呼ばないため、数千ノードでも数百マイクロ秒で完了する。
    // layout_dirtyフラグは変更しない（後続のViewportLayoutが正しく計測できるようにする）。
    assert(cache.size() == nodes.size());
    const auto node_count = nodes.size();
    MENDO_PLOT("layout.estimate.node_count", static_cast<int64_t>(node_count));

    std::pmr::vector<float> block_heights;
    block_heights.reserve(node_count);

    float y = theme.margin_top;
    for (size_t i = 0; i < node_count; i++) {
        if ((i & 0xFFFu) == 0u && stop_token.stop_requested()) {
            return;
        }
        const auto& node = nodes[i];
        const float h = EstimateNodeHeight(node, theme);
        const float sa = GetSpacingAbove(node, theme);
        const float sb = GetSpacingBelow(node, theme);

        const auto adv = AdvanceNodeY(y, sa, h, sb);
        cache[i].height = h;
        cache[i].text_top = adv.text_top;
        block_heights.push_back(adv.block_height);
    }
    cache.BuildBlockHeights(block_heights);
}

bool EstimateInvisibleNodeHeight(const Node& node, NodeLayoutEntry& entry, const Theme& theme, float node_width) noexcept
{
    // ダイアグラム系コードブロックの高さは描画完了時にビットマップ実寸で確定する。
    // テキスト基準の EstimateNodeHeight で上書きすると描画時に bitmap が後続ノードへ
    // はみ出すため既存値を維持する。
    if (node.type == NodeType::CodeBlock && IsDiagramLanguage(node.code_language())) {
        return false;
    }
    // 同じ幅での実測キャッシュがあればそれを使い、無ければ推定値で成長させる。
    constexpr float kCachedWidthEpsilon = 0.5f;
    const bool cache_hit =
        entry.is_measured() &&
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

YPositionResult RecomputeYPositions(
    std::pmr::vector<Node>& nodes, LayoutCache& cache, const Theme& theme,
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

    // safe_exit_after の契約: 以降のノードは height/sa/sb が不変。よって block_height も不変で
    // Fenwick 更新は不要、text_top は一定 delta のシフトで済む。size_t 飽和は node_count にクランプ。
    const size_t tail_start = (safe_exit_after < node_count) ? safe_exit_after + 1 : node_count;

    // 全件処理時のみ Fenwick を BuildBlockHeights で一括ロード。中間早期終了の可能性がある
    // 場合は per-element Set で逐次同期し、tail-shift パスからも参照可能にする。
    const bool can_bulk_build = (from_index == 0) && (tail_start >= node_count);
    std::pmr::vector<float> block_heights;
    if (can_bulk_build) {
        block_heights.reserve(node_count);
    }

    for (size_t i = from_index; i < tail_start; i++) {
        auto& entry = cache[i];
        if (entry.layout_dirty) {
            result.has_dirty_nodes = true;
        }

        const float sa = GetSpacingAbove(nodes[i], theme);
        const float sb = GetSpacingBelow(nodes[i], theme);

        const auto adv = AdvanceNodeY(y, sa, entry.height, sb);
        entry.text_top = adv.text_top;
        if (can_bulk_build) {
            block_heights.push_back(adv.block_height);
        }
        else {
            cache.SetBlockHeight(i, adv.block_height);
        }
    }

    if (can_bulk_build) {
        cache.BuildBlockHeights(block_heights);
        result.total_height = y + theme.margin_top;
        return result;
    }

    // |delta| < EPSILON なら text_top も実質変化なし → write 自体スキップ (dirty 集計のみ)。
    if (tail_start < node_count) {
        auto& first_tail = cache[tail_start];
        const float sa_first = GetSpacingAbove(nodes[tail_start], theme);
        const float new_text_top = y + sa_first;
        const float delta = new_text_top - first_tail.text_top;

        if (std::abs(delta) < Y_POSITION_EPSILON) {
            if (!result.has_dirty_nodes) {
                result.has_dirty_nodes = std::ranges::any_of(
                    std::views::iota(tail_start, node_count),
                    [&cache](size_t j) { return cache[j].layout_dirty; });
            }
        }
        else {
            // dirty 確定までは layout_dirty を観測しつつシフト、確定後は分岐なしの純粋シフトに
            // 切り替えて auto-vectorize を許可する。
            size_t i = tail_start;
            if (!result.has_dirty_nodes) {
                for (; i < node_count; ++i) {
                    auto& entry = cache[i];
                    entry.text_top += delta;
                    if (entry.layout_dirty) {
                        result.has_dirty_nodes = true;
                        ++i;
                        break;
                    }
                }
            }
            for (; i < node_count; ++i) {
                cache[i].text_top += delta;
            }
        }

        const size_t last_idx = node_count - 1;
        result.total_height = cache[last_idx].text_top + cache[last_idx].height + GetSpacingBelow(nodes[last_idx], theme) + theme.margin_top;
        return result;
    }

    result.total_height = y + theme.margin_top;
    return result;
}

} // namespace mendo::layout
