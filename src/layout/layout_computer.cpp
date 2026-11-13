#include "layout_computer.h"
#include "document_utils.h"
#include "profiler.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <functional>
#include <iterator>
#include <ranges>
#include <string_view>

namespace layout {

namespace {
// この差 (DIP) 未満では後ろのノードをずらさない (ShiftTops を省く)。0.01 DIP は 96 DPI で 1/100 px 未満で、見た目に差が出ない。
constexpr float Y_POSITION_EPSILON = 0.01f;

// nodes[index] の spacing_above を足す前の Y (直前ノードの下端 + spacing_below)。index == 0 は margin_top。
float NodeStartY(std::span<const Node> nodes, const LayoutCache& cache, const Theme& theme, size_t index) noexcept
{
    if (index == 0) {
        return theme.margin_top;
    }
    return cache.Bottom(index - 1) + GetSpacingBelow(nodes[index - 1], theme);
}

// 1 ノード分の Y 進行。戻り値はテキスト上端。加算順序 (above → height → below) は大規模ファイルでの
// catastrophic cancellation 回避のため既存の累積順序を厳密に保持する。
float AdvanceNodeY(float& y, float spacing_above, float height, float spacing_below) noexcept
{
    y += spacing_above;
    const float text_top = y;
    y += height;
    y += spacing_below;
    return text_top;
}

// nodes[from, end) の高さを height_fn(i) で決めながら Top を積む。y は nodes[from] の spacing_above を足す前の Y で、
// 戻ると nodes[end] の手前の Y になる。height_fn は高さを書き換えてよい。
template <class HeightFn>
void AccumulateTops(std::span<const Node> nodes, LayoutCache& cache, const Theme& theme, size_t from, size_t end, float& y, HeightFn&& height_fn)
{
    for (size_t i = from; i < end; i++) {
        const float h = height_fn(i);
        cache.SetTop(i, AdvanceNodeY(y, GetSpacingAbove(nodes[i], theme), h, GetSpacingBelow(nodes[i], theme)));
    }
}

// node_start を超える最初の source offset。見つからなければ fallback。
size_t NextSourceOffset(std::span<const Node> nodes, size_t node_start, size_t fallback) noexcept
{
    const auto it = std::ranges::upper_bound(nodes, node_start, {}, &Node::SourceOffset);
    return it != nodes.end() ? it->SourceOffset() : fallback;
}
} // namespace

float GetSpacingAbove(const Node& node, const Theme& theme) noexcept
{
    if (node.continues_prev()) {
        return 0.0f;
    }
    switch (node.type) {
    case NodeType::Heading:
        return theme.heading_spacing_above;
    case NodeType::CodeBlock:
    case NodeType::Diagram:
        return theme.code_block_spacing_above;
    case NodeType::Paragraph:
        return node.quote_depth > 0 ? theme.code_block_spacing_above : 0.0f;
    case NodeType::HorizontalRule:
    case NodeType::ListItem:
    case NodeType::Table:
    case NodeType::TaskListItem:
        return 0.0f;
    }
    std::unreachable();
}

float GetSpacingBelow(const Node& node, const Theme& theme) noexcept
{
    if (node.continues_next()) {
        return 0.0f;
    }
    switch (node.type) {
    case NodeType::Heading:
        // h1/h2 は下線を描くため、下線と次行の余白を確保すべく大きめの値を返す。
        return (node.heading_level <= 2) ? theme.heading_spacing_below_h1h2 : theme.heading_spacing_below;
    case NodeType::CodeBlock:
    case NodeType::Diagram:
        return theme.paragraph_spacing + theme.code_block_spacing_above;
    case NodeType::ListItem:
    case NodeType::TaskListItem:
        // 空 LI に sb を入れると直下 P の text_top が下ズレし bullet と分離する (issue#237)。
        return IsEmptyListItemContainer(node) ? 0.0f : theme.list_item_spacing;
    case NodeType::HorizontalRule:
        return 0.0f;
    case NodeType::Table:
    case NodeType::Paragraph:
        return theme.paragraph_spacing;
    }
    std::unreachable();
}

void ComputeColumnWidths(std::vector<float>& out, std::span<const float> natural_widths, std::span<const float> min_widths, float available_width)
{
    out.resize(natural_widths.size());
    // GitHub (Chrome の自動の表組み、width: max-content; max-width: 100%) と同じく、収まる表は広げずに自然幅で並べる。
    const float total_natural = std::ranges::fold_left(natural_widths, 0.0f, std::plus{});
    if (total_natural <= available_width) {
        std::ranges::copy(natural_widths, out.begin());
        return;
    }
    // 収まらなければ各列を最小幅 (最も長い単語) までは縮め、余りを (自然幅 - 最小幅) に比例して配る。番号のような狭い列は
    // 自然幅のまま残り、長い文の列が折り返す。最小幅でも収まらなければ最小幅で横スクロールに任せる。
    const float total_min = std::ranges::fold_left(min_widths, 0.0f, std::plus{});
    if (total_min >= available_width) {
        std::ranges::copy(min_widths, out.begin());
        return;
    }
    const float ratio = (available_width - total_min) / (total_natural - total_min);
    for (auto [w, nw, mw] : std::views::zip(out, natural_widths, min_widths)) {
        w = mw + (nw - mw) * ratio;
    }
}

float EstimateNodeHeight(const Node& node, const Theme& theme, float node_width) noexcept
{
    const float line_height = theme.font_size_body * BODY_LINE_HEIGHT_FACTOR;
    switch (node.type) {
    case NodeType::Heading:
        return theme.GetHeadingSize(node.heading_level) * 1.5f;
    case NodeType::CodeBlock:
    case NodeType::Diagram: {
        // 段落と同じく少なめに倒す。Cascadia Mono 14px の 1 行は実測 16.27 DIP (1.162 倍)、日本語を含むと 17.45。
        const int lines = 1 + node.line_count;
        const float h = theme.font_size_code * 1.16f * static_cast<float>(lines);
        return std::max(h, line_height);
    }
    case NodeType::HorizontalRule:
        return HorizontalRuleHeight(theme);
    case NodeType::Table:
        return line_height * 1.5f * static_cast<float>(node.table_data()->row_count);
    case NodeType::ListItem:
    case NodeType::TaskListItem:
    case NodeType::Paragraph:
        if (!node.HasText()) {
            return EmptyTextNodeHeight(node, theme);
        }
        float lines = static_cast<float>(1 + node.line_count);
        if (node_width > 0.0f) {
            // ASCII は 1 バイトで約 0.5em、CJK は 3 バイトで約 1em。文字を数えずバイト数だけで見積もり、
            // 少なめに倒す (多すぎると初回の計測範囲が足りず計測をやり直す)。
            constexpr float kEmPerByte = 0.4f;
            const float text_width = static_cast<float>(node.GetText().size()) * kEmPerByte * theme.font_size_body;
            lines += std::floor(text_width / node_width);
        }
        return line_height * lines;
    }
    std::unreachable();
}

void EstimateNodeHeights(std::span<const Node> nodes, LayoutCache& cache, const Theme& theme, float content_width, std::stop_token stop_token)
{
    MENDO_PROFILE("EstimateNodeHeights");
    // DirectWriteを一切呼ばないため、数千ノードでも数百マイクロ秒で完了する。
    // layout_dirtyフラグは変更しない（後続のViewportLayoutが正しく計測できるようにする）。
    assert(cache.size() == nodes.size());
    const auto node_count = nodes.size();
    MENDO_PLOT("layout.estimate.node_count", static_cast<int64_t>(node_count));

    constexpr size_t kStopCheckInterval = 4096;
    float y = theme.margin_top;
    for (size_t from = 0; from < node_count; from += kStopCheckInterval) {
        if (stop_token.stop_requested()) {
            return;
        }
        AccumulateTops(nodes, cache, theme, from, std::min(from + kStopCheckInterval, node_count), y, [&](size_t i) {
            const auto& node = nodes[i];
            return cache[i].height = EstimateNodeHeight(node, theme, content_width > 0.0f ? NodeContentWidth(node, theme, content_width) : 0.0f);
        });
    }
}

void ReestimateNodeHeights(std::span<const Node> nodes, LayoutCache& cache, const Theme& theme, float content_width)
{
    MENDO_PROFILE("ReestimateNodeHeights");
    assert(cache.size() == nodes.size());
    float y = theme.margin_top;
    AccumulateTops(nodes, cache, theme, 0, nodes.size(), y, [&](size_t i) {
        auto& entry = cache[i];
        entry.layout_dirty = true;
        EstimateInvisibleNodeHeight(nodes[i], entry, theme, NodeContentWidth(nodes[i], theme, content_width));
        return entry.height;
    });
}

size_t CarryOverUnchanged(
    std::span<const Node> old_nodes, LayoutCache& old_cache,
    std::span<const Node> new_nodes, LayoutCache& new_cache, const UnchangedEnds& ends)
{
    MENDO_PROFILE("layout::CarryOverUnchanged");
    assert(old_cache.size() == old_nodes.size() && new_cache.size() == new_nodes.size());
    assert(ends.old_count == old_nodes.size() && ends.new_count == new_nodes.size());
    new_cache.TakeEntries(old_cache, 0, 0, ends.head);
    new_cache.TakeEntries(old_cache, ends.OldTailBegin(), ends.NewTailBegin(), ends.tail);

    // 図のコードを書き換えて保存すると、描き直しの間プレースホルダに戻って下の文章が 2 回跳ねる。
    // 対応が付く図は旧結果を仮表示として引き継ぐ。
    size_t stale = 0;
    for (const auto [old_index, new_index] : PairChangedDiagrams(old_nodes, new_nodes, ends)) {
        stale += new_cache.TakeDiagramAsStale(old_cache, old_index, new_index) ? 1 : 0;
    }
    return ends.head + ends.tail + stale;
}

LayoutCache MakeEstimatedLayoutCache(std::span<const Node> nodes, const Theme& theme, float content_width, std::stop_token stop_token)
{
    LayoutCache cache(nodes.size());
    EstimateNodeHeights(nodes, cache, theme, content_width, std::move(stop_token));
    if (content_width > 0.0f) {
        cache.SetEstimateBasis({ content_width, theme.zoom });
    }
    return cache;
}

void EstimateInvisibleNodeHeight(const Node& node, NodeLayoutEntry& entry, const Theme& theme, float node_width) noexcept
{
    // 図の高さは寸法が届いた時点で DiagramNodeHeight が決める。推定で上書きすると
    // 描画時に図が後続ノードへはみ出すため既存値を維持する。
    if (node.type == NodeType::Diagram) {
        return;
    }
    // 同じ幅で実測していれば、height は実測値かそれ以上なので推定は要らない。
    constexpr float kCachedWidthEpsilon = 0.5f;
    if (entry.is_measured() && std::abs(entry.cached_width - NodeLayoutWidth(node, node_width)) < kCachedWidthEpsilon) {
        return;
    }
    // テーブルや折り返しが多い段落では推定値が実測値を大きく下回るため、
    // シュリンク方向の更新は後続ノードと重なる原因になる。よって既存値より小さくはしない。
    // 表の幾何は推定で伸ばした高さと食い違うが、計測が幾何の高さ (TotalHeight) で確定し直す。
    entry.height = std::max(entry.height, EstimateNodeHeight(node, theme, node_width));
}

void RecomputeYPositions(
    std::span<const Node> nodes, LayoutCache& cache, const Theme& theme,
    size_t from_index, size_t safe_exit_after) noexcept
{
    MENDO_PROFILE("RecomputeYPositions");
    const auto node_count = nodes.size();
    float y = NodeStartY(nodes, cache, theme, from_index);

    // safe_exit_after の契約: 以降のノードは height/sa/sb が不変なので、
    // text_top は一定 delta のシフトで済む。size_t 飽和は node_count にクランプ。
    const size_t tail_start = (safe_exit_after < node_count) ? safe_exit_after + 1 : node_count;

    AccumulateTops(nodes, cache, theme, from_index, tail_start, y, [&cache](size_t i) noexcept { return cache[i].height; });

    if (tail_start < node_count) {
        const float new_text_top = y + GetSpacingAbove(nodes[tail_start], theme);
        const float delta = new_text_top - cache.Top(tail_start);
        if (std::abs(delta) >= Y_POSITION_EPSILON) {
            cache.ShiftTops(tail_start, delta);
        }
    }
}

float CalcScrollYForDiff(
    std::span<const Node> nodes,
    const LayoutCache& cache,
    size_t content_size,
    size_t diff_pos,
    float viewport_height,
    float fallback_scroll) noexcept
{
    if (diff_pos == std::string_view::npos) {
        return fallback_scroll;
    }
    assert(cache.size() == nodes.size());
    const auto changed_node = FindNodeBySourceOffset(nodes, diff_pos);
    if (changed_node < 0) {
        return fallback_scroll;
    }

    float node_y = cache.Top(static_cast<size_t>(changed_node));
    const float node_h = cache[changed_node].height;

    const auto node_start = nodes[changed_node].SourceOffset();
    const size_t next_start = NextSourceOffset(nodes, node_start, content_size);
    if (next_start > node_start) {
        const float fraction = static_cast<float>(diff_pos - node_start) / static_cast<float>(next_start - node_start);
        node_y += node_h * std::min(fraction, 1.0f);
    }

    const float margin = viewport_height * 0.2f;
    return std::max(0.0f, node_y - margin);
}

int FindActiveTocIndex(std::span<const TocEntry> entries, const LayoutCache& cache, float scroll_y, float margin) noexcept
{
    // cache.Top(entries[i].node_index) <= scroll_y + margin を満たす最大の i を求める。
    const float threshold = scroll_y + margin;
    const auto it = std::ranges::partition_point(entries, [&](const TocEntry& e) noexcept {
        return cache.Top(static_cast<size_t>(e.node_index)) <= threshold;
    });
    return (it != entries.begin()) ? static_cast<int>(std::prev(it) - entries.begin()) : -1;
}

} // namespace layout
