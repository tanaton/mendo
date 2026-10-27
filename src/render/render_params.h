#pragma once
// Renderer::Render の引数として渡す各種描画パラメータ構造体。
#include "block_h_scroll.h"
#include "document_types.h"
#include "layout_cache.h"
#include "file_explorer.h"
#include "titlebar.h"
#include "pane_layout.h"
#include "ui_types.h"
#include "search_state.h"
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

struct GesturePoint;

// App がマウスジェスチャーとスワイプから作る (ResolveGestureOverlay)。
struct GestureRenderState {
    const std::pmr::vector<GesturePoint>* trail_points = nullptr;
    NavDirection direction = NavDirection::None;
    bool trail_active = false;
};

// message が空なら非表示。
struct ToastRenderState {
    std::wstring_view message;
};

struct TitleBarRenderState {
    const TitleBar& bar;
    std::wstring_view title_text;
    float window_width = 0.0f;
    // ホバー中のボタン。ボタンごとに bool を持たせず一元管理する。
    TitleBarHitZone hovered_zone = TitleBarHitZone::None;
    bool search_active = false;
    bool file_pane_visible = false;
    bool toc_pane_visible = false;
    bool is_maximized = false;
    bool window_active = true;
};

// File / TOC ペインの対称な状態をまとめた値型。SidePaneState::panes[2] の要素。
struct SidePaneInstance {
    PaneRect rect{};
    ScrollState scroll{};
    int hovered_index = -1;
    bool show = false;
    PaneHeaderButton hovered_button = PaneHeaderButton::None;
    bool reveal_enabled = false; // TOC 側は常に false
    // 一覧の項目に掛ける倍率と、それを掛けた行の高さ (当たり判定と同じ SidePaneItemHeight)。
    float zoom = 1.0f;
    float item_height = 0.0f;
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

// 本文ペインの描画コマンドを作る入力 (CommandGenerator::GenerateMdPane)。
struct MdPaneParams {
    const std::pmr::vector<Node>& nodes;
    const LayoutCache& cache;
    const TextSelection& selection;
    PaneRect md_pane_rect;
    float scroll_y = 0.0f;
    OverlayButtonHit hovered;
    int hovered_diagram = -1;
    // ブロック単位の横スクロール。値で持ち、ポインタで AppState 側 map を参照する。
    BlockHScrollContext block_h_scroll;
    // 検索のハイライト。空なら描かない。
    std::span<const SearchMatch> search_matches;
    int current_match_index = -1;
    // マッチを引き直すたびに変わる。ノードが持つハイライト矩形のキャッシュをこれで捨てる。
    uint32_t search_generation = 0;
};

struct RenderParams {
    MdPaneParams md;
    const SidePaneState& side_panes;
    const TitleBarRenderState& titlebar;
    const GestureRenderState& gesture;
    const ToastRenderState& toast;
    const SearchBarRenderState& search_bar;
    NavDirection nav_hovered = NavDirection::None;
    bool can_go_back = false;
    bool can_go_forward = false;
    // 読み込み中のスピナーの角度。あれば本文ペイン・ナビボタン・検索バー・スクロールバーの代わりにスピナーを描く。
    std::optional<float> loading_angle;
};
