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

void LayoutEngine::ComputeLayout(std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width, float viewport_top, float viewport_bottom)
{
    MENDO_PROFILE("LayoutEngine::ComputeLayout");
    const auto node_count = nodes.size();
    cache.Resize(node_count);

    const bool width_changed = std::abs(viewport_width - last_viewport_width_) > kWidthChangeThreshold;
    const bool partial = (viewport_top >= 0.0f);

    if (width_changed) {
        last_viewport_width_ = viewport_width;
    }

    // partial=false 時は ±∞ (既定値) で full レイアウトを再現する
    // (visible が常に true、不可視推定経路と early break が発火しない)。
    // 部分レイアウトでは計測にも可視範囲を渡してテーブル内行を絞り込む。
    const MeasureViewportRange vp = partial ? MeasureViewportRange{ viewport_top, viewport_bottom } : MeasureViewportRange{};
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
        start = static_cast<size_t>(FindFirstVisibleNodeIndex(cache, node_count, vp.top));
        // 未推定のキャッシュ (位置が全て 0) では可視ノードが見つからず、省くと何も計測されない。
        // 表示経路は MakeEstimatedLayoutCache 済みだが、空白表示を防ぐ安全網として先頭から組み直す。
        // 位置確定済みで末尾より下を見ているだけなら全件走査を避けるため従来どおり省く。
        if (start >= node_count && node_count > 0 && cache.Bottom(node_count - 1) <= 0.0f) {
            start = 0;
        }
        else if (start > 0 && start < node_count) {
            y = mendo::layout::NodeStartY(nodes, cache, *theme_, start);
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
            // 可視判定は古い高さを使った推定。
            if (!IsOffscreen(y, entry.height, vp.top, vp.bottom)) {
                const float old_height = entry.height;
                cache.NoteMaterialized(i);
                MeasureEntry(*measurer_, node, entry, node_width, nullptr, vp, y + sa);
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
        if (!width_changed && y > vp.bottom) {
            if (any_height_changed) {
                RecomputeYPositions(nodes, cache, *theme_, i + 1, i);
            }
            // 中断地点より先にダーティノードが存在する可能性を保守的に仮定する。
            // 実際に存在しなければ ProcessDirtyBatch が速やかに確認・クリアする。
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
        else if (entry.has_table_layout() && entry.table_layout->HasEvictedRows()) {
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
        constexpr size_t kMinVisibleForParallel = 8;
        mendo::layout::MeasureIndicesParallel(nodes, cache, content_width, *theme_, *measurer_, dirty_indices, vp, layout_scheduler_, kMinVisibleForParallel);
        any_updated = true;
    }

    if (any_updated || any_restored) {
        cache.IncrementEffectsGeneration();
    }
    if (any_updated) {
        has_dirty_nodes_ |= RecomputeYPositions(nodes, cache, *theme_, lo, last_measured);
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
            ? mendo::layout::RunParallel(nodes, cache, content_width, *theme_, *measurer_, clip, mendo::layout::ParallelBudget{ batch_size }, *layout_scheduler_)
            : mendo::layout::RunSerial(nodes, cache, content_width, *theme_, *measurer_, clip, mendo::layout::SerialBudget{ batch_size, time_budget_us });

    // processed では判定しない: 例外で計測できなかったノードがあっても、計測できたノードは
    // 高さが変わっているので Y を組み直し、残った dirty は再試行に回す必要がある。
    if (result.reason == mendo::layout::StopReason::NoneDirty) {
        has_dirty_nodes_ = false;
        return false;
    }

    cache.IncrementEffectsGeneration();
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
    engine_.ComputeLayout(doc.GetNodesMut(), cache, width, scroll_y, scroll_y + height);
    viewport_.ApplyScrollTarget(cache);
}

bool LayoutService::ProcessDirtyBatch(Document& doc, LayoutCache& cache, float width, int batch_size, int time_budget_us, ViewportLimit viewport)
{
    // height <= 0 なら ViewportClip::active() が false になりクリップは無効。
    const bool more = engine_.ProcessDirtyBatch(doc.GetNodesMut(), cache, width, batch_size, time_budget_us, viewport_.GetScrollY(), viewport.height, viewport.buffer_screens);
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
        RecomputeYPositions(doc.GetNodes(), cache, theme, changed.first, changed.last);
    }
    viewport_.ApplyScrollTarget(cache);
}
