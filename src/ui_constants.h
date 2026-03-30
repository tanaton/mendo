#pragma once
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

// ホバー時のヒットテスト省略判定用の距離の二乗
inline constexpr int HOVER_THROTTLE_DISTANCE_SQ = 16;

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

// 点が矩形内にあるか判定する（D2D規約に合わせ右辺・下辺は排他的）。
inline constexpr bool PointInRect(float x, float y, const D2D1_RECT_F& r) noexcept {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// ペインヘッダー閉じるボタンの余白
inline constexpr float PANE_CLOSE_BTN_MARGIN = 2.0f;

// ペインヘッダー内の閉じるボタン矩形を返す（ペインローカル座標）。
inline D2D1_RECT_F PaneCloseButtonRect(float pane_width, float header_height) noexcept {
    float btn_size = header_height - 2.0f * PANE_CLOSE_BTN_MARGIN;
    float btn_x = pane_width - btn_size - PANE_CLOSE_BTN_MARGIN;
    float btn_y = (header_height - btn_size) / 2.0f;
    return D2D1::RectF(btn_x, btn_y, btn_x + btn_size, btn_y + btn_size);
}

// ペインヘッダー内の更新ボタン矩形を返す（閉じるボタンの左隣、ペインローカル座標）。
inline D2D1_RECT_F PaneRefreshButtonRect(float pane_width, float header_height) noexcept {
    D2D1_RECT_F close_rect = PaneCloseButtonRect(pane_width, header_height);
    float btn_size = close_rect.right - close_rect.left;
    float btn_x = close_rect.left - btn_size - PANE_CLOSE_BTN_MARGIN;
    float btn_y = close_rect.top;
    return D2D1::RectF(btn_x, btn_y, btn_x + btn_size, btn_y + btn_size);
}

// スクロール位置を物理ピクセル境界にスナップする。
// ClearTypeヒンティングのフレーム間変動によるテキストのガタつきを防止する。
// dpi_scale: DPI / DEFAULT_DPI（例: 100%→1.0, 150%→1.5, 200%→2.0）
inline float SnapScrollToPixel(float scroll_y, float dpi_scale) noexcept {
    return std::round(scroll_y * dpi_scale) / dpi_scale;
}

// コードブロック背景の右上を基準にコピーボタンの矩形を返す。
// block_right: コードブロック背景の右端, block_top: コードブロック背景の上端
inline D2D1_RECT_F CopyButtonRect(float block_right, float block_top) noexcept {
    float bx = block_right - COPY_BTN_MARGIN - COPY_BTN_SIZE;
    float by = block_top + COPY_BTN_MARGIN;
    return D2D1::RectF(bx, by, bx + COPY_BTN_SIZE, by + COPY_BTN_SIZE);
}
