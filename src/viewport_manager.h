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

    float GetScrollY() const noexcept { return scroll_y_; }
    float GetScrollTarget() const noexcept { return scroll_target_; }
    float GetMaxScroll() const noexcept { return max_scroll_; }
    bool IsSmoothScrolling() const noexcept { return smooth_scrolling_; }

    void ScrollTo(float position) noexcept {
        scroll_y_ = std::clamp(position, 0.0f, max_scroll_);
        scroll_target_ = scroll_y_;
    }

    void SmoothScrollBy(float delta) noexcept {
        scroll_target_ = std::clamp(scroll_target_ + delta, 0.0f, max_scroll_);
        smooth_scrolling_ = true;
    }

    // スムーススクロール補間を1フレーム進める。
    // スクロールがまだ継続中の場合trueを返す（呼び出し側はタイマーを維持すべき）。
    bool UpdateSmoothScroll() noexcept {
        float diff = scroll_target_ - scroll_y_;
        if (std::abs(diff) < SCROLL_EPSILON) {
            scroll_y_ = scroll_target_;
            smooth_scrolling_ = false;
            return false;
        }
        scroll_y_ += diff * SCROLL_SPEED;
        scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll_);
        return true;
    }

    void StopSmoothScroll() noexcept {
        if (!smooth_scrolling_) return;
        scroll_y_ = scroll_target_;
        smooth_scrolling_ = false;
    }

    void SyncMaxScroll(float total_height, float viewport_height) noexcept {
        max_scroll_ = std::max(0.0f, total_height - viewport_height);
        scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll_);
        scroll_target_ = scroll_y_;
    }

    // 下端がscroll_y_より下にある最初のノードを見つける。
    // 表示可能なノードが存在しない場合は-1を返す。
    int FindFirstVisibleNode(const LayoutCache& cache, size_t node_count) const noexcept {
        int idx = FindFirstVisibleNodeIndex(cache, node_count, scroll_y_);
        return idx < static_cast<int>(node_count) ? idx : -1;
    }

    void AnchorCompensateScroll(int anchor_idx, float anchor_y_before, const LayoutCache& cache) noexcept {
        if (anchor_idx < 0) return;
        float shift = cache[anchor_idx].y_position - anchor_y_before;
        scroll_y_ = std::max(0.0f, scroll_y_ + shift);
        scroll_target_ = std::max(0.0f, scroll_target_ + shift);
        // 注意: 呼び出し側はこの後SyncMaxScroll()を呼ぶ必要がある
    }

    void SetScrollY(float y) noexcept { scroll_y_ = y; }
    void SetScrollTarget(float t) noexcept { scroll_target_ = t; }

    bool IsScrollbarTracking() const noexcept { return is_scrollbar_tracking_; }
    void SetScrollbarTracking(bool v) noexcept { is_scrollbar_tracking_ = v; }

    // ---- 選択 ----

    const TextSelection& GetSelection() const noexcept { return selection_; }
    TextSelection& GetSelectionMut() noexcept { return selection_; }
    void SetSelection(const TextSelection& sel) noexcept { selection_ = sel; }

    int GetAnchorNode() const noexcept { return anchor_node_; }
    uint32_t GetAnchorPos() const noexcept { return anchor_pos_; }
    void SetAnchor(int node, uint32_t pos) noexcept { anchor_node_ = node; anchor_pos_ = pos; }

    bool IsDragging() const noexcept { return is_dragging_; }
    void SetDragging(bool v) noexcept { is_dragging_ = v; }

    int GetClickStartX() const noexcept { return click_start_x_; }
    int GetClickStartY() const noexcept { return click_start_y_; }
    void SetClickStart(int x, int y) noexcept { click_start_x_ = x; click_start_y_ = y; }

    void ClearSelection() noexcept {
        selection_.Clear();
        anchor_node_ = -1;
        is_dragging_ = false;
    }

    void SelectAll(const std::pmr::vector<Node>& nodes) noexcept {
        if (nodes.empty()) {
            ClearSelection();
            return;
        }
        int last = static_cast<int>(nodes.size()) - 1;
        selection_ = TextSelection::MakeOrdered(
            0, 0, last, static_cast<uint32_t>(nodes[last].text.size()));
    }

    // ---- ズーム ----

    int GetZoomIndex() const noexcept { return zoom_index_; }
    void SetZoomIndex(int idx) noexcept { zoom_index_ = idx; }
    float GetCurrentZoom() const noexcept { return ZOOM_STEPS[zoom_index_]; }

    // 新しいズーム値を返す。既に上限/下限の場合は0を返す。
    float ZoomIn() noexcept {
        if (zoom_index_ < ZOOM_STEP_COUNT - 1) return ZOOM_STEPS[++zoom_index_];
        return 0.0f;
    }

    float ZoomOut() noexcept {
        if (zoom_index_ > 0) return ZOOM_STEPS[--zoom_index_];
        return 0.0f;
    }

    float ZoomReset() noexcept {
        if (zoom_index_ != ZOOM_DEFAULT_INDEX) {
            zoom_index_ = ZOOM_DEFAULT_INDEX;
            return ZOOM_STEPS[zoom_index_];
        }
        return 0.0f;
    }

    static constexpr float SCROLL_SPEED = 0.25f;
    static constexpr float SCROLL_EPSILON = 1.5f;

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
