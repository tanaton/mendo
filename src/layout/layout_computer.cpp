#include "layout_computer.h"
#include "document_utils.h"
#include "profiler.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <functional>
#include <iterator>
#include <ranges>

namespace layout {

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

// nodes[index] の spacing_above を足す前の Y (直前ノードの下端 + spacing_below)。index == 0 は margin_top。
float NodeStartY(const std::vector<Node>& nodes, const LayoutCache& cache, const Theme& theme, size_t index) noexcept
{
    if (index == 0) {
        return theme.margin_top;
    }
    return cache.Bottom(index - 1) + GetSpacingBelow(nodes[index - 1], theme);
}

// nodes[from, end) の高さを height_fn(i) で決めながら Top を積む。y は nodes[from] の spacing_above を足す前の Y で、
// 戻ると nodes[end] の手前の Y になる。height_fn は高さを書き換えてよい。
template <class HeightFn>
void AccumulateTops(const std::vector<Node>& nodes, LayoutCache& cache, const Theme& theme, size_t from, size_t end, float& y, HeightFn&& height_fn)
{
    for (size_t i = from; i < end; i++) {
        const float h = height_fn(i);
        cache.SetTop(i, AdvanceNodeY(y, GetSpacingAbove(nodes[i], theme), h, GetSpacingBelow(nodes[i], theme)));
    }
}

// node_index より後ろで node_start を超える最初の source offset。見つからなければ fallback。
// 途中に offset 無しノード (HR や空のルーズリスト等) が連続しても取り逃さないよう探索するが、
// 末尾まで線形走査すると大規模ファイルのリロードが重くなるため上限を設ける
// (通常は最初の有効 offset で見つかるため上限には届かない)。
size_t NextSourceOffset(const std::vector<Node>& nodes, int node_index, size_t node_start, size_t fallback) noexcept
{
    constexpr int kMaxOffsetProbe = 4096;
    const auto probe = nodes | std::views::drop(node_index + 1) | std::views::take(kMaxOffsetProbe);
    const auto it = std::ranges::find_if(probe, [node_start](const Node& n) noexcept { return n.HasSourceOffset() && n.SourceOffset() > node_start; });
    return it != probe.end() ? it->SourceOffset() : fallback;
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
    if (node.continues_next()) {
        return 0.0f;
    }
    switch (node.type) {
    case NodeType::Heading:
        // h1/h2 は下線を描くため、下線と次行の余白を確保すべく大きめの値を返す。
        return (node.heading_level <= 2) ? theme.heading_spacing_below_h1h2 : theme.heading_spacing_below;
    case NodeType::CodeBlock:
    case NodeType::Diagram:
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

void ComputeColumnWidths(std::vector<float>& out, std::span<const float> natural_widths, float available_width)
{
    const size_t col_count = natural_widths.size();
    out.resize(col_count);
    if (col_count == 0) {
        return;
    }
    const float effective_available = std::max(available_width, static_cast<float>(col_count) * MIN_COLUMN_WIDTH);

    const float total_natural = std::ranges::fold_left(natural_widths, 0.0f, std::plus{});

    if (total_natural > 0 && total_natural > effective_available) {
        // 比例圧縮で最も狭い列が MIN_COLUMN_WIDTH を割る場合は、無理に押し込めず
        // 自然幅で出して横スクロールに任せる方が読みやすい。
        const float smallest = *std::ranges::min_element(natural_widths);
        if (smallest * effective_available < MIN_COLUMN_WIDTH * total_natural) {
            std::ranges::copy(natural_widths, out.begin());
            return;
        }
        for (auto [w, nw] : std::views::zip(out, natural_widths)) {
            w = effective_available * nw / total_natural;
        }
    }
    else {
        // 一律 max(nw + pad, even) だと幅広列の超過分だけ合計が available を超え、
        // 自然幅なら収まる表まで横スクロールになる。
        // 幅広列に nw + pad を確保し、残りを他列で均等に分けて合計を available に揃える。
        const auto columns = std::views::zip(out, natural_widths);
        const float n = static_cast<float>(col_count);
        const float demand_total = total_natural + n * COLUMN_WIDTH_PADDING;
        if (demand_total > effective_available) {
            // 余白込みでは収まらないが自然幅は収まる。余りを均等に足し、列が自然幅を割らないようにする。
            const float extra = (effective_available - total_natural) / n;
            for (auto [w, nw] : columns) {
                w = nw + extra;
            }
            return;
        }
        // level を下げるたびに nw + pad > level の列が増えるだけなので、列数回以内で収束する。
        float level = effective_available / n;
        size_t flex = col_count;
        for (;;) {
            float fixed_total = 0.0f;
            size_t next_flex = 0;
            for (const float nw : natural_widths) {
                if (nw + COLUMN_WIDTH_PADDING > level) {
                    fixed_total += nw + COLUMN_WIDTH_PADDING;
                }
                else {
                    ++next_flex;
                }
            }
            if (next_flex == flex || next_flex == 0) {
                break;
            }
            flex = next_flex;
            level = (effective_available - fixed_total) / static_cast<float>(flex);
        }
        for (auto [w, nw] : columns) {
            w = std::max(nw + COLUMN_WIDTH_PADDING, level);
        }
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
    case NodeType::Table: {
        const size_t row_count = node.has_table() ? std::max<size_t>(1u, node.table_data()->row_count) : 1u;
        return line_height * 1.5f * static_cast<float>(row_count);
    }
    case NodeType::Image:
        return PlaceholderHeight(theme);
    case NodeType::ListItem:
    case NodeType::TaskListItem:
    case NodeType::Paragraph:
    case NodeType::BlockQuote:
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

void EstimateNodeHeights(const std::vector<Node>& nodes, LayoutCache& cache, const Theme& theme, float content_width, std::stop_token stop_token)
{
    MENDO_PROFILE("EstimateNodeHeights");
    // ノードの種類に応じた既定の高さを割り当て、Y座標を累積計算する。
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

void ReestimateNodeHeights(const std::vector<Node>& nodes, LayoutCache& cache, const Theme& theme, float content_width)
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

CarryOverResult CarryOverUnchanged(
    const std::vector<Node>& old_nodes, LayoutCache& old_cache,
    const std::vector<Node>& new_nodes, LayoutCache& new_cache, const UnchangedEnds& ends)
{
    MENDO_PROFILE("layout::CarryOverUnchanged");
    assert(old_cache.size() == old_nodes.size() && new_cache.size() == new_nodes.size());
    assert(ends.old_count == old_nodes.size() && ends.new_count == new_nodes.size());
    new_cache.TakeEntries(old_cache, 0, 0, ends.head);
    new_cache.TakeEntries(old_cache, ends.OldTailBegin(), ends.NewTailBegin(), ends.tail);

    // 図のコードを書き換えて保存すると、描き直しの間プレースホルダに戻って下の文章が 2 回跳ねる。
    // 変わった範囲の図の数が新旧で同じなら、出現順に旧結果を仮表示として引き継ぐ。
    const auto diagrams_in = [](const std::vector<Node>& nodes, size_t begin, size_t end) {
        std::vector<size_t> indices;
        for (size_t i = begin; i < end; ++i) {
            if (nodes[i].type == NodeType::Diagram) {
                indices.push_back(i);
            }
        }
        return indices;
    };
    const auto old_diagrams = diagrams_in(old_nodes, ends.head, ends.OldTailBegin());
    const auto new_diagrams = diagrams_in(new_nodes, ends.head, ends.NewTailBegin());
    size_t stale = 0;
    if (old_diagrams.size() == new_diagrams.size()) {
        for (size_t k = 0; k < old_diagrams.size(); ++k) {
            stale += new_cache.TakeDiagramAsStale(old_cache, old_diagrams[k], new_diagrams[k]) ? 1 : 0;
        }
    }
    return { .ends = ends, .carried = ends.head + ends.tail + stale };
}

LayoutCache MakeEstimatedLayoutCache(const std::vector<Node>& nodes, const Theme& theme, float content_width, std::stop_token stop_token)
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
    // 画像・図の高さは寸法が届いた時点で BitmapNodeHeight が決める。推定で上書きすると
    // 描画時にビットマップが後続ノードへはみ出すため既存値を維持する。
    if (IsBitmapDrawNode(node)) {
        return;
    }
    // 同じ幅での実測キャッシュがあればそれを使い、無ければ推定値で成長させる。
    constexpr float kCachedWidthEpsilon = 0.5f;
    const bool cache_hit =
        entry.is_measured() &&
        std::abs(entry.cached_width - NodeLayoutWidth(node, node_width)) < kCachedWidthEpsilon &&
        entry.cached_height > 0.0f;
    const float fallback = cache_hit ? entry.cached_height : EstimateNodeHeight(node, theme, node_width);
    // テーブルや折り返しが多い段落では推定値が実測値を大きく下回るため、
    // シュリンク方向の更新は後続ノードと重なる原因になる。よって既存値より小さくはしない。
    // 表の幾何は推定で伸ばした高さと食い違うが、計測が幾何の高さ (TotalHeight) で確定し直す。
    entry.height = std::max(entry.height, fallback);
}

void RecomputeYPositions(
    const std::vector<Node>& nodes, LayoutCache& cache, const Theme& theme,
    size_t from_index, size_t safe_exit_after) noexcept
{
    MENDO_PROFILE("RecomputeYPositions");
    const auto node_count = nodes.size();
    float y = (from_index < node_count) ? NodeStartY(nodes, cache, theme, from_index) : theme.margin_top;

    // safe_exit_after の契約: 以降のノードは height/sa/sb が不変なので、
    // text_top は一定 delta のシフトで済む。size_t 飽和は node_count にクランプ。
    const size_t tail_start = (safe_exit_after < node_count) ? safe_exit_after + 1 : node_count;

    AccumulateTops(nodes, cache, theme, from_index, tail_start, y, [&cache](size_t i) noexcept { return cache[i].height; });

    if (tail_start < node_count) {
        const float new_text_top = y + GetSpacingAbove(nodes[tail_start], theme);
        const float delta = new_text_top - cache.Top(tail_start);
        // |delta| < EPSILON なら実質変化なしなので write 自体スキップする。
        if (std::abs(delta) >= Y_POSITION_EPSILON) {
            cache.ShiftTops(tail_start, delta);
        }
    }
}

float CalcScrollYForDiff(
    const std::vector<Node>& nodes,
    const LayoutCache& cache,
    std::string_view content,
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

    // FindNodeBySourceOffset はソース位置を持つノードだけを返す。
    const auto node_start = nodes[changed_node].SourceOffset();
    const size_t next_start = NextSourceOffset(nodes, changed_node, node_start, content.size());
    if (next_start > node_start) {
        const float fraction = static_cast<float>(diff_pos - node_start) / static_cast<float>(next_start - node_start);
        node_y += node_h * std::min(fraction, 1.0f);
    }

    const float margin = viewport_height * 0.2f;
    return std::max(0.0f, node_y - margin);
}

int FindActiveTocIndex(std::span<const TocEntry> entries, const LayoutCache& cache, float scroll_y, float margin) noexcept
{
    const int n = static_cast<int>(entries.size());
    if (n == 0) {
        return -1;
    }
    // cache[entries[i].node_index].text_top <= scroll_y + margin を満たす最大の i を求める。
    const float threshold = scroll_y + margin;
    const auto it = std::ranges::partition_point(entries, [&](const TocEntry& e) noexcept {
        return cache.Top(static_cast<size_t>(e.node_index)) <= threshold;
    });
    return (it != entries.begin()) ? static_cast<int>(std::prev(it) - entries.begin()) : -1;
}

} // namespace layout
