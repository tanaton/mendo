#include "layout.h"
#include "document.h"
#include "memory_resource.h"
#include "parallel_measure.h"
#include "profiler.h"
#include "task_scheduler.h"
#include <algorithm>
#include <cmath>
#include <limits>
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

    // partial=false 時は ±∞ で full レイアウトを再現する
    // (visible が常に true、不可視推定経路と early break が発火しない)。
    constexpr float kInf = std::numeric_limits<float>::infinity();
    const float vp_top = partial ? viewport_top : -kInf;
    const float vp_bottom = partial ? viewport_bottom : kInf;
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

        if (width_changed || entry.layout_dirty) {
            const float node_bottom = y + entry.height; // 古い高さを使って推定
            const bool visible = (node_bottom >= vp_top && y <= vp_bottom);
            if (visible) {
                const float old_height = entry.height;
                // 部分レイアウトでは可視範囲を渡してテーブル内行を絞り込む。
                // partial=false (フルレイアウト) では vp_top/bottom が ±inf なので全範囲扱い。
                const MeasureViewportRange vp{ vp_top, vp_bottom };
                backend_->MeasureNode(node, entry, node_width, nullptr, vp);
                any_measured = true;
                entry.cached_width = node_width;
                entry.cached_height = entry.height;
                if (entry.height != old_height) {
                    any_height_changed = true;
                }
            }
            else {
                // 不可視ノードは保守的な推定値だけ更新し、厳密値は後続の
                // ProcessDirtyBatch / EnsureVisibleLayout に委ねる。
                if (EstimateInvisibleNodeHeight(node, entry, *theme_, node_width)) {
                    any_height_changed = true;
                }
                entry.layout_dirty = true;
            }
        }

        if (entry.layout_dirty) {
            any_dirty = true;
        }

        const float sa = GetSpacingAbove(node, *theme_);
        const float sb = GetSpacingBelow(node, *theme_);

        y += sa;
        entry.text_top = y;
        y += entry.height;
        y += sb;

        block_heights.push_back(sa + entry.height + sb);

        // 幅の変更がなく、ビューポートを超えた後に高さの変更もなければ、
        // 残りの Y 位置は変わらないので早期終了する。
        if (!width_changed && !any_height_changed && y > vp_bottom) {
            // 中断地点より先にダーティノードが存在する可能性を保守的に仮定する。
            // ProcessDirtyBatch が存在しない場合は速やかに確認・クリアする。
            any_dirty = true;
            broke_early = true;
            break;
        }
    }

    ApplyComputeLayoutBlockHeights(cache, block_heights, broke_early, y);
    has_dirty_nodes_ = any_dirty;
    if (any_measured) {
        cache.IncrementEffectsGeneration();
    }

    MENDO_PLOT("layout.compute.partial", static_cast<int64_t>(partial));
    MENDO_PLOT("layout.compute.width_changed", static_cast<int64_t>(width_changed));
    MENDO_PLOT("layout.compute.node_count", static_cast<int64_t>(node_count));
    MENDO_PLOT("layout.compute.broke_early", static_cast<int64_t>(broke_early));
}

void LayoutEngine::ApplyComputeLayoutBlockHeights(LayoutCache& cache, const std::pmr::vector<float>& block_heights,
                                                  bool broke_early, float final_y) noexcept
{
    if (!broke_early) {
        cache.BuildBlockHeights(block_heights);
        total_height_ = final_y + theme_->margin_top;
    }
    else {
        for (size_t i = 0; i < block_heights.size(); ++i) {
            cache.SetBlockHeight(i, block_heights[i]);
        }
    }
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
        if (entry.text_top > viewport_bottom) {
            break;
        }
        if (!entry.layout_dirty) {
            continue;
        }
        const float indent = NodeIndent(nodes[i], *theme_);
        const MeasureViewportRange vp{ viewport_top, viewport_bottom };
        backend_->MeasureNode(nodes[i], entry, content_width - indent, nullptr, vp);
        any_updated = true;
        last_measured = i;
    }

    if (any_updated) {
        cache.IncrementEffectsGeneration();
        const auto result = RecomputeYPositions(nodes, cache, *theme_, static_cast<size_t>(lo), has_dirty_nodes_, static_cast<size_t>(last_measured));
        total_height_ = result.total_height;
        has_dirty_nodes_ = result.has_dirty_nodes;
    }
    return any_updated;
}

bool LayoutEngine::ProcessDirtyBatch(
    std::pmr::vector<Node>& nodes, LayoutCache& cache,
    float viewport_width, int batch_size, int time_budget_us,
    float viewport_top, float viewport_height, float buffer_screens)
{
    MENDO_PROFILE("LayoutEngine::ProcessDirtyBatch");
    const float content_width = theme_->ContentWidth(viewport_width);

    const mendo::layout::ViewportClip clip{ viewport_top, viewport_height, buffer_screens };

    // 並列版は ParallelBudget (max_nodes のみ) を取り、time_budget は型レベルで遮断される。
    // batch_size は Phase 1 で適用するのでスクロール時バッチも上限以下に収まる。
    // 小規模 dirty は RunParallel 内部で inline 直列に倒れる。
    const auto result =
        layout_scheduler_
            ? mendo::layout::RunParallel(nodes, cache, content_width, *theme_, *backend_, clip, mendo::layout::ParallelBudget{ batch_size }, *layout_scheduler_)
            : scheduler_.RunSerial(nodes, cache, content_width, *theme_, *backend_, clip, mendo::layout::SerialBudget{ batch_size, time_budget_us });

    if (result.processed == 0) {
        has_dirty_nodes_ = false;
        return false;
    }

    cache.IncrementEffectsGeneration();
    const auto y_result = RecomputeYPositions(nodes, cache, *theme_, result.first_processed, false, result.last_processed);
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
