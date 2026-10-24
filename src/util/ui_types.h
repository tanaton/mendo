#pragma once
#include <algorithm>
#include <cstdint>
#include <iterator>

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

// Chrome風の段階的ズームステップ
inline constexpr float ZOOM_STEPS[] = {
    0.25f, 0.33f, 0.50f, 0.67f, 0.75f, 0.80f, 0.90f,
    1.00f,
    1.10f, 1.25f, 1.50f, 1.75f, 2.00f, 2.50f, 3.00f, 4.00f, 5.00f
};
inline constexpr int ZOOM_STEP_COUNT = static_cast<int>(std::size(ZOOM_STEPS));
inline constexpr int ZOOM_DEFAULT_INDEX = 7;
static_assert(ZOOM_STEPS[ZOOM_DEFAULT_INDEX] == 1.0f);

enum class ZoomDirection : uint8_t {
    In,
    Out,
    Reset
};

// ZOOM_STEPS の段。本文とサイドペイン (ファイル一覧・目次) がそれぞれ持つ。
// 段を動かす操作は変化したかを返す。float の戻り値で「0.0 == 変化なし」を表すと
// ZOOM_STEPS に 0.0 が入った瞬間に破綻するし、SSOT を段に寄せる方針と相性が悪い。
class ZoomLevel {
public:
    constexpr int Index() const noexcept
    {
        return index_;
    }
    // 設定ファイル等の外部入力から不正値が来ても範囲外を指さないよう、ここで必ずクランプする。
    constexpr void SetIndex(int idx) noexcept
    {
        index_ = static_cast<uint8_t>(std::clamp(idx, 0, ZOOM_STEP_COUNT - 1));
    }
    constexpr float Value() const noexcept
    {
        return ZOOM_STEPS[index_];
    }
    // 表示用の百分率。
    constexpr int Percent() const noexcept
    {
        return static_cast<int>(Value() * 100.0f + 0.5f);
    }
    constexpr bool CanZoomIn() const noexcept
    {
        return index_ < ZOOM_STEP_COUNT - 1;
    }
    constexpr bool CanZoomOut() const noexcept
    {
        return index_ > 0;
    }
    constexpr bool IsDefault() const noexcept
    {
        return index_ == ZOOM_DEFAULT_INDEX;
    }
    constexpr bool ZoomIn() noexcept
    {
        if (!CanZoomIn()) {
            return false;
        }
        ++index_;
        return true;
    }
    constexpr bool ZoomOut() noexcept
    {
        if (!CanZoomOut()) {
            return false;
        }
        --index_;
        return true;
    }
    constexpr bool ZoomReset() noexcept
    {
        if (IsDefault()) {
            return false;
        }
        index_ = static_cast<uint8_t>(ZOOM_DEFAULT_INDEX);
        return true;
    }
    constexpr bool Step(ZoomDirection direction) noexcept
    {
        switch (direction) {
        case ZoomDirection::In:
            return ZoomIn();
        case ZoomDirection::Out:
            return ZoomOut();
        case ZoomDirection::Reset:
            return ZoomReset();
        }
        return false;
    }
    // 押せるか (段が動くか)。メニューのボタンの有効・無効に使う。
    constexpr bool CanStep(ZoomDirection direction) const noexcept
    {
        switch (direction) {
        case ZoomDirection::In:
            return CanZoomIn();
        case ZoomDirection::Out:
            return CanZoomOut();
        case ZoomDirection::Reset:
            return !IsDefault();
        }
        return false;
    }

private:
    uint8_t index_ = static_cast<uint8_t>(ZOOM_DEFAULT_INDEX);
};
