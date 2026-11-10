#pragma once
// 描画・ヒット判定・reducer だけが読む。部品の寸法を変えるたびに広く組み直さないよう、大半の TU が読む ui_constants.h とも、
// 設定の保存やペインの計算が読む pane_layout.h とも分けてある。
#include "ui_types.h"
#include <d2d1.h>
#include <algorithm>
#include <cstdint>
#include <numbers>

// ナビゲーションオーバーレイボタンの定数（DIP単位）。
inline constexpr float NAV_BTN_SIZE = 32.0f;
inline constexpr float NAV_BTN_MARGIN = 16.0f;
inline constexpr float NAV_BTN_GAP = 2.0f;
inline constexpr float NAV_BTN_SCROLLBAR_OFFSET = 16.0f;

// 描画とヒットテスト (NavButtonHitTest) が共有するナビボタン矩形。テストもこの API で座標を得る。
inline D2D1_RECT_F NavBackButtonRect(const PaneRect& md_rect) noexcept
{
    const float x = md_rect.x + md_rect.width - NAV_BTN_MARGIN - NAV_BTN_SIZE * 2.0f - NAV_BTN_GAP - NAV_BTN_SCROLLBAR_OFFSET;
    const float y = md_rect.y + md_rect.height - NAV_BTN_MARGIN - NAV_BTN_SIZE;
    return D2D1::RectF(x, y, x + NAV_BTN_SIZE, y + NAV_BTN_SIZE);
}

inline D2D1_RECT_F NavForwardButtonRect(const PaneRect& md_rect) noexcept
{
    const D2D1_RECT_F back = NavBackButtonRect(md_rect);
    const float x = back.right + NAV_BTN_GAP;
    return D2D1::RectF(x, back.top, x + NAV_BTN_SIZE, back.bottom);
}

// 検索バーの定数（DIP単位）
inline constexpr float SEARCH_BAR_HEIGHT = 36.0f;
inline constexpr float SEARCH_BAR_PADDING = 6.0f;
inline constexpr float SEARCH_INPUT_MAX_WIDTH = 400.0f;
inline constexpr float SEARCH_BTN_SIZE = 28.0f;
inline constexpr float SEARCH_BAR_GAP = 4.0f;
inline constexpr float SEARCH_INPUT_HEIGHT = 24.0f;
inline constexpr float SEARCH_INPUT_TEXT_PAD_LEFT = 6.0f;
inline constexpr float SEARCH_INPUT_TEXT_PAD_RIGHT = 4.0f;
inline constexpr float SEARCH_MATCH_COUNT_WIDTH = 80.0f;

// 検索バーのフォントサイズ（DIP）。枠の寸法は本文ズームに追従しないので、
// 文字も固定値にして枠からのはみ出しを防ぐ。
inline constexpr float SEARCH_INPUT_FONT_SIZE = 13.0f;

// 検索バーの各要素の矩形を保持する構造体。
// 描画・ヒットテスト・ホバー判定の3箇所で共有し、レイアウト計算の重複を防ぐ。
struct SearchBarLayout {
    D2D1_RECT_F input_rect{};
    D2D1_RECT_F up_btn{};
    D2D1_RECT_F down_btn{};
    D2D1_RECT_F count_rect{};
    D2D1_RECT_F case_btn{};
    D2D1_RECT_F highlight_btn{};
    D2D1_RECT_F close_btn{};
    D2D1_RECT_F icon_rect{};
    // バー全体。ペインの下端に幅いっぱいに敷く。
    D2D1_RECT_F bar_rect{};

    // 検索入力のテキスト描画領域。描画(EmitSearchBar)とヒットテスト(app)で共有する。
    constexpr float text_left() const noexcept
    {
        return input_rect.left + SEARCH_INPUT_TEXT_PAD_LEFT;
    }
    constexpr float text_right() const noexcept
    {
        return input_rect.right - SEARCH_INPUT_TEXT_PAD_RIGHT;
    }
    constexpr float text_width() const noexcept
    {
        return text_right() - text_left();
    }
};

inline SearchBarLayout ComputeSearchBarLayout(const PaneRect& md_rect, bool has_query) noexcept
{
    SearchBarLayout l;
    const float bar_left = md_rect.x;
    const float bar_right = md_rect.x + md_rect.width;
    const float bar_bottom = md_rect.y + md_rect.height;
    const float bar_top = bar_bottom - SEARCH_BAR_HEIGHT;
    l.bar_rect = D2D1::RectF(bar_left, bar_top, bar_right, bar_bottom);

    const float center_y = bar_top + (SEARCH_BAR_HEIGHT - SEARCH_INPUT_HEIGHT) / 2.0f;
    const float center_bottom = center_y + SEARCH_INPUT_HEIGHT;

    float x = bar_left + SEARCH_BAR_PADDING;
    const auto place = [&x](float w, float top, float bottom) noexcept {
        const D2D1_RECT_F r = D2D1::RectF(x, top, x + w, bottom);
        x += w + SEARCH_BAR_GAP;
        return r;
    };
    const auto place_btn = [&] { return place(SEARCH_BTN_SIZE, center_y, center_bottom); };

    l.icon_rect = place(SEARCH_BTN_SIZE, bar_top, bar_bottom);
    l.input_rect = place(std::min(SEARCH_INPUT_MAX_WIDTH, (bar_right - bar_left) * 0.5f), center_y, center_bottom);
    l.up_btn = place_btn();
    l.down_btn = place_btn();
    if (has_query) {
        l.count_rect = place(SEARCH_MATCH_COUNT_WIDTH, bar_top, bar_bottom);
    }
    l.case_btn = place_btn();
    l.highlight_btn = place_btn();
    // close_btn だけバー右端へ寄せる。狭幅で左側ボタン群と重なる場合は従来の左詰め位置 x へフォールバック
    const float close_x = std::max(x, bar_right - SEARCH_BAR_PADDING - SEARCH_BTN_SIZE);
    l.close_btn = D2D1::RectF(close_x, center_y, close_x + SEARCH_BTN_SIZE, center_bottom);

    return l;
}

// 検索バーのヒット判定結果。クリック／ホバー／Controller で共有する。
enum class SearchBarHitZone : uint8_t {
    None,
    Up,
    Down,
    CaseSensitive,
    Highlight,
    Close,
    Input,
};

constexpr SearchBarHitZone HitTestSearchBar(const SearchBarLayout& sbl, float x, float y) noexcept
{
    if (y < sbl.bar_rect.top) {
        return SearchBarHitZone::None;
    }
    const struct {
        const D2D1_RECT_F& rect;
        SearchBarHitZone zone;
    } targets[] = {
        { sbl.up_btn,        SearchBarHitZone::Up            },
        { sbl.down_btn,      SearchBarHitZone::Down          },
        { sbl.case_btn,      SearchBarHitZone::CaseSensitive },
        { sbl.highlight_btn, SearchBarHitZone::Highlight     },
        { sbl.close_btn,     SearchBarHitZone::Close         },
        { sbl.input_rect,    SearchBarHitZone::Input         },
    };
    for (const auto& t : targets) {
        if (PointInRect(x, y, t.rect)) {
            return t.zone;
        }
    }
    return SearchBarHitZone::None;
}

// 本文のリンクを指している間、本文ペインの下端に出すリンクの行き先 (Chrome の左下の表示と同じ)。
inline constexpr float LINK_STATUS_HEIGHT = 22.0f;
inline constexpr float LINK_STATUS_PADDING_X = 8.0f;
inline constexpr float LINK_STATUS_CORNER = 4.0f;

// 行き先の表示を寄せる側。ポインタが表示と同じ高さにあるときは、ポインタと反対の半分に寄せて指しているリンクを隠さない。
enum class LinkStatusDock : uint8_t {
    // 左端から、帯の幅いっぱいまで伸ばす。
    Left,
    LeftHalf,
    RightHalf,
};

// 行き先の表示を置ける帯。本文ペインの下端 (検索バーを出していればその上) で、右端はナビボタンとスクロールバーの手前まで。
// 描画とホバーの判定 (ポインタが帯の高さにあるか) で共有する。
inline D2D1_RECT_F LinkStatusBand(const PaneRect& md_rect, bool search_bar_visible) noexcept
{
    const float bottom = md_rect.y + md_rect.height - (search_bar_visible ? SEARCH_BAR_HEIGHT : 0.0f);
    const float right = std::max(md_rect.x, NavBackButtonRect(md_rect).left - NAV_BTN_GAP);
    return D2D1::RectF(md_rect.x, bottom - LINK_STATUS_HEIGHT, right, bottom);
}

// 半分に寄せたときは帯の中央で分ける (EmitLinkStatus)。
inline LinkStatusDock LinkStatusDockAt(const D2D1_RECT_F& band, DipPoint pointer) noexcept
{
    if (pointer.y < band.top) {
        return LinkStatusDock::Left;
    }
    return pointer.x < RectCenter(band).x ? LinkStatusDock::RightHalf : LinkStatusDock::LeftHalf;
}

inline constexpr float GESTURE_OVERLAY_WIDTH = 280.0f;

inline constexpr float TOAST_OVERLAY_HEIGHT = 48.0f;

inline constexpr float TWO_PI = std::numbers::pi_v<float> * 2.0f;

namespace spinner {
inline constexpr float RADIUS = 20.0f;
inline constexpr int DOT_COUNT = 8;
} // namespace spinner
