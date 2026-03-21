#pragma once
#include "pane.h"

// ペイン領域識別子（ある座標がどのペインに属するか）
enum class PaneZone : int {
    None,
    FilePane,
    Splitter1,
    TocPane,
    Splitter2,
    MdPane
};

// 計算済みペインレイアウト位置
struct PaneLayout {
    PaneRect file_rect{};
    PaneRect toc_rect{};
    PaneRect md_rect{};
};

// ペインのスクロール情報（スクロールバーの描画と操作に使用）
struct PaneScrollInfo {
    float content_top = 0.0f;
    float content_height = 0.0f;
    float total_content = 0.0f;
    float max_scroll = 0.0f;
    float thumb_height = 0.0f;
};

// ウィンドウサイズとペイン設定からペインレイアウトを計算する。
PaneLayout ComputePaneLayout(float total_width, float total_height,
                              float file_pane_width, float toc_pane_width,
                              float splitter_width, bool show_file, bool show_toc,
                              float md_min_width = 200.0f) noexcept;

// ある座標（DIP座標）がどのペイン領域に属するかを判定する。
PaneZone DetectPaneZone(float dip_x, const PaneLayout& layout,
                         float splitter_width, bool show_file, bool show_toc) noexcept;

// ペインのスクロール情報を計算する。スクロールバーの描画と操作に使用。
PaneScrollInfo ComputeScrollInfo(const PaneRect& rect, float header_height,
                                  float total_content, float thumb_min = PANE_SCROLLBAR_THUMB_MIN) noexcept;

// ドラッグ計算用にスクロールバーのつまみ位置を計算する。
// つまみ上端のY座標を返す。
float ComputeThumbY(const PaneScrollInfo& info, float scroll_y) noexcept;

// つまみのドラッグから新しいスクロール位置を計算する。
// 新しいつまみのY座標を指定する。
float ScrollFromThumbY(const PaneScrollInfo& info, float thumb_y) noexcept;
