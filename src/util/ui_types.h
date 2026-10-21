#pragma once
#include <cstdint>

// include 連鎖を浅く保つため 1 か所に集約。

struct DipPoint {
    float x = 0.0f;
    float y = 0.0f;
};

// <d2d1.h> を巻き込まずに矩形を扱う。
struct DipRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

// D2D 規約に合わせ右辺・下辺は排他的。
template <class Rect>
constexpr bool PointInRect(float x, float y, const Rect& r) noexcept
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// 右辺・下辺を含む inclusive 版。
template <class Rect>
constexpr bool PointInRectInclusive(float x, float y, const Rect& r) noexcept
{
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

struct PaneRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct ScrollState {
    float scroll_y = 0.0f;
};

inline constexpr float PANE_SCROLLBAR_WIDTH = 8.0f;
inline constexpr float PANE_SCROLLBAR_THUMB_MIN = 24.0f;
inline constexpr float PANE_SCROLLBAR_MARGIN = 2.0f;
inline constexpr float PANE_SCROLLBAR_HIT_PADDING = 4.0f;

// コードブロック・図の右上に重ねるボタン。
enum class OverlayButton : uint8_t {
    None,
    CodeCopy,
    DiagramSave,
    DiagramCopy,
};

// マウス位置にあるオーバーレイボタン。マウス座標は 1 点なので同時に当たるのは 1 つだけ。
struct OverlayButtonHit {
    int node = -1;
    OverlayButton kind = OverlayButton::None;

    constexpr bool Any() const noexcept
    {
        return kind != OverlayButton::None;
    }
    bool operator==(const OverlayButtonHit&) const = default;
};

// 進行中の左ボタンドラッグの種類。
enum class LeftDrag : uint8_t {
    None,
    TextSelect,
    Splitter,      // LeftDragState::pane の右端
    PaneScrollbar, // LeftDragState::pane のスクロールバー
    MdScrollbar,
    BlockHScroll,
    SearchInput,
};

// ノード番号や検索語に結び付くドラッグ。文書を差し替えると対象を失う。
constexpr bool IsDocumentBoundDrag(LeftDrag d) noexcept
{
    return d == LeftDrag::TextSelect || d == LeftDrag::BlockHScroll || d == LeftDrag::SearchInput;
}

// 履歴移動の向き。ジェスチャー / 水平スワイプの向きと、ナビボタンのホバー対象を表す。
enum class NavDirection : uint8_t {
    None,
    Back,
    Forward
};
