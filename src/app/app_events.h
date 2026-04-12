#pragma once
#include "pane_layout.h"
#include <variant>
#include <string>
#include <memory_resource>
#include <windows.h>

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

// ──── アクション: コマンド系 (アプリが実行すべき操作) ────

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

// ──── アクション: マウスイベント系 ────

struct LButtonDownAction { float dip_x; float dip_y; int px; int py; };
struct LButtonUpAction { float dip_x; float dip_y; int px; int py; };
struct MouseMoveAction { float dip_x; float dip_y; int px; int py; };
struct MouseHoverAction { float dip_x; float dip_y; int px; int py; };
struct LButtonDblClkAction { float dip_x; float dip_y; int px; int py; };
struct RButtonDownAction { float dip_x; float dip_y; int px; int py; };
struct RButtonUpAction { float dip_x; float dip_y; int px; int py; };
struct RButtonMoveAction { float dip_x; float dip_y; int px; int py; };
struct ContextMenuAction { int screen_x; int screen_y; };
struct MouseLeaveAction {};
struct XButtonBackAction {};
struct XButtonForwardAction {};
struct HWheelAction { short delta; };
struct DropFilesAction { std::pmr::wstring path; };

// ──── アクション: システムイベント系 ────

struct ResizeAction { UINT width; UINT height; };
struct DpiChangedAction { UINT dpi; RECT suggested; };
struct ActivateAction { bool active; };
struct EnterSizeMoveAction {};
struct ExitSizeMoveAction {};
struct CaptureChangedAction {};
struct DestroyAction {};

// ──── アクション: タイマー・非同期コールバック系 ────

struct TimerAction { UINT_PTR timer_id; };
struct FileWatchAction {};
struct ParseCompleteAction {};
struct ImageLoadedAction {};

// ──── アクション: 検索系 ────

struct SearchTextChangedAction { std::pmr::wstring text; };
struct ToggleCaseSensitiveAction {};
struct ToggleHighlightAction {};
struct SearchSelectionAction { int sel_start; int sel_end; };
struct ImeCompositionAction { std::pmr::wstring text; };

// ──── AppAction: 全アクションの統一型 ────

using AppAction = std::variant<
    // コマンド系
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
    SearchPrevAction,
    // マウスイベント系
    LButtonDownAction,
    LButtonUpAction,
    MouseMoveAction,
    MouseHoverAction,
    LButtonDblClkAction,
    RButtonDownAction,
    RButtonUpAction,
    RButtonMoveAction,
    ContextMenuAction,
    MouseLeaveAction,
    XButtonBackAction,
    XButtonForwardAction,
    HWheelAction,
    DropFilesAction,
    // システムイベント系
    ResizeAction,
    DpiChangedAction,
    ActivateAction,
    EnterSizeMoveAction,
    ExitSizeMoveAction,
    CaptureChangedAction,
    DestroyAction,
    // タイマー・非同期系
    TimerAction,
    FileWatchAction,
    ParseCompleteAction,
    ImageLoadedAction,
    // 検索系
    SearchTextChangedAction,
    ToggleCaseSensitiveAction,
    ToggleHighlightAction,
    SearchSelectionAction,
    ImeCompositionAction
>;

