#pragma once
// Renderer::Render の引数として渡す各種描画パラメータ構造体。
#include "types.h"
#include "layout_cache.h"
#include "file_explorer.h"
#include "toc.h"
#include "titlebar.h"
#include "pane.h"
#include "mouse_gesture.h"
#include <d2d1.h>
#include <wrl/client.h>
#include <memory_resource>
#include <string_view>

struct GestureRenderState {
    const std::pmr::deque<GesturePoint>* trail_points = nullptr;
    int direction = 0;   // -1=Left(戻る), 1=Right(進む)
    float overlay_alpha = 0.0f;
    bool trail_active = false;
    bool overlay_visible = false;
};

struct ToastRenderState {
    bool visible = false;
    float alpha = 0.0f;
    std::wstring_view message;
};

// タイトルバー描画パラメータ。
struct TitleBarRenderState {
    // --- 8バイトアライメント ---
    std::wstring_view title_text;
    // --- TitleBarButton (D2D1_RECT_F + bool) ---
    TitleBarButton open_file;
    TitleBarButton help;
    TitleBarButton theme_toggle;
    TitleBarButton search;
    TitleBarButton file_toggle;
    TitleBarButton toc_toggle;
    TitleBarButton minimize;
    TitleBarButton maximize;
    TitleBarButton close;
    // --- 4バイトアライメント ---
    D2D1_RECT_F icon_rect{};
    D2D1_RECT_F title_text_rect{};
    float height = 0.0f;
    float window_width = 0.0f;
    // --- 1バイトアライメント ---
    bool is_dark_mode = false;
    bool search_active = false;
    bool file_pane_visible = false;
    bool toc_pane_visible = false;
    bool is_maximized = false;
    bool window_active = true;
};

// サイドペイン描画パラメータを一つの構造体にまとめたもの。
struct SidePaneState {
    // --- 8バイトアライメント (参照 = ポインタ) ---
    const PaneRect& file_pane_rect;
    const PaneRect& toc_pane_rect;
    const std::pmr::vector<FileEntry>& file_entries;
    const ScrollState& file_scroll;
    const std::pmr::vector<TocEntry>& toc_entries;
    const std::pmr::vector<Node>& nodes; // TocEntryからのテキスト参照用
    const ScrollState& toc_scroll;
    // --- 4バイトアライメント ---
    int hovered_file_index;
    int hovered_toc_index;
    int active_toc_index;
    // --- 1バイトアライメント ---
    bool show_file_pane;
    bool show_toc_pane;
    bool file_close_hovered;
    bool file_refresh_hovered;
    bool toc_close_hovered;
};

// ペインビットマップキャッシュ — サイドペインはオフスクリーンビットマップに描画され、
// 内容が変更された場合のみ再描画される。
struct PaneCache {
    Microsoft::WRL::ComPtr<ID2D1BitmapRenderTarget> bitmap_rt;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> cached_bitmap; // GetBitmap() の毎フレーム呼び出しを回避
    bool dirty = true;
    float cached_width = 0;
    float cached_height = 0;

    constexpr void Invalidate() noexcept { dirty = true; }
    void Reset() noexcept { bitmap_rt.Reset(); cached_bitmap.Reset(); dirty = true; cached_width = 0; cached_height = 0; }
};

// 検索バー描画パラメータ
struct SearchBarRenderState {
    // --- 8バイトアライメント ---
    std::wstring_view query;
    std::wstring_view ime_composition; // IME変換中のコンポジション文字列
    // --- 4バイトアライメント ---
    int current_match = -1;    // 0-based、-1 = マッチなし
    int total_matches = 0;
    int caret_pos = -1;         // キャレット位置（-1 = テキスト末尾）
    int selection_start = -1;   // 選択開始位置（caret_posと異なる場合、選択範囲あり）
    // --- 1バイトアライメント ---
    bool visible = false;
    bool has_focus = false;
    bool caret_visible = false; // キャレット（点滅制御）
    // チェックボックス状態
    bool case_sensitive = false;
    bool highlight_enabled = true;
    // 検索バー内のボタンホバー状態
    bool up_btn_hovered = false;
    bool down_btn_hovered = false;
    bool close_btn_hovered = false;
    bool case_btn_hovered = false;
    bool highlight_btn_hovered = false;
};

// Renderer::Render に渡す全パラメータをまとめた構造体。
struct RenderParams {
    // --- 8バイトアライメント (参照 = ポインタ) ---
    const std::pmr::vector<Node>& nodes;
    const LayoutCache& cache;
    const TextSelection& selection;
    const PaneRect& md_pane_rect;
    const SidePaneState& side_panes;
    const TitleBarRenderState& titlebar;
    const GestureRenderState& gesture;
    const ToastRenderState& toast;
    const SearchBarRenderState& search_bar;
    // --- 4バイトアライメント ---
    float scroll_y = 0.0f;
    float total_content_height = 0.0f;
    int nav_hovered = 0;
    int hovered_copy_node = -1;
    int hovered_save_node = -1;
    // --- 1バイトアライメント ---
    bool can_go_back = false;
    bool can_go_forward = false;
    bool has_dirty_nodes = false;
};
