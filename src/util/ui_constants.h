#pragma once
#include "ui_types.h"
#include <d2d1.h>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

inline constexpr D2D1_COLOR_F SELECTION_COLOR = { 0.26f, 0.56f, 0.84f, 0.3f };

inline constexpr float TWO_PI = std::numbers::pi_v<float> * 2.0f;

// ローディングスピナーの定数
namespace spinner {
inline constexpr float RADIUS = 20.0f;
inline constexpr float DOT_RADIUS = 3.0f;
inline constexpr int DOT_COUNT = 8;
inline constexpr float ROTATION_INCREMENT = 0.15f;
inline constexpr float DOT_FADE_FACTOR = 0.85f;
} // namespace spinner

// Windowsの基準DPI（100%スケーリング時の値）
inline constexpr float DEFAULT_DPI = 96.0f;

// dpi が 0 以下なら 1.0 (DPI 未初期化時のフォールバック)、それ以外は dpi / 96。
constexpr float DpiScaleFrom(float dpi) noexcept
{
    return (dpi > 0.0f) ? (dpi / DEFAULT_DPI) : 1.0f;
}

inline constexpr float MOUSE_WHEEL_SCROLL_MULTIPLIER = 0.8f;

inline constexpr float TOC_INDENT_PER_LEVEL = 12.0f;

// テーブル描画で共有するレイアウト定数
inline constexpr float TABLE_CELL_PADDING = 8.0f;
inline constexpr float TABLE_BORDER_WIDTH = 1.0f;

// 画像/ダイアグラム未デコード時の placeholder 最小高さ (DIP)。
inline constexpr float MIN_DIAGRAM_PLACEHOLDER_HEIGHT = 60.0f;
// 深いネストでもノードに残す本文幅 (DIP)。
inline constexpr float MIN_NODE_CONTENT_WIDTH = 64.0f;

// width から inset (余白・インデント) を引いた本文幅。引きすぎても MIN_NODE_CONTENT_WIDTH は残す
// (width 自体がそれより狭ければ width)。負の幅ではテキストレイアウトを作れず、画像は原寸デコードになる。
constexpr float ShrinkKeepingMinContent(float width, float inset) noexcept
{
    return std::max(width - inset, std::min(width, MIN_NODE_CONTENT_WIDTH));
}

// テーブルストライプ（偶数行背景）のアルファ値
inline constexpr float TABLE_STRIPE_ALPHA_DARK = 0.05f;
inline constexpr float TABLE_STRIPE_ALPHA_LIGHT = 0.02f;

// 横ホイール 1 ノッチあたりの DIP スクロール量。
// Shift+Wheel / WM_MOUSEHWHEEL の delta (WHEEL_DELTA = 120 単位) を DIP に換算する。
inline constexpr float HSCROLL_DIP_PER_NOTCH = 60.0f;

// 縦スクロールバーの左端 X。right_edge はペイン右端 (ローカル/スクリーンどちらの系でも可)。
// 描画 (renderer_pane) とヒットテスト (app_mouse_*) で同じ式を共有する。
constexpr float VScrollbarLeftX(float right_edge) noexcept
{
    return right_edge - PANE_SCROLLBAR_WIDTH - PANE_SCROLLBAR_MARGIN;
}

// 縦スクロールバーのヒット判定の X 範囲。細いバーを掴みやすいよう左に広げ、右端の余白は外す
// (md ペインはウィンドウ右端に接するので、そこはリサイズ枠に譲る)。
constexpr bool InVScrollbarHitX(float x, float right_edge) noexcept
{
    return x >= VScrollbarLeftX(right_edge) - PANE_SCROLLBAR_HIT_PADDING && x <= right_edge - PANE_SCROLLBAR_MARGIN;
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

// ナビゲーションオーバーレイボタンの定数（DIP単位）。
// レンダラー（描画）とhit_test_service（クリック検出）の間で共有される。
inline constexpr float NAV_BTN_SIZE = 32.0f;
inline constexpr float NAV_BTN_MARGIN = 16.0f;
inline constexpr float NAV_BTN_GAP = 2.0f;
inline constexpr float NAV_BTN_CORNER = 6.0f;
inline constexpr float NAV_BTN_SCROLLBAR_OFFSET = 16.0f;

// コードブロック コピーボタンの定数（DIP単位）。
// CommandGenerator（描画）と hit_test（クリック検出）の間で共有される。
inline constexpr float COPY_BTN_SIZE = 28.0f;
inline constexpr float COPY_BTN_MARGIN = 6.0f;
inline constexpr float COPY_BTN_CORNER = 4.0f;
inline constexpr float COPY_BTN_GAP = 4.0f;

// 背景を行の高さから上下に縮める量。行送り 24 DIP で背景の高さと隙間を GitHub と同じ 21 / 3 DIP にする。
inline constexpr float INLINE_CODE_INSET_Y = 1.5f;
// コード範囲の両端に足す字間で、背景の左右パディングになる。背景はこの字間までに留め、
// GitHub と同じく前後の空白を背景の外にまるごと残す。
inline constexpr float INLINE_CODE_SPACING = 5.0f;
inline constexpr float INLINE_CODE_CORNER = 6.0f;
inline constexpr float CODE_BLOCK_CORNER = 4.0f;
// 本文の行送りをフォントサイズの何倍にするか (GitHub の line-height: 1.5)。
// 行頭に重ねて描くリスト番号・チェックボックスも同じ行送りにしてベースラインを揃える。
inline constexpr float BODY_LINE_HEIGHT_FACTOR = 1.5f;

inline constexpr float LIST_BULLET_X_FACTOR = 0.6f;
// 円と同じ直径の四角は面積が 4/π 倍あって重く見えるので、半辺を半径より少し小さくする。
inline constexpr float LIST_SQUARE_HALF_FACTOR = 0.85f;
inline constexpr float LIST_NUMBER_PAD_LEFT = 4.0f;
inline constexpr float LIST_NUMBER_PAD_RIGHT = 8.0f;

inline constexpr float TASK_CHECKBOX_HEIGHT_FACTOR = 1.5f;
inline constexpr float TABLE_ROW_HEIGHT_FACTOR = 1.4f;

// DirectWriteメトリクス取得失敗時の代替値
inline constexpr float FALLBACK_LINE_HEIGHT_FACTOR = 1.3f;

inline constexpr UINT TOOLTIP_DELAY_MS = 500;

// クリック判定距離の二乗（ドラッグ選択とクリックを区別するための閾値）
inline constexpr int CLICK_DISTANCE_THRESHOLD_SQ = 25;

inline constexpr float PANE_CLOSE_BTN_MARGIN = 2.0f;

// 無効状態の UI (ペインヘッダーボタン・メニュー項目) の不透明度
inline constexpr float DISABLED_UI_ALPHA = 0.35f;

// ペインヘッダー内の閉じるボタン矩形を返す（ペインローカル座標）。
inline D2D1_RECT_F PaneCloseButtonRect(float pane_width, float header_height) noexcept
{
    const float btn_size = header_height - 2.0f * PANE_CLOSE_BTN_MARGIN;
    const float btn_x = pane_width - btn_size - PANE_CLOSE_BTN_MARGIN;
    const float btn_y = (header_height - btn_size) / 2.0f;
    return D2D1::RectF(btn_x, btn_y, btn_x + btn_size, btn_y + btn_size);
}

inline D2D1_RECT_F PaneButtonLeftOf(const D2D1_RECT_F& right) noexcept
{
    const float btn_size = right.right - right.left;
    const float btn_x = right.left - btn_size - PANE_CLOSE_BTN_MARGIN;
    return D2D1::RectF(btn_x, right.top, btn_x + btn_size, right.top + btn_size);
}

// ペインヘッダー内の更新ボタン矩形を返す（閉じるボタンの左隣、ペインローカル座標）。
inline D2D1_RECT_F PaneRefreshButtonRect(float pane_width, float header_height) noexcept
{
    return PaneButtonLeftOf(PaneCloseButtonRect(pane_width, header_height));
}

// ペインヘッダー内の「現在のファイルの場所へ移動」ボタン矩形を返す（更新ボタンの左隣、ペインローカル座標）。
inline D2D1_RECT_F PaneRevealButtonRect(float pane_width, float header_height) noexcept
{
    return PaneButtonLeftOf(PaneRefreshButtonRect(pane_width, header_height));
}

// ペインヘッダーの見出し矩形（ペインローカル座標）。ウィンドウが狭くサイド幅が縮小されると
// ボタンだけで埋まり矩形が反転するため、その場合は nullopt (見出しを描かない)。
inline std::optional<D2D1_RECT_F> PaneHeaderTextRect(float pane_width, float header_height, bool with_file_buttons) noexcept
{
    constexpr float TEXT_LEFT = 8.0f;
    constexpr float BUTTON_GAP = 4.0f;
    const D2D1_RECT_F leftmost_button = with_file_buttons ? PaneRevealButtonRect(pane_width, header_height) : PaneCloseButtonRect(pane_width, header_height);
    const float right = leftmost_button.left - BUTTON_GAP;
    if (right <= TEXT_LEFT) {
        return std::nullopt;
    }
    return D2D1::RectF(TEXT_LEFT, 0.0f, right, header_height);
}

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

// タイトルバーのテキストフォントサイズ（DIP）。
// 本文ズームは Markdown 本文だけを対象にするので、窓枠側の寸法は固定値で持つ。
inline constexpr float TITLEBAR_TEXT_FONT_SIZE = 13.0f;
// タイトルバーと検索バーのアイコンのフォントサイズ（DIP）。
inline constexpr float UI_ICON_FONT_SIZE = 14.0f;

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

// 検索バーのフォントサイズ（DIP）。枠の寸法は本文ズームに追従しないので、
// 文字も固定値にして枠からのはみ出しを防ぐ。
inline constexpr float SEARCH_INPUT_FONT_SIZE = 13.0f;
inline constexpr float SEARCH_COUNT_FONT_SIZE = SEARCH_INPUT_FONT_SIZE * 0.9f;

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
    l.bar_bottom = md_rect.y + md_rect.height;
    l.bar_top = l.bar_bottom - SEARCH_BAR_HEIGHT;

    const float center_y = l.bar_top + (SEARCH_BAR_HEIGHT - SEARCH_INPUT_HEIGHT) / 2.0f;
    const float center_bottom = center_y + SEARCH_INPUT_HEIGHT;

    // 左から順に幅 w の要素を詰めて置く。
    float x = bar_left + SEARCH_BAR_PADDING;
    const auto place = [&x](float w, float top, float bottom) noexcept {
        const D2D1_RECT_F r = D2D1::RectF(x, top, x + w, bottom);
        x += w + SEARCH_BAR_GAP;
        return r;
    };
    const auto place_btn = [&] { return place(SEARCH_BTN_SIZE, center_y, center_bottom); };

    l.icon_rect = place(SEARCH_BTN_SIZE, l.bar_top, l.bar_bottom);
    l.input_rect = place(std::min(SEARCH_INPUT_MAX_WIDTH, (bar_right - bar_left) * 0.5f), center_y, center_bottom);
    l.up_btn = place_btn();
    l.down_btn = place_btn();
    if (has_query) {
        l.count_rect = place(SEARCH_MATCH_COUNT_WIDTH, l.bar_top, l.bar_bottom);
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
    if (y < sbl.bar_top) {
        return SearchBarHitZone::None;
    }
    const struct {
        const D2D1_RECT_F& rect;
        SearchBarHitZone zone;
    } targets[] = {
        { sbl.up_btn, SearchBarHitZone::Up },
        { sbl.down_btn, SearchBarHitZone::Down },
        { sbl.case_btn, SearchBarHitZone::CaseSensitive },
        { sbl.highlight_btn, SearchBarHitZone::Highlight },
        { sbl.close_btn, SearchBarHitZone::Close },
        { sbl.input_rect, SearchBarHitZone::Input },
    };
    for (const auto& t : targets) {
        if (PointInRect(x, y, t.rect)) {
            return t.zone;
        }
    }
    return SearchBarHitZone::None;
}

// デフォルトウィンドウサイズ（ピクセル）
inline constexpr int DEFAULT_WINDOW_WIDTH = 1600;
inline constexpr int DEFAULT_WINDOW_HEIGHT = 900;

// 検索ボックス IME コンポジションウィンドウのキャレット位置 X オフセット (px)。
// EM_POSFROMCHAR が返す位置はキャレットの左端なので、フォント幅の半分強を足してキャレット直下に表示する。
inline constexpr LONG IME_CARET_X_OFFSET = 8;

// マークダウンペインの最小幅 (DIP)。スプリッタードラッグでこれ以上 md ペインを縮められない。
// PaneController の表示幅の縮小とスプリッタードラッグの制約で共有する。
inline constexpr float MD_PANE_MIN_WIDTH = 200.0f;

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

// 要素の右上を基準にオーバーレイボタン（コピー/保存）の矩形を返す。
// 図のボタンは右から 保存・コピー の順に、間隔 COPY_BTN_GAP で並ぶ。
// anchor_right: 基準領域の右端, anchor_top: 基準領域の上端
inline D2D1_RECT_F OverlayButtonRect(float anchor_right, float anchor_top, OverlayButton kind) noexcept
{
    const float slot = kind == OverlayButton::DiagramCopy ? 1.0f : 0.0f;
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

// Mermaidダイアグラムのビットマップ描画矩形を計算する（スケーリング＋中央寄せ）。
// CommandGenerator（描画）と hit_test（クリック検出）の間で共有される。
inline D2D1_RECT_F MermaidBitmapRect(float diagram_w, float diagram_h, float x, float cw, float y) noexcept
{
    const auto draw = FitBitmapSize(diagram_w, diagram_h, cw);
    const float dx = x + (cw - draw.width) * 0.5f;
    return D2D1::RectF(dx, y, dx + draw.width, y + draw.height);
}
