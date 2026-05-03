#include "layout.h"
#include "document.h"
#include "profiler.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <ranges>

static constexpr float MIN_COLUMN_WIDTH = 30.0f;
static constexpr float COLUMN_WIDTH_PADDING = 4.0f;
static constexpr float Y_POSITION_EPSILON = 0.01f; // Y座標の早期終了判定用許容誤差（DIP単位）

static float GetSpacingAbove(const Node& node, const Theme& theme) noexcept
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

static float GetSpacingBelow(const Node& node, const Theme& theme) noexcept
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

    const float total_natural = std::ranges::fold_left(
        natural_widths,
        0.0f,
        [](float a, float b) static noexcept { return a + b; });

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

std::pmr::wstring BuildLinearizedTableText(const std::pmr::vector<TableRow>& rows)
{
    // 実セルサイズ + 区切り文字数を集計して正確に reserve する。
    size_t total = 0;
    for (const auto& row : rows) {
        for (const auto& cell : row.cells) {
            total += cell.text.size();
        }
        if (!row.cells.empty()) {
            total += row.cells.size() - 1; // タブ区切り
        }
    }
    if (!rows.empty()) {
        total += rows.size() - 1; // 行区切り（末尾改行なし）
    }

    std::pmr::wstring text;
    text.reserve(total);
    const auto row_count = rows.size();
    const auto last_row = static_cast<ptrdiff_t>(row_count) - 1;
    for (const auto& [r, row] : rows | std::views::enumerate) {
        for (const auto& [c, cell] : row.cells | std::views::enumerate) {
            if (c > 0) {
                text += L'\t';
            }
            text += cell.text;
        }
        if (r < last_row) {
            text += L'\n';
        }
    }
    return text;
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
        const size_t row_count = node.has_table() ? std::max(1uz, node.table_rows().size()) : 1uz;
        return line_height * 1.5f * static_cast<float>(row_count);
    }
    case NodeType::Image:
        return std::max(60.0f, theme.font_size_body * 3.0f);
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

void EstimateNodeHeights(const std::pmr::vector<Node>& nodes, LayoutCache& cache, const Theme& theme) noexcept
{
    MENDO_PROFILE("EstimateNodeHeights");
    // ノードの種類に応じた既定の高さを割り当て、Y座標を累積計算する。
    // DirectWriteを一切呼ばないため、数千ノードでも数百マイクロ秒で完了する。
    // layout_dirtyフラグは変更しない（後続のViewportLayoutが正しく計測できるようにする）。
    assert(cache.size() >= nodes.size());
    const auto node_count = nodes.size();
    MENDO_PLOT("layout.estimate.node_count", static_cast<int64_t>(node_count));

    float y = theme.margin_top;
    for (size_t i = 0; i < node_count; i++) {
        const auto& node = nodes[i];
        const float h = EstimateNodeHeight(node, theme);

        y += GetSpacingAbove(node, theme);
        cache[i].height = h;
        cache[i].y_position = y;
        y += h;
        y += GetSpacingBelow(node, theme);
    }
}

YPositionResult RecomputeYPositions(std::pmr::vector<Node>& nodes, LayoutCache& cache, const Theme& theme,
                                    size_t from_index, bool has_earlier_dirty, size_t safe_exit_after) noexcept
{
    MENDO_PROFILE("RecomputeYPositions");
    YPositionResult result;
    result.has_dirty_nodes = has_earlier_dirty;
    const auto node_count = nodes.size();
    float y = theme.margin_top;

    // 途中から開始する場合、前のノードの終了位置から再開する。
    if (from_index > 0 && from_index < node_count) {
        auto& prev = cache[from_index - 1];
        y = prev.y_position + prev.height;
        y += GetSpacingBelow(nodes[from_index - 1], theme);
    }

    for (size_t i = from_index; i < node_count; i++) {
        auto& entry = cache[i];
        if (entry.layout_dirty) {
            result.has_dirty_nodes = true;
        }

        y += GetSpacingAbove(nodes[i], theme);

        // safe_exit_after 以降でY位置が一致すれば、以降のノードのY位置は変わらないので早期終了する。
        if (i > safe_exit_after && std::abs(entry.y_position - y) < Y_POSITION_EPSILON) {
            // Y更新より軽量なフラグチェックのみで残りのダーティノードを確認
            if (!result.has_dirty_nodes) {
                result.has_dirty_nodes = std::ranges::any_of(
                    std::views::iota(i, node_count),
                    [&cache](size_t j) { return cache[j].layout_dirty; });
            }
            const size_t last_idx = node_count - 1;
            result.total_height = cache[last_idx].y_position + cache[last_idx].height + GetSpacingBelow(nodes[last_idx], theme) + theme.margin_top;
            return result;
        }

        entry.y_position = y;
        y += entry.height;

        y += GetSpacingBelow(nodes[i], theme);
    }

    result.total_height = y + theme.margin_top;
    return result;
}

bool LayoutEngine::Init(ITextMeasurer* measurer, const Theme& theme)
{
    measurer_ = measurer;
    theme_ = &theme;
    return measurer_->Init(theme);
}

bool LayoutEngine::RecreateFormats()
{
    if (!measurer_) {
        return false;
    }
    last_viewport_width_ = 0.0f;
    return measurer_->RecreateFormats();
}

void LayoutEngine::ComputeLayout(std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width, float viewport_top, float viewport_bottom)
{
    MENDO_PROFILE("LayoutEngine::ComputeLayout");
    const auto node_count = nodes.size();
    cache.Resize(node_count);

    // 小刻みな WM_SIZE で全ノード再レイアウトが頻発するのを防ぐ
    static constexpr float WIDTH_CHANGE_THRESHOLD = 2.0f;
    const bool width_changed = std::abs(viewport_width - last_viewport_width_) > WIDTH_CHANGE_THRESHOLD;
    const bool partial = (viewport_top >= 0.0f);

    if (width_changed) {
        last_viewport_width_ = viewport_width;
    }

    const float content_width = theme_->ContentWidth(viewport_width);
    float y = theme_->margin_top;
    bool any_dirty = false;
    bool any_height_changed = false;
    bool any_measured = false;
    bool broke_early = false;

    for (size_t i = 0; i < node_count; i++) {
        auto& node = nodes[i];
        auto& entry = cache[i];
        const float indent = NodeIndent(node, *theme_);
        const float node_width = content_width - indent;

        const bool needs_layout = width_changed || entry.layout_dirty;

        if (needs_layout) {
            if (partial) {
                const float node_bottom = y + entry.height; // 古い高さを使って推定
                const bool visible = (node_bottom >= viewport_top && y <= viewport_bottom);
                if (visible) {
                    const float old_height = entry.height;
                    measurer_->MeasureNode(node, entry, node_width);
                    any_measured = true;
                    if (entry.height != old_height) {
                        any_height_changed = true;
                    }
                }
                else {
                    // 不可視ノードは MeasureNode をスキップするが、entry.height が
                    // 旧テーマ/ズーム時の値のままだと total_height_ にそれが
                    // 反映され、直後の SyncMaxScroll が不正確な max_scroll を
                    // 計算しうる。現テーマでの推定値で素早く更新しておき、
                    // 厳密値は後続の ProcessDirtyBatch / EnsureVisibleLayout に委ねる。
                    // ただしダイアグラム系コードブロックの高さは描画完了時にビットマップ
                    // 実寸で確定する。テキスト基準の EstimateNodeHeight で上書きすると
                    // 描画時に bitmap が後続ノードへはみ出すため既存値を維持する。
                    // また、テーブル/画像/折り返しが多い段落では推定値が実測値を
                    // 大きく下回るため、シュリンク方向の更新も後続ノードと重なる
                    // 原因になる。よって既存値より小さくはしない。
                    const bool is_diagram = (node.type == NodeType::CodeBlock && IsDiagramLanguage(node.code_language));
                    if (!is_diagram) {
                        const float estimated = EstimateNodeHeight(node, *theme_);
                        if (entry.height < estimated) {
                            entry.height = estimated;
                            any_height_changed = true;
                            // entry.height は推定値に成長したが、テーブルの row_heights
                            // 合計や col_widths は旧値のまま乖離する。次の MeasureNode が
                            // 走るまで描画を抑制し、描画範囲(row_heights 合計)が
                            // 後続ノード位置(entry.height ベース)を越えて重なるのを防ぐ。
                            if (node.type == NodeType::Table && entry.has_table_layout()) {
                                entry.table_layout->col_widths.clear();
                                entry.table_layout->cached_table_width = 0.0f;
                            }
                        }
                    }
                    entry.layout_dirty = true;
                }
            }
            else {
                measurer_->MeasureNode(node, entry, node_width);
                any_measured = true;
            }
        }

        if (entry.layout_dirty) {
            any_dirty = true;
        }

        y += GetSpacingAbove(node, *theme_);
        entry.y_position = y;
        y += entry.height;
        y += GetSpacingBelow(node, *theme_);

        // 部分モードで幅の変更がなく、ビューポートを超えた後に
        // 高さの変更もなければ、残りの Y 位置は変わらないので早期終了する。
        if (partial && !width_changed && !any_height_changed && y > viewport_bottom) {
            // 中断地点より先にダーティノードが存在する可能性を保守的に仮定する。
            // ProcessDirtyBatch が存在しない場合は速やかに確認・クリアする。
            any_dirty = true;
            broke_early = true;
            break;
        }
    }

    if (!broke_early) {
        total_height_ = y + theme_->margin_top;
    }
    has_dirty_nodes_ = any_dirty;

    if (any_measured) {
        cache.IncrementEffectsGeneration();
    }

    MENDO_PLOT("layout.compute.partial", static_cast<int64_t>(partial));
    MENDO_PLOT("layout.compute.width_changed", static_cast<int64_t>(width_changed));
    MENDO_PLOT("layout.compute.node_count", static_cast<int64_t>(node_count));
    MENDO_PLOT("layout.compute.broke_early", static_cast<int64_t>(broke_early));
}

void LayoutEngine::LayoutNodes(std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width)
{
    last_viewport_width_ = 0.0f; // 幅の変更検出を強制する
    // 逆変換: content→viewport
    ComputeLayout(nodes, cache, viewport_width + theme_->margin_left + theme_->margin_right);
}

bool LayoutEngine::EnsureVisibleLayout(std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width, float viewport_top, float viewport_bottom)
{
    MENDO_PROFILE("LayoutEngine::EnsureVisibleLayout");
    const float content_width = theme_->ContentWidth(viewport_width);
    bool any_updated = false;
    int last_measured = -1;

    const auto node_count = nodes.size();
    const int lo = FindFirstVisibleNodeIndex(cache, node_count, viewport_top);

    for (int i = lo; i < static_cast<int>(node_count); i++) {
        auto& entry = cache[i];
        if (entry.y_position > viewport_bottom) {
            break;
        }
        if (!entry.layout_dirty) {
            continue;
        }
        const float indent = NodeIndent(nodes[i], *theme_);
        measurer_->MeasureNode(nodes[i], entry, content_width - indent);
        any_updated = true;
        last_measured = i;
    }

    if (any_updated) {
        cache.IncrementEffectsGeneration();
        const auto result = RecomputeYPositions(nodes, cache, *theme_,
                                                static_cast<size_t>(lo), has_dirty_nodes_, static_cast<size_t>(last_measured));
        total_height_ = result.total_height;
        has_dirty_nodes_ = result.has_dirty_nodes;
    }
    return any_updated;
}

bool LayoutEngine::ProcessDirtyBatch(std::pmr::vector<Node>& nodes, LayoutCache& cache,
                                     float viewport_width, int batch_size, int time_budget_us,
                                     float viewport_top, float viewport_height, float buffer_screens)
{
    MENDO_PROFILE("LayoutEngine::ProcessDirtyBatch");
    const float content_width = theme_->ContentWidth(viewport_width);
    const auto node_count = nodes.size();
    int processed = 0;
    size_t first_dirty = node_count;
    size_t last_processed = 0;

    const bool has_viewport_limit = (viewport_top >= 0.0f && viewport_height > 0.0f);
    const float limit_top = has_viewport_limit ? viewport_top - viewport_height * buffer_screens : 0.0f;
    const float limit_bottom = has_viewport_limit ? viewport_top + viewport_height + viewport_height * buffer_screens : 0.0f;

    const bool has_budget = (time_budget_us > 0);
    const auto start = has_budget ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    // メインループ中にビューポート付近にスキップされたダーティノードがあるか追跡する。
    // 二重 O(n) スキャンを回避するため。
    bool any_nearby_dirty_skipped = false;

    for (size_t i = 0; i < node_count; i++) {
        auto& entry = cache[i];
        if (!entry.layout_dirty) {
            continue;
        }

        if (has_viewport_limit && IsOffscreen(entry.y_position, entry.height, limit_top, limit_bottom)) {
            continue;
        }

        // MeasureNode の前に判定し超過分を抑えるが、進行保証のため最低1ノードは処理する。
        if (has_budget && processed > 0) {
            const auto elapsed = std::chrono::steady_clock::now() - start;
            if (std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() >= time_budget_us) {
                any_nearby_dirty_skipped = true;
                break;
            }
        }

        if (first_dirty == nodes.size()) {
            first_dirty = i;
        }
        const float indent = NodeIndent(nodes[i], *theme_);
        measurer_->MeasureNode(nodes[i], entry, content_width - indent);
        last_processed = i;

        if (++processed >= batch_size) {
            any_nearby_dirty_skipped = true;
            break;
        }
    }

    MENDO_PLOT("layout.dirty_batch.processed", static_cast<int64_t>(processed));

    if (processed == 0) {
        has_dirty_nodes_ = false;
        return false;
    }

    cache.IncrementEffectsGeneration();
    const auto result = RecomputeYPositions(nodes, cache, *theme_, first_dirty, false, last_processed);
    total_height_ = result.total_height;
    has_dirty_nodes_ = result.has_dirty_nodes;

    // ビューポート制限時: 付近のダーティノードが全て処理済みなら完了とみなす。
    // 遠方のダーティノードはスクロール時に EnsureVisibleLayout で処理される。
    if (has_viewport_limit && has_dirty_nodes_ && !any_nearby_dirty_skipped) {
        has_dirty_nodes_ = false;
    }

    if (!has_dirty_nodes_) {
        last_viewport_width_ = viewport_width;
    }
    return has_dirty_nodes_;
}

void LayoutService::ViewportLayout(Document& doc, LayoutCache& cache, float width, float height)
{
    const float scroll_y = viewport_.GetScrollY();
    engine_.ComputeLayout(doc.GetNodesMut(), cache, width, scroll_y, scroll_y + height);
    viewport_.ApplyScrollTarget(cache);
}

bool LayoutService::ProcessDirtyBatch(Document& doc, LayoutCache& cache, float width, int batch_size, int time_budget_us, float viewport_height, float buffer_screens)
{
    bool more;
    if (viewport_height > 0.0f) {
        const float vp_top = viewport_.GetScrollY();
        more = engine_.ProcessDirtyBatch(doc.GetNodesMut(), cache, width, batch_size, time_budget_us, vp_top, viewport_height, buffer_screens);
    }
    else {
        more = engine_.ProcessDirtyBatch(doc.GetNodesMut(), cache, width, batch_size, time_budget_us);
    }
    viewport_.ApplyScrollTarget(cache);
    return more;
}

bool LayoutService::EnsureVisibleLayout(Document& doc, LayoutCache& cache, float width, float height)
{
    const float scroll_y = viewport_.GetScrollY();
    const bool updated = engine_.EnsureVisibleLayout(doc.GetNodesMut(), cache, width, scroll_y, scroll_y + height);

    viewport_.ApplyScrollTarget(cache);
    return updated;
}

void LayoutService::RecomputeAfterDiagram(Document& doc, LayoutCache& cache, const Theme& theme) noexcept
{
    const auto result = RecomputeYPositions(doc.GetNodesMut(), cache, theme);
    engine_.SetTotalHeight(result.total_height);
    viewport_.ApplyScrollTarget(cache);
}
