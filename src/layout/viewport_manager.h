#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "ui_types.h"
#include <algorithm>

// スクロール位置の「目的地」を表す値型。
// node が有効な間、レイアウトが変化するたびに scroll_y = text_top[node] + offset として再評価される。
// ユーザー発のピクセルスクロール (SetScrollY / ScrollTo / DirectScrollBy) で無効化される。
struct ScrollTarget {
    int node = -1;       // -1 = 無効
    float offset = 0.0f; // ノード先頭からのピクセル距離

    constexpr bool IsValid() const noexcept
    {
        return node >= 0;
    }
};

// 見出しノードを md ペイン上端の heading_spacing_above 分だけ下に配置する ScrollTarget を作る。
// TOCクリック・#anchor ナビゲーションで共通して使う。
constexpr ScrollTarget MakeHeadingTopTarget(int node, float heading_spacing_above) noexcept
{
    return { node, -heading_spacing_above };
}


// スクロール、選択、ズームの純粋な状態管理。
class ViewportManager {
public:
    // ---- スクロール ----

    constexpr float GetScrollY() const noexcept
    {
        return scroll_y_;
    }

    // ピクセル絶対スクロール。scroll_target を無効化する。
    constexpr void ScrollTo(float position, float max_scroll) noexcept
    {
        scroll_y_ = std::clamp(position, 0.0f, max_scroll);
        scroll_target_ = {};
    }

    // 現在の scroll_y を [0, max_scroll] にクランプし、scroll_target を無効化する。
    // ApplyScrollTarget 直後に「以降のレイアウト変化で範囲外へ戻らない」確定処理として使う。
    constexpr void ClampAndDetach(float max_scroll) noexcept
    {
        ScrollTo(scroll_y_, max_scroll);
    }

    // ユーザー発のピクセルスクロール。scroll_target を無効化する。
    constexpr void DirectScrollBy(float delta, float max_scroll) noexcept
    {
        ScrollTo(scroll_y_ + delta, max_scroll);
    }

    // scroll_target は残す (以降のレイアウト変化でも見ている位置を保つ)。
    constexpr void ClampScroll(float max_scroll) noexcept
    {
        scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll);
    }

    // 下端がscroll_y_より下にある最初のノードを見つける。
    // 表示可能なノードが存在しない場合は-1を返す。
    constexpr int FindFirstVisibleNode(const LayoutCache& cache) const noexcept
    {
        const int idx = FindFirstVisibleNodeIndex(cache, scroll_y_);
        return idx < static_cast<int>(cache.size()) ? idx : -1;
    }

    // scroll_target を明示設定する。内部の scroll_y は更新しないので、
    // 呼び出し側は直後に ApplyScrollTarget を呼ぶか、レイアウト操作経由のフック
    // で再評価されるのを待つ必要がある。
    constexpr void SetScrollTarget(int node, float offset) noexcept
    {
        scroll_target_ = { node, offset };
    }

    constexpr const ScrollTarget& GetScrollTarget() const noexcept
    {
        return scroll_target_;
    }

    // scroll_target が有効な場合、現在のレイアウトキャッシュから scroll_y を再評価する。
    // max_scroll によるクランプは行わない（ClampScroll が担当する）。
    // 負値は NodeOffsetToScrollY 側で 0 に、node はキャッシュ末尾にクランプされる。
    constexpr void ApplyScrollTarget(const LayoutCache& cache) noexcept
    {
        if (!scroll_target_.IsValid()) {
            return;
        }
        scroll_y_ = NodeOffsetToScrollY(cache, scroll_target_.node, scroll_target_.offset);
    }

    // scroll_target が未設定なら、現在の可視先頭ノードから合成する。
    // レイアウト変化を挟む処理（OnPaint/SettleViewport/OnDeferredLayout 等）の直前に呼んで、
    // 「見ている位置を保つ」挙動を target ベースで表現する。
    constexpr void EnsureScrollTarget(const LayoutCache& cache) noexcept
    {
        if (scroll_target_.IsValid()) {
            return;
        }
        if (const auto t = CurrentTarget(cache); t.IsValid()) {
            scroll_target_ = t;
        }
    }

    // 可視先頭ノード基準の現在位置。ノードが無ければ node=-1, offset=scroll_y。
    constexpr ScrollTarget CurrentTarget(const LayoutCache& cache) const noexcept
    {
        const int idx = FindFirstVisibleNode(cache);
        const float top = idx >= 0 ? cache.Top(static_cast<size_t>(idx)) : 0.0f;
        return { idx, scroll_y_ - top };
    }

    // クランプを掛けない scroll_y 書き込み。scroll_target を無効化する。
    // max_scroll が未確定な段階（ファイルロード直後など）のシード投入に使う。
    constexpr void SetScrollY(float y) noexcept
    {
        scroll_y_ = y;
        scroll_target_ = {};
    }

    // ---- 選択 ----

    constexpr auto& GetSelection(this auto& self) noexcept
    {
        return self.selection_;
    }
    constexpr void SetSelection(const TextSelection& sel) noexcept
    {
        selection_ = sel;
    }

    constexpr void ClearSelection() noexcept
    {
        selection_.Clear();
    }

    constexpr void SelectAll(const std::vector<Node>& nodes) noexcept
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

    constexpr ZoomLevel& Zoom() noexcept
    {
        return zoom_;
    }
    constexpr const ZoomLevel& Zoom() const noexcept
    {
        return zoom_;
    }

private:
    // --- 4 バイトアライメント ---
    float scroll_y_ = 0.0f;
    ScrollTarget scroll_target_{}; // int(4) + float(4)
    TextSelection selection_; // 20B

    // --- 1 バイトアライメント ---
    ZoomLevel zoom_;
};
