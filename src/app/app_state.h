#pragma once
#include "block_h_scroll.h"
#include "document.h"
#include "layout_cache.h"
#include "viewport_manager.h"
#include "pane_controller.h"
#include "search_state.h"
#include "app_search_bar_callbacks.h"
#include "nav.h"
#include "file_explorer.h"
#include "mouse_gesture.h"
#include "swipe_detector.h"
#include "titlebar.h"
#include "toast_notifier.h"
#include "tooltip.h"
#include "ui_types.h"
#include "context_menu.h"
#include "last_hover_pos.h"
#include "pane_layout.h"
#include "theme.h"
#include <string>
#include <memory_resource>
#include <unordered_map>

struct DocumentState {
    Document doc;
    LayoutCache layout_cache;
};

struct ViewState {
    ViewportManager viewport;
    PaneController panes;
    ScrollRestoration scroll_restore;
    NavHistory nav_history;

    // ブロック単位の横スクロール。キーは LayoutCache のノードインデックス。
    // ファイルロード/リロードでクリア (永続化しない)。スクロールバーはホバー中とドラッグ中の
    // 対象のみ描画するため、map サイズはユーザー操作分に限定される。
    std::pmr::unordered_map<int, float> block_scroll_x;
    int hovered_h_block = -1;
    int h_drag_node = -1;
    float h_drag_start_x = 0.0f;
    float h_drag_start_scroll = 0.0f;
    int active_toc_index = -1;

    float GetBlockScrollX(int node_index, const BlockHScrollGeometry& geom) const noexcept
    {
        return LookupBlockScrollX(&block_scroll_x, node_index, geom);
    }

    // 再パースでノード/見出し index がずれると別の要素を指すため、リロード時にクリアする一時状態。
    void ResetPerNodeTransientState()
    {
        viewport.ClearSelection();
        block_scroll_x.clear();
        hovered_h_block = -1;
        h_drag_node = -1;
        active_toc_index = -1;
    }

    // 別文書 (ヘルプ含む) へ切り替える時のリセット。同一文書の再パースと違い
    // サイドペインのスクロールも先頭へ戻す。
    void ResetForNewDocument()
    {
        panes.ResetScrollStates();
        ResetPerNodeTransientState();
    }
};

struct InteractionState {
    MouseGesture gesture;
    SwipeDetector swipe_detector;
    LastHoverPos last_hover_pos;
    Tooltip tooltip;
    ToastNotifier toast;
    HoveredButtons hovered;
    NavButtonHover nav_hover = NavButtonHover::None;
    int zoom_wheel_accum = 0;
};

struct SearchGroup {
    SearchState search_state;
    SearchBarController search_bar_ctrl;
};

struct WindowState {
    TitleBar titlebar;
    bool is_sizing = false;
    // 移動だけのサイズ変更ループでは終了時のレイアウト処理を省くため。
    bool size_changed_in_sizing = false;
    bool window_active = true;
    float cached_dpi_scale = 1.0f;
    // 直近の WM_SIZE のクライアント領域 (物理 px)。同じ大きさの WM_SIZE を無視するために使う。
    uint32_t client_width = 0;
    uint32_t client_height = 0;
};

struct AppState {
    DocumentState document;
    ViewState view;
    InteractionState interaction;
    SearchGroup search;
    WindowState window;

    // Renderer が所有する Theme への非所有参照。App::Init で設定され、
    // Theme 変更時 (renderer_.SetTheme 経由) は内部値が in-place 更新されるためポインタは
    // 安定。reducer 系が Y 位置計算に必要な spacing_above 等を参照するために使う。
    const Theme* theme = nullptr;

    FileExplorer file_explorer;
    ContextMenu ctx_menu;

    std::pmr::wstring cached_title_text = L"mendo";
    PaneLayoutCache pane_layout_cache;
};

ScrollTarget SnapshotVisibleTarget(const AppState& state) noexcept;
NavEntry CurrentNavEntry(const AppState& state);
// load_path: これから開くファイル。現在と別のファイルなら、ロード失敗 (LoadFailedAction) で取り消す仮の履歴にする。
void PushCurrentNavEntry(AppState& state, std::wstring_view load_path = {});

// reducer と App (Win32 層) の双方が AppState から導出するペイン/ブロック情報。
// 非 inline 関数の定義は reducer.cpp。

struct SidePaneContext {
    PaneScrollInfo info;
    ScrollState& scroll;
};
SidePaneContext GetSidePaneContext(AppState& state, PaneTarget pane);

// サイドペイン (rect) で dip_y にある項目の番号。無ければ -1。
inline int SidePaneItemAt(const AppState& state, PaneTarget pane, const PaneRect& rect, float dip_y)
{
    const float item_h = state.theme->pane_item_height;
    const float local_y = SidePaneLocalY(dip_y, PaneContentTop(rect, state.theme->pane_header_height), state.view.panes.SidePaneScroll(pane).scroll_y);
    return pane == PaneTarget::File ? state.file_explorer.HitTest(local_y, item_h) : state.document.doc.GetToc().HitTest(local_y, item_h);
}

// サイドペインヘッダーのボタン構成。Refresh/Reveal はファイルペインのみ。
struct SidePaneHeaderButtons {
    bool has_file_buttons = false;
    // 無効時は描画のみ行い、ヒットしない。
    bool reveal_enabled = false;
};

inline SidePaneHeaderButtons SidePaneHeaderButtonsFor(const AppState& state, PaneTarget target) noexcept
{
    const bool is_file = target == PaneTarget::File;
    return { .has_file_buttons = is_file, .reveal_enabled = is_file && state.document.doc.HasBackingFile() };
}

// 左ボタンのドラッグ (テキスト選択・スプリッタ・各スクロールバー・ブロック横スクロール・検索入力) が進行中か。
inline bool IsLeftDragActive(const AppState& state) noexcept
{
    return state.view.viewport.IsDragging() ||
           state.view.panes.GetDragTarget() != PaneController::DragTarget::None ||
           state.view.h_drag_node >= 0 ||
           state.search.search_bar_ctrl.IsDragging();
}

BlockHScrollGeometry ResolveBlockHScrollGeometry(const AppState& state, int node_index) noexcept;
float MdScrollableContentHeight(const AppState& state) noexcept;
