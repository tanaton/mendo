#pragma once
#include "types.h"
#include "layout_cache.h"
#include "theme.h"
#include <algorithm>
#include <cmath>
#include <memory_resource>

// スクロール、選択、ズームの純粋な状態管理。
// Win32 API依存なし — 完全にテスト可能。
class ViewportManager {
public:
    // ---- スクロール ----

    constexpr float GetScrollY() const noexcept { return scroll_y_; }
    constexpr float GetScrollTarget() const noexcept { return scroll_target_; }
    constexpr float GetMaxScroll() const noexcept { return max_scroll_; }
    constexpr bool IsSmoothScrolling() const noexcept { return smooth_scrolling_; }

    constexpr void ScrollTo(float position) noexcept
    {
        scroll_y_ = std::clamp(position, 0.0f, max_scroll_);
        scroll_target_ = scroll_y_;
    }

    constexpr void SmoothScrollBy(float delta) noexcept
    {
        scroll_target_ = std::clamp(scroll_target_ + delta, 0.0f, max_scroll_);
        smooth_scrolling_ = true;
    }

    // スムーススクロール補間を1フレーム進める。
    // スクロールがまだ継続中の場合trueを返す。
    // dt_ms: 前フレームからの経過時間（ミリ秒）。フレームレート非依存の補間を行う。
    bool UpdateSmoothScroll(float dt_ms) noexcept
    {
        // 極端に大きなデルタタイムを防止（ウィンドウ最小化等）
        dt_ms = std::min(dt_ms, MAX_DELTA_MS);
        float diff = scroll_target_ - scroll_y_;
        if (std::abs(diff) < SCROLL_EPSILON) {
            scroll_y_ = scroll_target_;
            smooth_scrolling_ = false;
            return false;
        }
        float factor = 1.0f - std::pow(1.0f - SCROLL_SPEED, dt_ms / SCROLL_REFERENCE_DT);
        float movement = diff * factor;
        // タッチパッドで勢いよくスワイプ後に指を離した際のジャンプを防止。
        // 蓄積されたターゲットとの大きなギャップから一気に追いつくのを制限する。
        float max_movement = MAX_SCROLL_SPEED * dt_ms;
        if (std::abs(movement) > max_movement) {
            movement = std::copysign(max_movement, movement);
        }
        scroll_y_ += movement;
        scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll_);
        return true;
    }

    // 基準フレーム時間（16ms ≈ 60fps）でのオーバーロード。
    bool UpdateSmoothScroll() noexcept
    {
        return UpdateSmoothScroll(SCROLL_REFERENCE_DT);
    }

    constexpr void StopSmoothScroll() noexcept
    {
        if (!smooth_scrolling_) {
            return;
        }
        scroll_y_ = scroll_target_;
        smooth_scrolling_ = false;
    }

    constexpr void SyncMaxScroll(float total_height, float viewport_height) noexcept
    {
        max_scroll_ = std::max(0.0f, total_height - viewport_height);
        scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll_);
        scroll_target_ = std::clamp(scroll_target_, 0.0f, max_scroll_);
    }

    // 下端がscroll_y_より下にある最初のノードを見つける。
    // 表示可能なノードが存在しない場合は-1を返す。
    constexpr int FindFirstVisibleNode(const LayoutCache& cache, size_t node_count) const noexcept
    {
        int idx = FindFirstVisibleNodeIndex(cache, node_count, scroll_y_);
        return idx < static_cast<int>(node_count) ? idx : -1;
    }

    constexpr void AnchorCompensateScroll(int anchor_idx, float anchor_y_before, const LayoutCache& cache) noexcept
    {
        if (anchor_idx < 0) {
            return;
        }
        float shift = cache[anchor_idx].y_position - anchor_y_before;
        scroll_y_ = std::max(0.0f, scroll_y_ + shift);
        scroll_target_ = std::max(0.0f, scroll_target_ + shift);
        // 注意: 呼び出し側はこの後SyncMaxScroll()を呼ぶ必要がある
    }

    constexpr void SetScrollY(float y) noexcept { scroll_y_ = y; }
    constexpr void SetScrollTarget(float t) noexcept { scroll_target_ = t; }

    constexpr bool IsScrollbarTracking() const noexcept { return is_scrollbar_tracking_; }
    constexpr void SetScrollbarTracking(bool v) noexcept { is_scrollbar_tracking_ = v; }

    // ---- 選択 ----

    constexpr const TextSelection& GetSelection() const noexcept { return selection_; }
    constexpr TextSelection& GetSelectionMut() noexcept { return selection_; }
    constexpr void SetSelection(const TextSelection& sel) noexcept { selection_ = sel; }

    constexpr int GetAnchorNode() const noexcept { return anchor_node_; }
    constexpr uint32_t GetAnchorPos() const noexcept { return anchor_pos_; }
    constexpr void SetAnchor(int node, uint32_t pos) noexcept { anchor_node_ = node; anchor_pos_ = pos; }

    constexpr bool IsDragging() const noexcept { return is_dragging_; }
    constexpr void SetDragging(bool v) noexcept { is_dragging_ = v; }

    constexpr int GetClickStartX() const noexcept { return click_start_x_; }
    constexpr int GetClickStartY() const noexcept { return click_start_y_; }
    constexpr void SetClickStart(int x, int y) noexcept { click_start_x_ = x; click_start_y_ = y; }

    constexpr void ClearSelection() noexcept
    {
        selection_.Clear();
        anchor_node_ = -1;
        is_dragging_ = false;
    }

    constexpr void SelectAll(const std::pmr::vector<Node>& nodes) noexcept
    {
        if (nodes.empty()) {
            ClearSelection();
            return;
        }
        int last = static_cast<int>(nodes.size()) - 1;
        selection_ = TextSelection::MakeOrdered(
            0, 0, last, static_cast<uint32_t>(nodes[last].text.size()));
    }

    // ---- ズーム ----

    constexpr int GetZoomIndex() const noexcept { return zoom_index_; }
    constexpr void SetZoomIndex(int idx) noexcept { zoom_index_ = idx; }
    constexpr float GetCurrentZoom() const noexcept { return ZOOM_STEPS[zoom_index_]; }

    // 新しいズーム値を返す。既に上限/下限の場合は0を返す。
    constexpr float ZoomIn() noexcept
    {
        if (zoom_index_ < ZOOM_STEP_COUNT - 1) {
            return ZOOM_STEPS[++zoom_index_];
        }
        return 0.0f;
    }

    constexpr float ZoomOut() noexcept
    {
        if (zoom_index_ > 0) {
            return ZOOM_STEPS[--zoom_index_];
        }
        return 0.0f;
    }

    constexpr float ZoomReset() noexcept
    {
        if (zoom_index_ != ZOOM_DEFAULT_INDEX) {
            zoom_index_ = ZOOM_DEFAULT_INDEX;
            return ZOOM_STEPS[zoom_index_];
        }
        return 0.0f;
    }

    static constexpr float SCROLL_SPEED = 0.25f;
    static constexpr float SCROLL_EPSILON = 1.5f;
    static constexpr float SCROLL_REFERENCE_DT = 16.0f; // 基準フレーム時間（ms）
    static constexpr float MAX_DELTA_MS = 100.0f;       // デルタタイム上限（ms）
    static constexpr float MAX_SCROLL_SPEED = 10.0f;    // スクロール速度上限（px/ms）

private:
    // スクロール状態
    float scroll_y_ = 0.0f;
    float scroll_target_ = 0.0f;
    float max_scroll_ = 0.0f;
    bool smooth_scrolling_ = false;
    bool is_scrollbar_tracking_ = false;

    // 選択状態
    TextSelection selection_;
    int anchor_node_ = -1;
    uint32_t anchor_pos_ = 0;
    bool is_dragging_ = false;
    int click_start_x_ = 0;
    int click_start_y_ = 0;

    // ズーム状態
    int zoom_index_ = ZOOM_DEFAULT_INDEX;
};
