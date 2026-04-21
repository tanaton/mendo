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
#include "theme_constants.h"
#include <string>
#include <string_view>
#include <memory_resource>

// スクロール位置のアンカー。レイアウト無効化の前に保存し、復元に使用する。
struct AnchorState {
    int idx = -1;
    float y_before = 0.0f;
    float offset = 0.0f;
};

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
    int hovered_copy_node = -1;
    int hovered_save_node = -1;
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

// アプリケーションの全状態を集約する構造体。
// Win32ハンドルは含まない。状態に加えて一部のコントローラ
// (ContextMenu, HitTestService, SearchBarController) を含む。
// Reducer パターンの入出力として使用する。
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
    HitTestService hit_test;
    int active_toc_index = -1;

    // ---- リロード管理 ----
    size_t reload_diff_pos = std::string_view::npos;
    bool pending_prefix_shrink = false;

    // ---- ペインレイアウトキャッシュ ----
    std::pmr::wstring cached_title_text = L"mendo";
    PaneLayout cached_pane_layout{};
    float cached_window_width_for_layout = 0.0f;
    bool pane_layout_valid = false;
};

// 現在のスクロール位置からアンカーを保存する。Reducer と App の共通ヘルパー。
AnchorState SaveAnchorFromState(const AppState& state) noexcept;

// 現在のファイル/スクロール位置をナビゲーション履歴に Push する。
void PushCurrentNavEntry(AppState& state);

// 現在のファイル/スクロール位置を NavEntry として返す（Push せず）。
NavEntry CurrentNavEntry(const AppState& state);
