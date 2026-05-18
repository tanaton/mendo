#pragma once
// Renderer::Render の引数として渡す各種描画パラメータ構造体。
#include "block_h_scroll_context.h"
#include "document_types.h"
#include "layout_cache.h"
#include "file_explorer.h"
#include "toc.h"
#include "titlebar.h"
#include "pane_layout.h"
#include "ui_types.h"
#include "mouse_gesture.h"
#include <d2d1.h>
#include <wrl/client.h>
#include <bit>
#include <cstddef>
#include <memory_resource>
#include <string_view>
#include <utility>

struct GestureRenderState {
    const std::pmr::deque<GesturePoint>* trail_points = nullptr;
    int direction = 0; // -1=Left(戻る), 1=Right(進む)
    float overlay_alpha = 0.0f;
    bool trail_active = false;
    bool overlay_visible = false;
};

struct ToastRenderState {
    bool visible = false;
    float alpha = 0.0f;
    std::wstring_view message;
};

struct TitleBarRenderState {
    std::wstring_view title_text;
    TitleBarButton open_file;
    TitleBarButton help;
    TitleBarButton theme_toggle;
    TitleBarButton search;
    TitleBarButton file_toggle;
    TitleBarButton toc_toggle;
    TitleBarButton minimize;
    TitleBarButton maximize;
    TitleBarButton close;
    DipRect icon_rect{};
    DipRect title_text_rect{};
    float height = 0.0f;
    float window_width = 0.0f;
    // ホバー中のボタン。各 TitleBarButton に bool を持たせず一元管理する。
    TitleBarHitZone hovered_zone = TitleBarHitZone::None;
    bool is_dark_mode = false;
    bool search_active = false;
    bool file_pane_visible = false;
    bool toc_pane_visible = false;
    bool is_maximized = false;
    bool window_active = true;
};

// DipRect は D2D1_RECT_F とメンバ順・サイズ・アライメントが同一であることを
// 静的に保証し、std::bit_cast で安全にビット等価変換する。
static_assert(sizeof(DipRect) == sizeof(D2D1_RECT_F));
static_assert(alignof(DipRect) == alignof(D2D1_RECT_F));
static_assert(offsetof(DipRect, left) == offsetof(D2D1_RECT_F, left));
static_assert(offsetof(DipRect, top) == offsetof(D2D1_RECT_F, top));
static_assert(offsetof(DipRect, right) == offsetof(D2D1_RECT_F, right));
static_assert(offsetof(DipRect, bottom) == offsetof(D2D1_RECT_F, bottom));

inline D2D1_RECT_F ToD2DRect(const DipRect& r) noexcept
{
    return std::bit_cast<D2D1_RECT_F>(r);
}

// File / TOC ペインの対称な状態をまとめた値型。SidePaneState::panes[2] の要素。
struct SidePaneInstance {
    PaneRect rect{};
    ScrollState scroll{};
    int hovered_index = -1;
    bool show = false;
    bool close_hovered = false;
    bool refresh_hovered = false; // TOC 側は常に false
};

struct SidePaneState {
    SidePaneInstance panes[2];
    const std::pmr::vector<FileEntry>& file_entries;
    const std::pmr::vector<TocEntry>& toc_entries;
    const std::pmr::vector<Node>& nodes; // TocEntryからのテキスト参照用
    int active_toc_index;

    constexpr const SidePaneInstance& Get(PaneTarget t) const noexcept
    {
        return panes[std::to_underlying(t)];
    }
};

// ペインビットマップキャッシュ — サイドペインはオフスクリーンビットマップに描画され、
// 内容が変更された場合のみ再描画される。
struct PaneCache {
    Microsoft::WRL::ComPtr<ID2D1BitmapRenderTarget> bitmap_rt;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> cached_bitmap; // GetBitmap() の毎フレーム呼び出しを回避
    bool dirty = true;
    float cached_width = 0;
    float cached_height = 0;
    float cached_scroll_y = 0;

    constexpr void Invalidate() noexcept
    {
        dirty = true;
    }
    constexpr bool NeedsRedraw(float current_scroll_y) const noexcept
    {
        return dirty || cached_scroll_y != current_scroll_y;
    }
    void Reset() noexcept
    {
        bitmap_rt.Reset();
        cached_bitmap.Reset();
        dirty = true;
        cached_width = 0;
        cached_height = 0;
        cached_scroll_y = 0;
    }
};

struct SearchBarRenderState {
    std::wstring_view query;
    std::wstring_view ime_composition; // IME変換中のコンポジション文字列
    int current_match = -1; // 0-based、-1 = マッチなし
    int total_matches = 0;
    int caret_pos = -1;       // キャレット位置（-1 = テキスト末尾）
    int selection_start = -1; // 選択開始位置（caret_posと異なる場合、選択範囲あり）
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

struct RenderParams {
    const std::pmr::vector<Node>& nodes;
    const LayoutCache& cache;
    const TextSelection& selection;
    const PaneRect& md_pane_rect;
    const SidePaneState& side_panes;
    const TitleBarRenderState& titlebar;
    const GestureRenderState& gesture;
    const ToastRenderState& toast;
    const SearchBarRenderState& search_bar;
    float scroll_y = 0.0f;
    float total_content_height = 0.0f;
    int nav_hovered = 0;
    HoveredButtons hovered;
    bool can_go_back = false;
    bool can_go_forward = false;
    bool has_dirty_nodes = false;
    // ブロック単位の横スクロール。値で持ち、ポインタで AppState 側 map を参照する。
    BlockHScrollContext block_h_scroll;
};
