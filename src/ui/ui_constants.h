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

// ファイル監視タイマー間隔 (ms)
inline constexpr UINT FILE_WATCH_INTERVAL_MS = 250;

// クリック判定距離の二乗（ドラッグ選択とクリックを区別するための閾値）
inline constexpr int CLICK_DISTANCE_THRESHOLD_SQ = 25;

// 点が矩形内にあるか判定する（D2D規約に合わせ右辺・下辺は排他的）。
inline constexpr bool PointInRect(float x, float y, const D2D1_RECT_F& r) noexcept {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

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

// コードブロック背景の右上を基準にコピーボタンの矩形を返す。
// block_right: コードブロック背景の右端, block_top: コードブロック背景の上端
inline D2D1_RECT_F CopyButtonRect(float block_right, float block_top) noexcept {
    const float bx = block_right - COPY_BTN_MARGIN - COPY_BTN_SIZE;
    const float by = block_top + COPY_BTN_MARGIN;
    return D2D1::RectF(bx, by, bx + COPY_BTN_SIZE, by + COPY_BTN_SIZE);
}
