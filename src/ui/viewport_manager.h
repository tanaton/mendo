#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "theme.h"
#include <algorithm>
#include <memory_resource>

// スクロール位置の「目的地」を表す値型。
// node が有効な間、レイアウトが変化するたびに scroll_y = y_position[node] + offset として再評価される。
// ユーザー発のピクセルスクロールや ClearScrollTarget で無効化される。
struct ScrollTarget {
    int node = -1;          // -1 = 無効
    float offset = 0.0f;    // ノード先頭からのピクセル距離

    constexpr bool IsValid() const noexcept { return node >= 0; }
};

// 見出しノードを md ペイン上端の heading_spacing_above 分だけ下に配置する ScrollTarget を作る。
// TOCクリック・#anchor ナビゲーションで共通して使う。
constexpr ScrollTarget MakeHeadingTopTarget(int node, float heading_spacing_above, float md_pane_top) noexcept
{
    return { node, -(heading_spacing_above + md_pane_top) };
}

// スクロール、選択、ズームの純粋な状態管理。
// Win32 API依存なし — 完全にテスト可能。
class ViewportManager {
public:
    // ---- スクロール ----

    constexpr float GetScrollY() const noexcept { return scroll_y_; }
    constexpr float GetMaxScroll() const noexcept { return max_scroll_; }

    // ピクセル絶対スクロール。scroll_target を無効化する。
    constexpr void ScrollTo(float position) noexcept
    {
        scroll_y_ = std::clamp(position, 0.0f, max_scroll_);
        scroll_target_ = {};
    }

    // ユーザー発のピクセルスクロール。scroll_target を無効化する。
    constexpr void DirectScrollBy(float delta) noexcept
    {
        scroll_y_ = std::clamp(scroll_y_ + delta, 0.0f, max_scroll_);
        scroll_target_ = {};
    }

    constexpr void SyncMaxScroll(float total_height, float viewport_height) noexcept
    {
        max_scroll_ = std::max(0.0f, total_height - viewport_height);
        scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll_);
    }

    // 下端がscroll_y_より下にある最初のノードを見つける。
    // 表示可能なノードが存在しない場合は-1を返す。
    constexpr int FindFirstVisibleNode(const LayoutCache& cache, size_t node_count) const noexcept
    {
        const int idx = FindFirstVisibleNodeIndex(cache, node_count, scroll_y_);
        return idx < static_cast<int>(node_count) ? idx : -1;
    }

    // scroll_target を明示設定する。内部の scroll_y は更新しないので、
    // 呼び出し側は直後に ApplyScrollTarget を呼ぶか、レイアウト操作経由のフック
    // で再評価されるのを待つ必要がある。
    constexpr void SetScrollTarget(int node, float offset) noexcept
    {
        scroll_target_ = { node, offset };
    }

    constexpr void ClearScrollTarget() noexcept { scroll_target_ = {}; }
    constexpr bool HasScrollTarget() const noexcept { return scroll_target_.IsValid(); }
    constexpr const ScrollTarget& GetScrollTarget() const noexcept { return scroll_target_; }

    // scroll_target が有効な場合、現在のレイアウトキャッシュから scroll_y を再評価する。
    // max_scroll によるクランプは行わない（SyncMaxScroll が担当する）。
    // 負値は NodeOffsetToScrollY 側で 0 に、node はキャッシュ末尾にクランプされる。
    constexpr void ApplyScrollTarget(const LayoutCache& cache) noexcept
    {
        if (!scroll_target_.IsValid()) {
            return;
        }
        scroll_y_ = NodeOffsetToScrollY(cache, scroll_target_.node, scroll_target_.offset);
    }

    // scroll_target が未設定なら、現在の可視先頭ノードから合成する。
    // レイアウト変化を挟む処理（OnPaint/OnResizeEnd/OnDeferredLayout 等）の直前に呼んで、
    // 「見ている位置を保つ」挙動を target ベースで表現する。
    constexpr void EnsureScrollTarget(const LayoutCache& cache, size_t node_count) noexcept
    {
        if (scroll_target_.IsValid()) {
            return;
        }
        // node_count > cache.size() の過渡状態に備えて effective に揃えてから渡す。
        // 「該当なし」のときは FindFirstVisibleNodeIndex が effective を返す契約。
        const size_t effective = std::min(node_count, cache.size());
        const int idx = FindFirstVisibleNodeIndex(cache, effective, scroll_y_);
        if (idx < static_cast<int>(effective)) {
            scroll_target_ = { idx, scroll_y_ - cache[idx].y_position };
        }
    }

    // target を触らずクランプも掛けない生の scroll_y 書き込み。
    // max_scroll が未確定な段階（ファイルロード直後など）のシード投入に使う。target との整合は呼び出し側責任。
    constexpr void SetScrollY(float y) noexcept { scroll_y_ = y; }

    constexpr bool IsScrollbarTracking() const noexcept { return is_scrollbar_tracking_; }
    constexpr void SetScrollbarTracking(bool v) noexcept { is_scrollbar_tracking_ = v; }

    // ---- 選択 ----

    constexpr const TextSelection& GetSelection() const noexcept { return selection_; }
    constexpr TextSelection& GetSelectionMut() noexcept { return selection_; }
    constexpr void SetSelection(const TextSelection& sel) noexcept { selection_ = sel; }

    constexpr int GetAnchorNode() const noexcept { return anchor_node_; }
    constexpr uint32_t GetAnchorPos() const noexcept { return anchor_pos_; }
    constexpr void SetAnchor(int node, uint32_t pos) noexcept
    {
        anchor_node_ = node;
        anchor_pos_ = pos;
    }

    constexpr bool IsDragging() const noexcept { return is_dragging_; }
    constexpr void SetDragging(bool v) noexcept { is_dragging_ = v; }

    constexpr int GetClickStartX() const noexcept { return click_start_x_; }
    constexpr int GetClickStartY() const noexcept { return click_start_y_; }
    constexpr void SetClickStart(int x, int y) noexcept
    {
        click_start_x_ = x;
        click_start_y_ = y;
    }

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
        const int last = static_cast<int>(nodes.size()) - 1;
        selection_ = TextSelection::MakeOrdered(
            0, 0, last, static_cast<uint32_t>(nodes[last].GetText().size()));
    }

    // ---- ズーム ----

    constexpr int GetZoomIndex() const noexcept { return zoom_index_; }
    // ZOOM_STEPS の有効範囲 [0, ZOOM_STEP_COUNT) に必ずクランプする。
    // 設定ファイル等の外部入力経路から不正値が来ても out-of-bounds にならない契約を setter 側で保証する。
    constexpr void SetZoomIndex(int idx) noexcept
    {
        zoom_index_ = std::clamp(idx, 0, ZOOM_STEP_COUNT - 1);
    }
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

private:
    // スクロール状態
    float scroll_y_ = 0.0f;
    float max_scroll_ = 0.0f;
    bool is_scrollbar_tracking_ = false;
    ScrollTarget scroll_target_{};

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
