#pragma once
#include "ui_types.h"
#include <d2d1.h>
#include <algorithm>
#include <cmath>

// layout_cache.h と theme.h を通じて大半の TU が読むので、文書のレイアウトと DPI の換算に要るものだけを置く。
// ペインの配置は pane_layout.h、本文ペインに重ねる部品 (検索バー・ナビボタン・リンク先の表示など) の幾何は chrome_layout.h。

// Windowsの基準DPI（100%スケーリング時の値）
inline constexpr float DEFAULT_DPI = 96.0f;

// dpi が 0 以下なら 1.0 (DPI 未初期化時のフォールバック)、それ以外は dpi / 96。
constexpr float DpiScaleFrom(float dpi) noexcept
{
    return (dpi > 0.0f) ? (dpi / DEFAULT_DPI) : 1.0f;
}

// テーブル描画で共有するレイアウト定数
inline constexpr float TABLE_CELL_PADDING = 8.0f;
inline constexpr float TABLE_BORDER_WIDTH = 1.0f;

// 図を描き終えるまでの placeholder 最小高さ (DIP)。
inline constexpr float MIN_DIAGRAM_PLACEHOLDER_HEIGHT = 60.0f;
// 深いネストでもノードに残す本文幅 (DIP)。
inline constexpr float MIN_NODE_CONTENT_WIDTH = 64.0f;

// width から inset (余白・インデント) を引いた本文幅。引きすぎても MIN_NODE_CONTENT_WIDTH は残す
// (width 自体がそれより狭ければ width)。負の幅ではテキストレイアウトを作れず、画像は原寸デコードになる。
constexpr float ShrinkKeepingMinContent(float width, float inset) noexcept
{
    return std::max(width - inset, std::min(width, MIN_NODE_CONTENT_WIDTH));
}

// ブロック横スクロールバーの bar 上端 Y 座標 (ペインローカル document Y)。
// extra_padding は CodeBlock の背景下端 padding 内に置く場合に渡す (Table は 0)。
constexpr float BlockHScrollbarBarY(float entry_text_top, float entry_height, float extra_padding) noexcept
{
    return entry_text_top + entry_height + extra_padding - PANE_SCROLLBAR_WIDTH - PANE_SCROLLBAR_MARGIN;
}

// thumb の幅 (DIP)。track の縦と異なり最小幅張り付き時もドラッグ範囲を可動域に揃える必要があるので、
// 描画とドラッグの ratio 計算で同じ式を共有する。
constexpr float BlockHScrollbarThumbWidth(float visible_width, float block_width) noexcept
{
    if (block_width <= 0.0f) {
        return 0.0f;
    }
    const float raw = visible_width * (visible_width / block_width);
    // 可視幅が最小幅を下回るとき std::clamp は下限 > 上限で未定義なので、可視幅に収める側を優先する。
    return std::min(std::max(raw, PANE_SCROLLBAR_THUMB_MIN), visible_width);
}

// バーのヒット矩形 (block_x / bar_y と同じ座標系)。クリック判定 / 描画位置の式を一元化する。
inline D2D1_RECT_F BlockHScrollbarHitRect(float block_x, float visible_width, float bar_y) noexcept
{
    return D2D1::RectF(
        block_x,
        bar_y - PANE_SCROLLBAR_HIT_PADDING,
        block_x + visible_width,
        bar_y + PANE_SCROLLBAR_WIDTH + PANE_SCROLLBAR_HIT_PADDING);
}

// コードブロック・図の右上のボタン (OverlayButtonRect) の定数（DIP単位）。描画とヒット判定で共有する。
inline constexpr float COPY_BTN_SIZE = 28.0f;
inline constexpr float COPY_BTN_MARGIN = 6.0f;
inline constexpr float COPY_BTN_GAP = 4.0f;

// 本文の行送りをフォントサイズの何倍にするか (GitHub の line-height: 1.5)。
// 行頭に重ねて描くリスト番号・チェックボックスも同じ行送りにしてベースラインを揃える。
inline constexpr float BODY_LINE_HEIGHT_FACTOR = 1.5f;

inline constexpr float TASK_CHECKBOX_HEIGHT_FACTOR = 1.5f;
inline constexpr float TABLE_ROW_HEIGHT_FACTOR = 1.4f;

// 無効状態の UI (ペインヘッダーボタン・メニュー項目) の不透明度
inline constexpr float DISABLED_UI_ALPHA = 0.35f;

// DIP 値を物理ピクセル境界にスナップする。
// スクロール位置に適用すれば ClearType ヒンティングのフレーム間変動による
// テキストのガタつきを防止できる。bullet 等の小半径図形にも有効。
// dpi_scale: DPI / DEFAULT_DPI（例: 100%→1.0, 150%→1.5, 200%→2.0）
inline float SnapToPhysicalPixel(float v, float dpi_scale) noexcept
{
    return std::round(v * dpi_scale) / dpi_scale;
}

// DIP → ピクセル変換（切り捨て）。ウィンドウサイズ・クリッピング等の整数ピクセル値に使う。
constexpr int DipToPixel(float v, float dpi_scale) noexcept
{
    return static_cast<int>(v * dpi_scale);
}

// DIP → ピクセル変換（切り上げ）。ウィンドウサイズのはみ出し防止等に使う。
inline int DipToPixelCeil(float v, float dpi_scale) noexcept
{
    return static_cast<int>(std::ceil(v * dpi_scale));
}

// ピクセル → DIP 変換。Win32 メッセージ座標を DIP 座標系に戻す際に使う。
constexpr float PixelToDip(float v, float dpi_scale) noexcept
{
    return v / dpi_scale;
}

constexpr DipPoint PixelToDip(int px, int py, float dpi_scale) noexcept
{
    return { PixelToDip(static_cast<float>(px), dpi_scale), PixelToDip(static_cast<float>(py), dpi_scale) };
}

// 要素の右上を基準にオーバーレイボタン（コピー/保存/拡大表示）の矩形を返す。
// 図のボタンは右から 保存・コピー・拡大表示 の順に、間隔 COPY_BTN_GAP で並ぶ。
// anchor_right: 基準領域の右端, anchor_top: 基準領域の上端
inline D2D1_RECT_F OverlayButtonRect(float anchor_right, float anchor_top, OverlayButton kind) noexcept
{
    const float slot = kind == OverlayButton::DiagramView ? 2.0f : kind == OverlayButton::DiagramCopy ? 1.0f
                                                                                                      : 0.0f;
    const float bx = anchor_right - COPY_BTN_MARGIN - COPY_BTN_SIZE - slot * (COPY_BTN_SIZE + COPY_BTN_GAP);
    const float by = anchor_top + COPY_BTN_MARGIN;
    return D2D1::RectF(bx, by, bx + COPY_BTN_SIZE, by + COPY_BTN_SIZE);
}

// 画像・図は max_width を超える場合のみアスペクト比を保って縮小する (拡大はしない)。
// レイアウトの高さと描画矩形の両方がこの規則を使う。
constexpr D2D1_SIZE_F FitBitmapSize(float width, float height, float max_width) noexcept
{
    if (width > max_width && width > 0.0f) {
        return { max_width, height * (max_width / width) };
    }
    return { width, height };
}

// 図を描く矩形 (内容幅を超えるときだけ縮め、中央に寄せる)。描画・図のボタンの位置・ヒット判定で共有する。
inline D2D1_RECT_F DiagramRect(float diagram_w, float diagram_h, float x, float cw, float y) noexcept
{
    const auto draw = FitBitmapSize(diagram_w, diagram_h, cw);
    const float dx = x + (cw - draw.width) * 0.5f;
    return D2D1::RectF(dx, y, dx + draw.width, y + draw.height);
}
