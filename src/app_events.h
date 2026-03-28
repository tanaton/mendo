#pragma once
#include "pane_layout.h"
#include <variant>
#include <vector>
#include <memory_resource>

// ──── イベント (プラットフォーム非依存のユーザー入力) ────

struct KeyDownEvent {
    int key;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
};

struct MouseWheelEvent {
    int delta;                          // 生のWHEEL_DELTA単位
    bool ctrl = false;
    PaneZone zone = PaneZone::MdPane;
};

// ──── アクション (アプリが実行すべき操作) ────

enum class ScrollType { LineUp, LineDown, PageUp, PageDown, Home, End };

// キーボード/スクロールバースクロール (Shellがtypeから具体的なdeltaを算出)
struct KeyScrollAction { ScrollType type; };

// マウスホイールのスムーズスクロール (事前計算済みdelta)
struct SmoothScrollByAction { float delta; };

// ペイン (ファイル/目次) スクロール
struct ScrollPaneAction { PaneZone pane; float delta; };

struct CopyClipboardAction {};
struct SelectAllAction {};
struct ClearSelectionAction {};

// サイドペインの切り替え
struct TogglePaneAction { bool file_pane; };   // true = ファイル, false = 目次

// ズーム: -1 = 縮小, 0 = リセット, +1 = 拡大
struct ZoomAction { int direction; };

struct ReloadFileAction {};
struct OpenFileAction {};
struct ToggleDarkModeAction {};
struct NavigateBackAction {};
struct NavigateForwardAction {};
struct ShowHelpAction {};

using AppAction = std::variant<
    KeyScrollAction,
    SmoothScrollByAction,
    ScrollPaneAction,
    CopyClipboardAction,
    SelectAllAction,
    ClearSelectionAction,
    TogglePaneAction,
    ZoomAction,
    ReloadFileAction,
    OpenFileAction,
    ToggleDarkModeAction,
    NavigateBackAction,
    NavigateForwardAction,
    ShowHelpAction
>;

using ActionList = std::pmr::vector<AppAction>;
