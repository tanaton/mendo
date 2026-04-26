#include "layout.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <ranges>

// マジックナンバーの名前付き定数
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
    default:
        return 0.0f;
    }
}

static float GetSpacingBelow(const Node& node, const Theme& theme) noexcept
{
    switch (node.type) {
    case NodeType::Heading:
        // h1/h2 は下線を描くため、下線と次行の余白を確保すべく大きめの値を返す。
        return (node.heading_level <= 2)
            ? theme.heading_spacing_below_h1h2
            : theme.heading_spacing_below;
    case NodeType::CodeBlock:
    case NodeType::Image:
        return theme.paragraph_spacing + theme.code_block_spacing_above;
    case NodeType::ListItem:
    case NodeType::TaskListItem:
        return theme.list_item_spacing;
    case NodeType::HorizontalRule:
        return 0.0f;
    default:
        return theme.paragraph_spacing;
    }
}

// ---- フリー関数 ----

void ComputeColumnWidths(std::pmr::vector<float>& out,
    const std::pmr::vector<float>& natural_widths,
    float available_width, size_t col_count)
{
    out.resize(col_count);
    available_width = std::max(available_width, static_cast<float>(col_count) * MIN_COLUMN_WIDTH);

    const float total_natural = std::ranges::fold_left(
        natural_widths,
        0.0f,
        [](float a, float b) static noexcept { return a + b; }
    );

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
    const auto row_count = rows.size();
    // 概算でreserveし、1パスで構築する（二重走査を回避）
    std::pmr::wstring text;
    text.reserve(row_count * 64); // 行あたり平均64文字の概算
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
        const int level = std::clamp(node.heading_level, 1, 6) - 1;
        return theme.font_size_h[level] * 1.5f;
    }
    case NodeType::CodeBlock: {
        const int lines = 1 + node.line_count;
        const float h = theme.font_size_code * 1.3f * static_cast<float>(lines);
        return std::max(h, line_height);
    }
    case NodeType::Table: {
        const size_t row_count = node.has_table()
            ? std::max(static_cast<size_t>(1), node.table_rows().size())
            : static_cast<size_t>(1);
        return line_height * static_cast<float>(row_count);
    }
    case NodeType::Image:
        return std::max(60.0f, theme.font_size_body * 3.0f);
    case NodeType::HorizontalRule:
        return theme.paragraph_spacing + theme.hr_thickness;
    default:
        // テキストの行数からおおよその高さを推定
        if (!node.HasText()) {
            return theme.paragraph_spacing;
        }
        const int lines = 1 + node.line_count;
        return line_height * static_cast<float>(lines);
    }
}

void EstimateNodeHeights(const std::pmr::vector<Node>& nodes, LayoutCache& cache, const Theme& theme) noexcept
{
    // ノードの種類に応じた既定の高さを割り当て、Y座標を累積計算する。
    // DirectWriteを一切呼ばないため、数千ノードでも数百マイクロ秒で完了する。
    // layout_dirtyフラグは変更しない（後続のViewportLayoutが正しく計測できるようにする）。
    assert(cache.size() >= nodes.size());
    const auto node_count = nodes.size();

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

        // 早期終了: safe_exit_after 以降でY位置が一致すれば、
        // 以降のノードのY位置は変わらない。
        if (i > safe_exit_after && std::abs(entry.y_position - y) < Y_POSITION_EPSILON) {
            // 残りのダーティノードを確認（Y更新より軽量なフラグチェックのみ）
            if (!result.has_dirty_nodes) {
                result.has_dirty_nodes = std::ranges::any_of(
                    std::views::iota(i, node_count),
                    [&cache](size_t j) { return cache[j].layout_dirty; }
                );
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

// ---- LayoutEngine クラス ----

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

void LayoutEngine::ComputeLayout(std::pmr::vector<Node>& nodes, LayoutCache& cache,
    float viewport_width,
    float viewport_top, float viewport_bottom)
{
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
        const float indent = node.indent_level * theme_->indent_width;
        const float node_width = content_width - indent;

        const bool needs_layout = width_changed || entry.layout_dirty;

        if (needs_layout) {
            if (partial) {
                // 部分モードでは、可視ノードのレイアウトのみ計算する
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
                    const bool is_diagram = (node.type == NodeType::CodeBlock
                        && IsDiagramLanguage(node.code_language));
                    if (!is_diagram) {
                        const float estimated = EstimateNodeHeight(node, *theme_);
                        if (entry.height != estimated) {
                            entry.height = estimated;
                            any_height_changed = true;
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

        // このパスで直接 Y 位置を設定する
        y += GetSpacingAbove(node, *theme_);
        entry.y_position = y;
        y += entry.height;
        y += GetSpacingBelow(node, *theme_);

        // 早期終了: 部分モードで幅の変更がなく、ビューポートを超えた後に
        // 高さの変更もなければ、残りの Y 位置は変わらない。
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
}

void LayoutEngine::LayoutNodes(std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width)
{
    last_viewport_width_ = 0.0f; // 幅の変更検出を強制する
    ComputeLayout(nodes, cache, viewport_width + theme_->margin_left + theme_->margin_right); // 逆変換: content→viewport
}

bool LayoutEngine::EnsureVisibleLayout(std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width, float viewport_top, float viewport_bottom)
{
    const float content_width = theme_->ContentWidth(viewport_width);
    bool any_updated = false;
    int last_measured = -1;

    // 下端が viewport_top 以上の最初のノードを見つける
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
        const float indent = nodes[i].indent_level * theme_->indent_width;
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

        // 時間予算チェック: MeasureNodeの前に判定し、超過分を抑える。
        // 最低1ノードは処理する（進行を保証）。
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
        const float indent = nodes[i].indent_level * theme_->indent_width;
        measurer_->MeasureNode(nodes[i], entry, content_width - indent);
        last_processed = i;

        if (++processed >= batch_size) {
            any_nearby_dirty_skipped = true;
            break;
        }
    }

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

    // すべてのダーティノードが処理されたら last_viewport_width_ を更新する
    if (!has_dirty_nodes_) {
        last_viewport_width_ = viewport_width;
    }
    return has_dirty_nodes_;
}
