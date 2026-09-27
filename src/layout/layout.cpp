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

    // 幅が不変なら可視範囲より上のノードは計測も推定も変わらないため、先頭からではなく
    // 可視先頭から始める (ウィンドウ移動やスクロールバードラッグ終了でも全件走査していた)。
    size_t start = 0;
    if (!width_changed && partial) {
        start = static_cast<size_t>(FindFirstVisibleNodeIndex(cache, node_count, vp_top));
        if (start > 0 && start < node_count) {
            y = cache.Bottom(start - 1) + GetSpacingBelow(nodes[start - 1], *theme_);
            // 上側の dirty は走査しないので保守的に仮定する。
            any_dirty = true;
        }
    }

    for (size_t i = start; i < node_count; i++) {
        auto& node = nodes[i];
        auto& entry = cache[i];
        const float indent = NodeIndent(node, *theme_);
        const float node_width = content_width - indent;

        const float sa = GetSpacingAbove(node, *theme_);
        const float sb = GetSpacingBelow(node, *theme_);

        if (width_changed || entry.layout_dirty) {
            const float node_bottom = y + entry.height; // 古い高さを使って推定
            const bool visible = (node_bottom >= vp_top && y <= vp_bottom);
            if (visible) {
                const float old_height = entry.height;
                // 部分レイアウトでは可視範囲を渡してテーブル内行を絞り込む。
                // partial=false (フルレイアウト) では vp_top/bottom が ±inf なので全範囲扱い。
                const MeasureViewportRange vp{ vp_top, vp_bottom };
                MeasureEntry(*backend_, node, entry, node_width, nullptr, vp, y + sa);
                any_measured = true;
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

        cache.SetTop(i, AdvanceNodeY(y, sa, entry.height, sb));

        // 幅の変更がなければビューポートより下の高さは変わらないので早期終了する。
        // 可視範囲で高さが変わった場合も、残りは一定量のシフトで済む。
        if (!width_changed && y > vp_bottom) {
            if (any_height_changed) {
                RecomputeYPositions(nodes, cache, *theme_, i + 1, false, i);
            }
            // 中断地点より先にダーティノードが存在する可能性を保守的に仮定する。
            // ProcessDirtyBatch が存在しない場合は速やかに確認・クリアする。
            any_dirty = true;
            broke_early = true;
            break;
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

    // doc 差し替え直後などの過渡状態では nodes.size() > cache.size() になりうる
    const auto node_count = std::min(nodes.size(), cache.size());
    const int lo = FindFirstVisibleNodeIndex(cache, node_count, viewport_top);

    bool any_restored = false;
    const MeasureViewportRange vp{ viewport_top, viewport_bottom };
    std::pmr::vector<size_t> dirty_indices;
    for (int i = lo; i < static_cast<int>(node_count); i++) {
        auto& entry = cache[i];
        const float entry_top = cache.Top(i);
        if (entry_top > viewport_bottom) {
            break;
        }
        if (entry.layout_dirty) {
            dirty_indices.push_back(static_cast<size_t>(i));
            last_measured = i;
        }
        else if (entry.has_table_layout() && entry.table_layout->HasEvictedRows()) {
            const float indent = NodeIndent(nodes[i], *theme_);
            const auto restored = backend_->RestoreEvictedTableRows(nodes[i], entry, content_width - indent, vp.ToLocal(entry_top));
            any_restored |= restored.restored;
            if (restored.height_changed) {
                any_updated = true;
                last_measured = i;
            }
        }
    }

    if (!dirty_indices.empty()) {
        // 未計測領域へのジャンプやスクロールバードラッグでは 1 画面分 (数十〜百ノード) を
        // 毎フレーム計測するため、scheduler があれば並列化する。
        constexpr size_t kMinVisibleForParallel = 8;
        mendo::layout::MeasureIndicesParallel(nodes, cache, content_width, *theme_, *backend_, dirty_indices, vp, layout_scheduler_, kMinVisibleForParallel);
        any_updated = true;
    }

    if (any_updated || any_restored) {
        cache.IncrementEffectsGeneration();
    }
    if (any_updated) {
        has_dirty_nodes_ = RecomputeYPositions(nodes, cache, *theme_, static_cast<size_t>(lo), has_dirty_nodes_, static_cast<size_t>(last_measured)).has_dirty_nodes;
    }
    return any_updated || any_restored;
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
    has_dirty_nodes_ = RecomputeYPositions(nodes, cache, *theme_, result.first_processed, false, result.last_processed).has_dirty_nodes;

    // ビューポート制限時: 付近のダーティノードが全て処理済みなら完了とみなす。
    // 遠方のダーティノードはスクロール時に EnsureVisibleLayout で処理される。
    const bool has_viewport_limit = (viewport_top >= 0.0f && viewport_height > 0.0f);
    if (has_viewport_limit && has_dirty_nodes_ && !result.any_nearby_skipped()) {
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

bool LayoutService::ProcessDirtyBatch(Document& doc, LayoutCache& cache, float width, int batch_size, int time_budget_us, ViewportLimit viewport)
{
    bool more;
    if (viewport.height > 0.0f) {
        const float vp_top = viewport_.GetScrollY();
        more = engine_.ProcessDirtyBatch(doc.GetNodesMut(), cache, width, batch_size, time_budget_us, vp_top, viewport.height, viewport.buffer_screens);
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

void LayoutService::RecomputeAfterDiagram(Document& doc, LayoutCache& cache, const Theme& theme,
                                          mendo::layout::HeightChangeRange changed) noexcept
{
    if (!changed.empty()) {
        RecomputeYPositions(doc.GetNodesMut(), cache, theme, changed.first, false, changed.last);
    }
    viewport_.ApplyScrollTarget(cache);
}

float LayoutService::GetScrollableContentHeight(const Document& doc, const LayoutCache& cache) const noexcept
{
    return ComputeTotalContentHeight(cache, doc.GetNodes().size(), engine_.GetMarginTop());
}
