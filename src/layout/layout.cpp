#include "layout.h"
#include "document.h"
#include "parallel_measure.h"
#include "profiler.h"
#include "task_scheduler.h"
#include "viewport_manager.h"
#include <algorithm>
#include <cmath>
#include <memory_resource>

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

void LayoutEngine::ComputeLayout(const std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width, float viewport_top, float viewport_bottom)
{
    MENDO_PROFILE("LayoutEngine::ComputeLayout");
    const auto node_count = nodes.size();
    cache.Resize(node_count);

    const bool width_changed = std::abs(viewport_width - last_viewport_width_) > kWidthChangeThreshold;
    if (width_changed) {
        last_viewport_width_ = viewport_width;
    }
    // 未推定のキャッシュ (位置が全て 0) では可視範囲が決まらないので、幅変更と同じく組み直す。
    const bool unpositioned = node_count > 0 && cache.Bottom(node_count - 1) <= 0.0f;
    if (width_changed || unpositioned) {
        EstimateDirtyHeights(nodes, cache, theme_->ContentWidth(viewport_width), width_changed);
    }

    // 計測で高さが変わると可視範囲に入るノードも変わるため、可視の dirty が無くなるまで繰り返す。
    // 計測に失敗し続けるノードがあっても止まるよう回数に上限を設ける。
    constexpr int kMaxMeasurePasses = 8;
    for (int pass = 0; pass < kMaxMeasurePasses; ++pass) {
        if (!EnsureVisibleLayout(nodes, cache, viewport_width, viewport_top, viewport_bottom)) {
            break;
        }
    }

    // 範囲外は走査しないので dirty が残っていると保守的に仮定する。
    const auto vr = ComputeVisibleNodeRange(cache, node_count, viewport_top, viewport_bottom);
    bool any_dirty = vr.first > 0 || vr.last_plus_1 < node_count;
    for (size_t i = vr.first; i < vr.last_plus_1 && !any_dirty; ++i) {
        any_dirty = cache[i].layout_dirty;
    }
    has_dirty_nodes_ = any_dirty;

    MENDO_PLOT("layout.compute.width_changed", static_cast<int64_t>(width_changed));
    MENDO_PLOT("layout.compute.node_count", static_cast<int64_t>(node_count));
}

void LayoutEngine::EstimateDirtyHeights(const std::pmr::vector<Node>& nodes, LayoutCache& cache, float content_width, bool mark_all_dirty)
{
    MENDO_PROFILE("LayoutEngine::EstimateDirtyHeights");
    float y = theme_->margin_top;
    for (size_t i = 0; i < nodes.size(); i++) {
        const auto& node = nodes[i];
        auto& entry = cache[i];
        if (mark_all_dirty) {
            entry.layout_dirty = true;
        }
        if (entry.layout_dirty) {
            EstimateInvisibleNodeHeight(node, entry, *theme_, content_width - NodeIndent(node, *theme_));
        }
        cache.SetTop(i, AdvanceNodeY(y, GetSpacingAbove(node, *theme_), entry.height, GetSpacingBelow(node, *theme_)));
    }
}

bool LayoutEngine::EnsureVisibleLayout(const std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width, float viewport_top, float viewport_bottom)
{
    MENDO_PROFILE("LayoutEngine::EnsureVisibleLayout");
    const float content_width = theme_->ContentWidth(viewport_width);
    bool any_updated = false;
    bool any_restored = false;

    // doc 差し替え直後などの過渡状態では nodes.size() > cache.size() になりうる
    const auto node_count = std::min(nodes.size(), cache.size());
    const auto lo = static_cast<size_t>(FindFirstVisibleNodeIndex(cache, node_count, viewport_top));
    // any_updated のときだけ意味を持つ。
    size_t last_measured = lo;

    const MeasureViewportRange vp{ viewport_top, viewport_bottom };
    std::pmr::vector<size_t> dirty_indices;
    for (size_t i = lo; i < node_count; i++) {
        auto& entry = cache[i];
        const float entry_top = cache.Top(i);
        if (entry_top > viewport_bottom) {
            break;
        }
        if (entry.layout_dirty) {
            dirty_indices.push_back(i);
            last_measured = i;
        }
        else if (const auto* tl = entry.table_layout(); tl && tl->HasEvictedRows()) {
            const float indent = NodeIndent(nodes[i], *theme_);
            cache.NoteMaterialized(static_cast<size_t>(i));
            const auto restored = measurer_->RestoreEvictedTableRows(nodes[i], entry, content_width - indent, vp.ToLocal(entry_top));
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
        mendo::layout::MeasureIndicesParallel(nodes, cache, content_width, *theme_, *measurer_, dirty_indices, vp, layout_scheduler_);
        any_updated = true;
    }

    if (any_updated) {
        has_dirty_nodes_ |= RecomputeYPositions(nodes, cache, *theme_, lo, last_measured);
    }
    return any_updated || any_restored;
}

bool LayoutEngine::ProcessDirtyBatch(
    const std::pmr::vector<Node>& nodes, LayoutCache& cache,
    float viewport_width, int batch_size,
    mendo::layout::ViewportClip clip)
{
    MENDO_PROFILE("LayoutEngine::ProcessDirtyBatch");
    const float content_width = theme_->ContentWidth(viewport_width);

    const auto result = mendo::layout::RunParallel(nodes, cache, content_width, *theme_, *measurer_, clip, batch_size, layout_scheduler_);

    // processed では判定しない: 例外で計測できなかったノードがあっても、計測できたノードは
    // 高さが変わっているので Y を組み直し、残った dirty は再試行に回す必要がある。
    if (result.reason == mendo::layout::StopReason::NoneDirty) {
        has_dirty_nodes_ = false;
        return false;
    }

    has_dirty_nodes_ = RecomputeYPositions(nodes, cache, *theme_, result.first_processed, result.last_processed);

    // ビューポート制限時: 付近のダーティノードが全て処理済みなら完了とみなす。
    // 遠方のダーティノードはスクロール時に EnsureVisibleLayout で処理される。
    if (clip.active() && has_dirty_nodes_ && !result.any_nearby_skipped()) {
        has_dirty_nodes_ = false;
    }
    // last_viewport_width_ は ComputeLayout が再計測を決めた幅のまま据え置く。ここで進めると
    // 2px 未満のリサイズとダーティ処理が交互に続いたとき、可視ノードが古い幅のまま取り残される。
    return has_dirty_nodes_;
}

void LayoutService::ViewportLayout(Document& doc, LayoutCache& cache, float width, float height)
{
    const float scroll_y = viewport_.GetScrollY();
    engine_.ComputeLayout(doc.GetNodes(), cache, width, scroll_y, scroll_y + height);
    viewport_.ApplyScrollTarget(cache);
}

bool LayoutService::ProcessDirtyBatch(Document& doc, LayoutCache& cache, float width, int batch_size, ViewportLimit viewport)
{
    // height <= 0 なら ViewportClip::active() が false になりクリップは無効。
    const bool more = engine_.ProcessDirtyBatch(doc.GetNodes(), cache, width, batch_size, { viewport_.GetScrollY(), viewport.height, viewport.buffer_screens });
    viewport_.ApplyScrollTarget(cache);
    return more;
}

bool LayoutService::EnsureVisibleLayout(Document& doc, LayoutCache& cache, float width, float height)
{
    const float scroll_y = viewport_.GetScrollY();
    const bool updated = engine_.EnsureVisibleLayout(doc.GetNodes(), cache, width, scroll_y, scroll_y + height);
    viewport_.ApplyScrollTarget(cache);
    return updated;
}

void LayoutService::RecomputeAfterDiagram(Document& doc, LayoutCache& cache, const Theme& theme,
                                          mendo::layout::HeightChangeRange changed) noexcept
{
    if (!changed.empty()) {
        RecomputeYPositions(doc.GetNodes(), cache, theme, changed.first, changed.last);
    }
    viewport_.ApplyScrollTarget(cache);
}
