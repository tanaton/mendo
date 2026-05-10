#pragma once
#include "document.h"
#include "layout_cache.h"
#include "viewport_manager.h"
#include "pane_controller.h"
#include "search_state.h"
#include "search_bar_controller.h"
#include "nav.h"
#include "file_explorer.h"
#include "mouse_gesture.h"
#include "swipe_detector.h"
#include "titlebar.h"
#include "toast_notifier.h"
#include "tooltip.h"
#include "ui_types.h"
#include "context_menu.h"
#include "hover_throttle.h"
#include "pane_layout.h"
#include "theme.h"
#include <string>
#include <string_view>
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

    float GetBlockScrollX(int node_index) const
    {
        const auto it = block_scroll_x.find(node_index);
        return (it != block_scroll_x.end()) ? it->second : 0.0f;
    }
};

struct InteractionState {
    MouseGesture gesture;
    SwipeDetector swipe_detector;
    HoverThrottle hover_throttle;
    Tooltip tooltip;
    ToastNotifier toast;
    HoveredButtons hovered;
    NavButtonHover nav_hover = NavButtonHover::None;
};

struct SearchGroup {
    SearchState search_state;
    SearchBarController search_bar_ctrl;
};

struct WindowState {
    TitleBar titlebar;
    bool is_sizing = false;
    bool window_active = true;
    float cached_dpi_scale = 1.0f;
};

// アプリケーションの全状態を集約する構造体。
struct AppState {
    DocumentState document;
    ViewState view;
    InteractionState interaction;
    SearchGroup search;
    WindowState window;

    // Renderer が所有する Theme への非所有参照。App::InitializeRenderer 後に設定され、
    // Theme 変更時 (renderer_.SetTheme 経由) は内部値が in-place 更新されるためポインタは
    // 安定。reducer 系が Y 位置計算に必要な spacing_above 等を参照するために使う。
    const Theme* theme = nullptr;

    FileExplorer file_explorer;
    ContextMenu ctx_menu;
    int active_toc_index = -1;

    // 差分位置は UTF-8 byte offset (CalcScrollYForDiff の string_view 引数と同じドメイン)。
    // npos が「未設定」のセンチネル。
    size_t reload_diff_pos = std::string_view::npos;
    // 短縮タイマーで再リロード予約済み（DeferPrefixShrink / partial-read race）。
    // ローディングアニメーションを抑制するために参照される。
    bool pending_reload_retry = false;

    std::pmr::wstring cached_title_text = L"mendo";
    PaneLayoutCache pane_layout_cache;
};

ScrollTarget SnapshotVisibleTarget(const AppState& state) noexcept;
NavEntry CurrentNavEntry(const AppState& state);
void PushCurrentNavEntry(AppState& state);
