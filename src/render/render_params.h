#pragma once
// Renderer::Render の引数として渡す各種描画パラメータ構造体。
#include "block_h_scroll.h"
#include "document_types.h"
#include "layout_cache.h"
#include "file_explorer.h"
#include "toc.h"
#include "titlebar.h"
#include "pane_layout.h"
#include "ui_types.h"
#include "mouse_gesture.h"
#include "swipe_detector.h"
#include <memory_resource>
#include <string_view>
#include <utility>

struct GestureRenderState {
    const std::pmr::vector<GesturePoint>* trail_points = nullptr;
    NavDirection direction = NavDirection::None;
    bool trail_active = false;
};

inline GestureRenderState ResolveGestureOverlay(const MouseGesture& gesture, const SwipeDetector& swipe) noexcept
{
    GestureRenderState gs;
    gs.trail_active = gesture.IsGestureActive();
    gs.trail_points = &gesture.GetTrailPoints();
    // マウスジェスチャーを優先する。None ならオーバーレイは出さない。
    gs.direction = gesture.IsOverlayVisible() ? gesture.GetDirection() : swipe.GetOverlayDirection();
    return gs;
}

// message が空なら非表示。
struct ToastRenderState {
    std::wstring_view message;
};

struct TitleBarRenderState {
    std::wstring_view title_text;
    TitleBarButtons buttons{};
    DipRect icon_rect{};
    DipRect title_text_rect{};
    float height = 0.0f;
    float window_width = 0.0f;
    // ホバー中のボタン。ボタンごとに bool を持たせず一元管理する。
    TitleBarHitZone hovered_zone = TitleBarHitZone::None;
    bool is_dark_mode = false;
    bool search_active = false;
    bool file_pane_visible = false;
    bool toc_pane_visible = false;
    bool is_maximized = false;
    bool window_active = true;

    constexpr const DipRect& Button(TitleBarHitZone zone) const noexcept
    {
        return TitleBarButtonRect(buttons, zone);
    }
};

// File / TOC ペインの対称な状態をまとめた値型。SidePaneState::panes[2] の要素。
struct SidePaneInstance {
    PaneRect rect{};
    ScrollState scroll{};
    int hovered_index = -1;
    bool show = false;
    PaneHeaderButton hovered_button = PaneHeaderButton::None;
    bool reveal_enabled = false; // TOC 側は常に false
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

struct SearchBarRenderState {
    std::wstring_view query;
    std::wstring_view ime_composition;
    int current_match = -1; // 0-based、-1 = マッチなし
    int total_matches = 0;    // -1 = 見せる結果がまだ無い (初めての検索の途中)
    int caret_pos = -1;       // キャレット位置（-1 = テキスト末尾）
    int selection_start = -1; // 選択開始位置（caret_posと異なる場合、選択範囲あり）
    bool visible = false;
    bool caret_visible = false; // 非アクティブ時は隠す。点滅させない (点滅のたびに全画面再描画が走る)
    // チェックボックス状態
    bool case_sensitive = false;
    bool highlight_enabled = true;
    SearchBarHitZone hovered = SearchBarHitZone::None;
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
    NavDirection nav_hovered = NavDirection::None;
    OverlayButtonHit hovered;
    bool can_go_back = false;
    bool can_go_forward = false;
    bool has_dirty_nodes = false;
    // ブロック単位の横スクロール。値で持ち、ポインタで AppState 側 map を参照する。
    BlockHScrollContext block_h_scroll;
};
