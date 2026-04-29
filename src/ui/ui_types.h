#pragma once

// UI レイヤで広く参照される小さい値型・定数を集約する。
// 個別ヘッダに分けるほどの規模ではないが、render/app/ui の境界を跨いで使われるので
// 1 か所に集めて include 連鎖を浅く保つ。

// DIP 単位の矩形。`<d2d1.h>` を巻き込まずに公開 API で矩形を扱うための型。
// 描画レイヤでは ToD2DRect() で D2D1_RECT_F に変換する
// （render_params.h で layout 互換を static_assert で担保）。
struct DipRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

// 点が矩形内にあるか判定する（D2D 規約に合わせ右辺・下辺は排他的）。
// `left/top/right/bottom` を持つ任意の矩形型 (DipRect / D2D1_RECT_F 等) に対応する。
template <class Rect>
inline constexpr bool PointInRect(float x, float y, const Rect& r) noexcept
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

struct PaneRect {
    float x, y, width, height;
};

struct ScrollState {
    float scroll_y = 0.0f;
    float max_scroll = 0.0f;
};

// スクロールバー共通定数
inline constexpr float PANE_SCROLLBAR_WIDTH = 8.0f;
inline constexpr float PANE_SCROLLBAR_THUMB_MIN = 24.0f;
inline constexpr float PANE_SCROLLBAR_MARGIN = 2.0f;
inline constexpr float PANE_SCROLLBAR_HIT_PADDING = 4.0f;

// セッション復元とナビゲーション履歴（戻る/進む）の両方をノードベースで統合する。
// 各フィールドは App の複数箇所から直接読み書きされるため public にしている。
struct ScrollRestoration {
    int pending_restore_node = -1;
    int pending_restore_offset = 0;

    constexpr bool HasNodeRestore() const noexcept { return pending_restore_node >= 0; }

    constexpr void SetNodeRestore(int node, int offset) noexcept
    {
        pending_restore_node = node;
        pending_restore_offset = offset;
    }

    constexpr void ClearNodeRestore() noexcept
    {
        pending_restore_node = -1;
        pending_restore_offset = 0;
    }
};

// MD ペインのオーバーレイボタンのホバー状態。
// 各フィールドはホバー対象のノードインデックスで、-1 は「該当なし」を表す。
struct HoveredButtons {
    int copy = -1;
    int save = -1;
    int svg_copy = -1;

    bool operator==(const HoveredButtons&) const = default;
};

// ナビゲーションボタン（戻る/進む）のホバー状態。
enum class NavButtonHover : uint8_t {
    None,
    Back,
    Forward
};
