#pragma once
#include "dip_rect.h"
#include <d2d1.h>
#include <cmath>
#include <numbers>

// 選択範囲のハイライトカラー
inline constexpr D2D1_COLOR_F SELECTION_COLOR = { 0.26f, 0.56f, 0.84f, 0.3f };

inline constexpr float TWO_PI = std::numbers::pi_v<float> *2.0f;

// ローディングスピナーの定数
namespace spinner {
inline constexpr float RADIUS = 20.0f;
inline constexpr float DOT_RADIUS = 3.0f;
inline constexpr int DOT_COUNT = 8;
inline constexpr float ROTATION_INCREMENT = 0.15f;
inline constexpr float DOT_FADE_FACTOR = 0.85f;
}

// Windowsの基準DPI（100%スケーリング時の値）
inline constexpr float DEFAULT_DPI = 96.0f;

// マウスホイールスクロールの倍率
inline constexpr float MOUSE_WHEEL_SCROLL_MULTIPLIER = 0.8f;

// 目次ペインの見出しレベル毎のインデント幅
inline constexpr float TOC_INDENT_PER_LEVEL = 12.0f;

// テーブル描画で共有するレイアウト定数
inline constexpr float TABLE_CELL_PADDING = 8.0f;
inline constexpr float TABLE_BORDER_WIDTH = 1.0f;

// テーブルストライプ（偶数行背景）のアルファ値
inline constexpr float TABLE_STRIPE_ALPHA_DARK = 0.05f;
inline constexpr float TABLE_STRIPE_ALPHA_LIGHT = 0.02f;

// ナビゲーションオーバーレイボタンの定数（DIP単位）。
// レンダラー（描画）とhit_test_service（クリック検出）の間で共有される。
inline constexpr float NAV_BTN_SIZE = 32.0f;
inline constexpr float NAV_BTN_MARGIN = 16.0f;
inline constexpr float NAV_BTN_GAP = 2.0f;
inline constexpr float NAV_BTN_CORNER = 6.0f;
inline constexpr float NAV_BTN_SCROLLBAR_OFFSET = 16.0f;

// コードブロック コピーボタンの定数（DIP単位）。
// CommandGenerator（描画）とHitTestService（クリック検出）の間で共有される。
inline constexpr float COPY_BTN_SIZE = 28.0f;
inline constexpr float COPY_BTN_MARGIN = 6.0f;
inline constexpr float COPY_BTN_CORNER = 4.0f;
inline constexpr float COPY_BTN_GAP = 4.0f;

inline constexpr float INLINE_CODE_PAD_X = 3.0f;
inline constexpr float INLINE_CODE_PAD_Y = 2.0f;
inline constexpr float INLINE_CODE_CORNER = 3.0f;
inline constexpr float CODE_BLOCK_CORNER = 4.0f;

inline constexpr float LIST_BULLET_RADIUS = 3.0f;
inline constexpr float LIST_BULLET_X_FACTOR = 0.6f;
inline constexpr float LIST_NUMBER_PAD_LEFT = 4.0f;
inline constexpr float LIST_NUMBER_PAD_RIGHT = 8.0f;

inline constexpr float TASK_CHECKBOX_HEIGHT_FACTOR = 1.5f;
inline constexpr float TABLE_ROW_HEIGHT_FACTOR = 1.4f;

// DirectWriteメトリクス取得失敗時の代替値
inline constexpr float FALLBACK_LINE_HEIGHT_FACTOR = 1.3f;

// ツールチップ表示遅延 (ms)
inline constexpr UINT TOOLTIP_DELAY_MS = 500;

// クリック判定距離の二乗（ドラッグ選択とクリックを区別するための閾値）
inline constexpr int CLICK_DISTANCE_THRESHOLD_SQ = 25;

// ペインヘッダー閉じるボタンの余白
inline constexpr float PANE_CLOSE_BTN_MARGIN = 2.0f;

// ペインヘッダー内の閉じるボタン矩形を返す（ペインローカル座標）。
inline D2D1_RECT_F PaneCloseButtonRect(float pane_width, float header_height) noexcept {
    const float btn_size = header_height - 2.0f * PANE_CLOSE_BTN_MARGIN;
    const float btn_x = pane_width - btn_size - PANE_CLOSE_BTN_MARGIN;
    const float btn_y = (header_height - btn_size) / 2.0f;
    return D2D1::RectF(btn_x, btn_y, btn_x + btn_size, btn_y + btn_size);
}

// ペインヘッダー内の更新ボタン矩形を返す（閉じるボタンの左隣、ペインローカル座標）。
inline D2D1_RECT_F PaneRefreshButtonRect(float pane_width, float header_height) noexcept {
    const D2D1_RECT_F close_rect = PaneCloseButtonRect(pane_width, header_height);
    const float btn_size = close_rect.right - close_rect.left;
    const float btn_x = close_rect.left - btn_size - PANE_CLOSE_BTN_MARGIN;
    const float btn_y = close_rect.top;
    return D2D1::RectF(btn_x, btn_y, btn_x + btn_size, btn_y + btn_size);
}

// スクロール位置を物理ピクセル境界にスナップする。
// ClearTypeヒンティングのフレーム間変動によるテキストのガタつきを防止する。
// dpi_scale: DPI / DEFAULT_DPI（例: 100%→1.0, 150%→1.5, 200%→2.0）
inline float SnapScrollToPixel(float scroll_y, float dpi_scale) noexcept {
    return std::round(scroll_y * dpi_scale) / dpi_scale;
}

// タイトルバーのテキストフォントサイズ（DIP）。
// pane_font_size と共有すると Zoom に追従してしまうため、専用の固定値を用意する。
inline constexpr float TITLEBAR_TEXT_FONT_SIZE = 13.0f;

// 検索バーの定数（DIP単位）
inline constexpr float SEARCH_BAR_HEIGHT = 36.0f;
inline constexpr float SEARCH_BAR_PADDING = 6.0f;
inline constexpr float SEARCH_INPUT_MAX_WIDTH = 400.0f;
inline constexpr float SEARCH_BTN_SIZE = 28.0f;
inline constexpr float SEARCH_BAR_GAP = 4.0f;
inline constexpr float SEARCH_BAR_CORNER = 4.0f;
inline constexpr float SEARCH_INPUT_HEIGHT = 24.0f;
inline constexpr float SEARCH_INPUT_TEXT_PAD_LEFT = 6.0f;
inline constexpr float SEARCH_INPUT_TEXT_PAD_RIGHT = 4.0f;
inline constexpr float SEARCH_MATCH_COUNT_WIDTH = 80.0f;

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
    float bar_top = 0.0f;
    float bar_bottom = 0.0f;
};

inline SearchBarLayout ComputeSearchBarLayout(float md_left, float md_width, float md_bottom, bool has_query) noexcept
{
    SearchBarLayout l;
    const float bar_left = md_left;
    const float bar_right = md_left + md_width;
    l.bar_top = md_bottom - SEARCH_BAR_HEIGHT;
    l.bar_bottom = md_bottom;

    const float btn = SEARCH_BTN_SIZE;
    const float gap = SEARCH_BAR_GAP;
    const float input_h = SEARCH_INPUT_HEIGHT;
    const float center_y = l.bar_top + (SEARCH_BAR_HEIGHT - input_h) / 2.0f;

    float x = bar_left + SEARCH_BAR_PADDING;
    l.icon_rect = D2D1::RectF(x, l.bar_top, x + btn, l.bar_bottom);
    x += btn + gap;

    const float input_w = std::min(SEARCH_INPUT_MAX_WIDTH, (bar_right - bar_left) * 0.5f);
    l.input_rect = D2D1::RectF(x, center_y, x + input_w, center_y + input_h);
    x += input_w + gap;

    l.up_btn = D2D1::RectF(x, center_y, x + btn, center_y + input_h);
    x += btn + gap;
    l.down_btn = D2D1::RectF(x, center_y, x + btn, center_y + input_h);
    x += btn + gap;

    if (has_query) {
        l.count_rect = D2D1::RectF(x, l.bar_top, x + SEARCH_MATCH_COUNT_WIDTH, l.bar_bottom);
        x += SEARCH_MATCH_COUNT_WIDTH + gap;
    }

    l.case_btn = D2D1::RectF(x, center_y, x + btn, center_y + input_h);
    x += btn + gap;
    l.highlight_btn = D2D1::RectF(x, center_y, x + btn, center_y + input_h);
    x += btn + gap;
    l.close_btn = D2D1::RectF(x, center_y, x + btn, center_y + input_h);

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

inline SearchBarHitZone HitTestSearchBar(const SearchBarLayout& sbl, float x, float y) noexcept
{
    if (y < sbl.bar_top) {
        return SearchBarHitZone::None;
    }
    if (PointInRect(x, y, sbl.up_btn)) {
        return SearchBarHitZone::Up;
    }
    if (PointInRect(x, y, sbl.down_btn)) {
        return SearchBarHitZone::Down;
    }
    if (PointInRect(x, y, sbl.case_btn)) {
        return SearchBarHitZone::CaseSensitive;
    }
    if (PointInRect(x, y, sbl.highlight_btn)) {
        return SearchBarHitZone::Highlight;
    }
    if (PointInRect(x, y, sbl.close_btn)) {
        return SearchBarHitZone::Close;
    }
    if (PointInRect(x, y, sbl.input_rect)) {
        return SearchBarHitZone::Input;
    }
    return SearchBarHitZone::None;
}

// デフォルトウィンドウサイズ（ピクセル）
inline constexpr int DEFAULT_WINDOW_WIDTH = 1600;
inline constexpr int DEFAULT_WINDOW_HEIGHT = 900;

// キーボードスクロール量（DIP）
inline constexpr float SCROLL_LINE_AMOUNT = 40.0f;
inline constexpr float SCROLL_PAGE_FACTOR = 0.9f;

// ジェスチャーオーバーレイのサイズ（DIP）
inline constexpr float GESTURE_OVERLAY_WIDTH = 280.0f;
inline constexpr float GESTURE_OVERLAY_HEIGHT = 80.0f;
inline constexpr float GESTURE_OVERLAY_CORNER = 12.0f;

// トーストオーバーレイのサイズ（DIP）
inline constexpr float TOAST_OVERLAY_WIDTH = 320.0f;
inline constexpr float TOAST_OVERLAY_HEIGHT = 48.0f;
inline constexpr float TOAST_OVERLAY_CORNER = 8.0f;
inline constexpr float TOAST_OVERLAY_BOTTOM_OFFSET = 16.0f;

// ジェスチャー軌跡のスタイル
inline constexpr float GESTURE_TRAIL_STROKE_WIDTH = 4.0f;

// ダイアグラム（Mermaid 等）右上に並ぶオーバーレイボタンのスロット。
// 値は OverlayButtonRect の button_index に対応し、0 が最も右、左隣に並ぶごとに +1。
enum class DiagramButtonSlot : int {
    Save = 0,
    SvgCopy = 1,
};

// 要素の右上を基準にオーバーレイボタン（コピー/保存）の矩形を返す。
// anchor_right: 基準領域の右端, anchor_top: 基準領域の上端
// button_index: 0=最も右, 1 以降は左隣に COPY_BTN_GAP 分ずれる。
inline D2D1_RECT_F OverlayButtonRect(float anchor_right, float anchor_top, int button_index = 0) noexcept {
    const float bx = anchor_right - COPY_BTN_MARGIN - COPY_BTN_SIZE
        - static_cast<float>(button_index) * (COPY_BTN_SIZE + COPY_BTN_GAP);
    const float by = anchor_top + COPY_BTN_MARGIN;
    return D2D1::RectF(bx, by, bx + COPY_BTN_SIZE, by + COPY_BTN_SIZE);
}

// Mermaidダイアグラムのビットマップ描画矩形を計算する（スケーリング＋中央寄せ）。
// CommandGenerator（描画）とHitTestService（クリック検出）の間で共有される。
inline D2D1_RECT_F MermaidBitmapRect(float diagram_w, float diagram_h,
    float x, float cw, float y) noexcept
{
    float draw_w = diagram_w;
    float draw_h = diagram_h;
    if (draw_w > cw && draw_w > 0) {
        const float scale = cw / draw_w;
        draw_h *= scale;
        draw_w = cw;
    }
    const float dx = x + (cw - draw_w) * 0.5f;
    return D2D1::RectF(dx, y, dx + draw_w, y + draw_h);
}
