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

// ---- ドメイン状態: ドキュメントとレイアウトキャッシュ ----
struct DocumentState {
    Document doc;
    LayoutCache layout_cache;
};

// ---- 表示・スクロール・ペインの状態 ----
struct ViewState {
    ViewportManager viewport;
    PaneController panes;
    ScrollRestoration scroll_restore;
    NavHistory nav_history;
    float cached_total_height = 0.0f;
};

// ---- ユーザー入力・操作の状態 ----
struct InteractionState {
    MouseGesture gesture;
    SwipeDetector swipe_detector;
    HoverThrottle hover_throttle;
    Tooltip tooltip;
    ToastNotifier toast;
    HoveredButtons hovered;
    NavButtonHover nav_hover = NavButtonHover::None;
};

// ---- 検索の状態 ----
struct SearchGroup {
    SearchState search_state;
    SearchBarController search_bar_ctrl;
};

// ---- ウィンドウ・テーマの状態 ----
struct WindowState {
    TitleBar titlebar;
    bool is_sizing = false;
    bool window_active = true;
    float cached_dpi_scale = 1.0f;
    ThemeConstants cached_theme;
};

// アプリケーションの全状態を集約する構造体
struct AppState {
    // ---- サブグループ ----
    DocumentState document;
    ViewState view;
    InteractionState interaction;
    SearchGroup search;
    WindowState window;

    // ---- UIコンポーネント ----
    FileExplorer file_explorer;
    ContextMenu ctx_menu;
    int active_toc_index = -1;

    // ---- リロード管理 ----
    // wstring_view::npos と string_view::npos は同じ値（static_cast<size_t>(-1)）だが、
    // 意味的に wide テキストへのオフセットなので wstring_view 側で揃える。
    size_t reload_diff_pos = std::wstring_view::npos;
    // 短縮タイマーで再リロード予約済み (DeferPrefixShrink / partial-read race)。
    // ローディングアニメーションを抑制するために参照される。
    bool pending_reload_retry = false;

    // ---- ペインレイアウトキャッシュ ----
    std::pmr::wstring cached_title_text = L"mendo";
    PaneLayout cached_pane_layout{};
    float cached_window_width_for_layout = 0.0f;
    bool pane_layout_valid = false;
};

ScrollTarget SnapshotVisibleTarget(const AppState& state) noexcept;
NavEntry CurrentNavEntry(const AppState& state);
void PushCurrentNavEntry(AppState& state);
