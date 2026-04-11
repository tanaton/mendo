#pragma once
#include "pane_layout.h"
#include <variant>

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

// ホイール/タッチパッドの直接スクロール (アニメーションなし)
struct DirectScrollByAction { float delta; };

// ペイン (ファイル/目次) スクロール
struct ScrollPaneAction { PaneZone pane; float delta; };

struct CopyClipboardAction {};
struct SelectAllAction {};
struct ClearSelectionAction {};

// サイドペインの切り替え
enum class PaneTarget { File, Toc };
struct TogglePaneAction { PaneTarget target; };

// ズーム操作
enum class ZoomDirection { In, Out, Reset };
struct ZoomAction { ZoomDirection direction; };

struct ReloadFileAction {};
struct OpenFileAction {};
struct ToggleDarkModeAction {};
struct NavigateBackAction {};
struct NavigateForwardAction {};
struct ShowHelpAction {};
struct OpenSearchBarAction {};
struct CloseSearchBarAction {};
struct SearchNextAction {};
struct SearchPrevAction {};
struct NoOpAction {};

using AppAction = std::variant<
    NoOpAction,
    KeyScrollAction,
    DirectScrollByAction,
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
    ShowHelpAction,
    OpenSearchBarAction,
    CloseSearchBarAction,
    SearchNextAction,
    SearchPrevAction
>;

