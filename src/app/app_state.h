#pragma once
#include "document.h"
#include "layout_cache.h"
#include "viewport_manager.h"
#include "pane_controller.h"
#include "search_state.h"
#include "search_bar_controller.h"
#include "nav_history.h"
#include "file_explorer.h"
#include "mouse_gesture.h"
#include "swipe_detector.h"
#include "titlebar.h"
#include "toast_notifier.h"
#include "tooltip.h"
#include "scroll_restoration.h"
#include "context_menu.h"
#include "hit_test_service.h"
#include "hover_throttle.h"
#include "pane_layout.h"
#include <string>
#include <string_view>
#include <memory_resource>

// アプリケーションの全状態を集約する構造体。
// Win32ハンドルやサービスオブジェクトは含まない — 純粋な状態のみ。
// Reducer パターンの入出力として使用する。
struct AppState {
    // ---- ドメイン状態 ----
    Document doc;
    LayoutCache layout_cache;
    ViewportManager viewport;

    // ---- UI状態 ----
    PaneController panes;
    SearchState search_state;
    SearchBarController search_bar_ctrl;
    NavHistory nav_history;
    FileExplorer file_explorer;
    MouseGesture gesture;
    SwipeDetector swipe_detector;
    TitleBar titlebar;
    ToastNotifier toast;
    Tooltip tooltip;
    ScrollRestoration scroll_restore;
    ContextMenu ctx_menu;
    HitTestService hit_test;
    HoverThrottle hover_throttle;

    // ---- フラグ類 ----
    bool is_sizing = false;
    bool window_active = true;
    int hovered_copy_node = -1;
    int hovered_save_node = -1;
    int active_toc_index = -1;
    HitTestService::NavButtonHover nav_hover = HitTestService::NavButtonHover::None;

    // ---- リロード管理 ----
    size_t reload_diff_pos = std::string_view::npos;
    float reload_old_scroll = 0.0f;
    bool pending_prefix_shrink = false;

    // ---- キャッシュ ----
    std::pmr::wstring cached_title_text = L"mendo";
    mutable PaneLayout cached_pane_layout{};
    mutable float cached_window_width_for_layout = 0.0f;
    mutable bool pane_layout_valid = false;
};
