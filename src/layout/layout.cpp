#include "layout.h"
#include "document.h"
#include "memory_resource.h"
#include "profiler.h"
#include <algorithm>
#include <cmath>
#include <memory_resource>

bool LayoutEngine::Init(ITextMeasurer* measurer, const Theme& theme)
{
    // ITextMeasurer は IMeasureBackend と IMeasureLifecycle を多重継承する合成 IF。
    // 同一インスタンスを 2 つの view として保持し、hot path は backend_ (const) を使う。
    lifecycle_ = measurer;
    backend_ = measurer;
    theme_ = &theme;
    return lifecycle_->Init(theme);
}

bool LayoutEngine::RecreateFormats()
{
    if (!lifecycle_) {
        return false;
    }
    last_viewport_width_ = 0.0f;
    return lifecycle_->RecreateFormats();
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

    StackArena<4096> arena;
    std::pmr::vector<float> block_heights(arena.resource());
    block_heights.reserve(node_count);

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
                    backend_->MeasureNode(node, entry, node_width);
                    any_measured = true;
                    entry.cached_width = node_width;
                    entry.cached_height = entry.height;
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
                        // 同じ幅での実測キャッシュがあればそれを使い、無ければ推定値で成長させる。
                        constexpr float kCachedWidthEpsilon = 0.5f;
                        const bool cache_hit = entry.cached_width > 0.0f &&
                                               std::abs(entry.cached_width - node_width) < kCachedWidthEpsilon &&
                                               entry.cached_height > 0.0f;
                        const float fallback = cache_hit ? entry.cached_height : EstimateNodeHeight(node, *theme_);
                        if (entry.height < fallback) {
                            entry.height = fallback;
                            any_height_changed = true;
                            // 推定で成長させた場合、テーブル幾何 (row_heights/col_widths) は
                            // 旧値のままなので clear して MeasureNode の lazy 復元経路を通させる。
                            if (node.type == NodeType::Table && entry.has_table_layout() && !cache_hit) {
                                entry.table_layout->col_widths.clear();
                                entry.table_layout->cached_table_width = 0.0f;
                            }
                        }
                    }
                    entry.layout_dirty = true;
                }
            }
            else {
                backend_->MeasureNode(node, entry, node_width);
                any_measured = true;
                entry.cached_width = node_width;
                entry.cached_height = entry.height;
            }
        }

        if (entry.layout_dirty) {
            any_dirty = true;
        }

        const float sa = GetSpacingAbove(node, *theme_);
        const float sb = GetSpacingBelow(node, *theme_);

        y += sa;
        entry.y_position = y;
        y += entry.height;
        y += sb;

        block_heights.push_back(sa + entry.height + sb);

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

    // フルパス完走時は Fenwick バルクロード (O(N))、途中 break 時は部分のみ個別 Set。
    if (!broke_early) {
        cache.BuildBlockHeights(block_heights);
        total_height_ = y + theme_->margin_top;
    }
    else {
        for (size_t i = 0; i < block_heights.size(); ++i) {
            cache.SetBlockHeight(i, block_heights[i]);
        }
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
        backend_->MeasureNode(nodes[i], entry, content_width - indent);
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

    const auto result = scheduler_.RunSerial(
        nodes, cache, content_width, *theme_, *backend_,
        mendo::layout::ViewportClip{ viewport_top, viewport_height, buffer_screens },
        mendo::layout::DirtyBudget{ batch_size, time_budget_us });

    if (result.processed == 0) {
        has_dirty_nodes_ = false;
        return false;
    }

    cache.IncrementEffectsGeneration();
    const auto y_result = RecomputeYPositions(nodes, cache, *theme_,
                                              result.first_processed, false, result.last_processed);
    total_height_ = y_result.total_height;
    has_dirty_nodes_ = y_result.has_dirty_nodes;

    // ビューポート制限時: 付近のダーティノードが全て処理済みなら完了とみなす。
    // 遠方のダーティノードはスクロール時に EnsureVisibleLayout で処理される。
    const bool has_viewport_limit = (viewport_top >= 0.0f && viewport_height > 0.0f);
    if (has_viewport_limit && has_dirty_nodes_ && !result.any_nearby_skipped) {
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
