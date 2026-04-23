#pragma once
#include "pane_layout.h"
#include "nav_button.h"
#include "pane_controller.h"
#include "tooltip_target.h"
#include <variant>
#include <string>
#include <cstdint>
#include <memory_resource>

// プラットフォーム非依存のピクセル矩形。App::OnDpiChanged 境界で
// Win32 の RECT から各フィールドの値をコピーして生成する。
struct PixelRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

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

enum class ScrollType {
    LineUp,
    LineDown,
    PageUp,
    PageDown,
    Home,
    End
};

// キーボード/スクロールバースクロール (Shellがtypeから具体的なdeltaを算出)
struct KeyScrollAction {
    ScrollType type;
};

// ホイール/タッチパッドの直接スクロール (アニメーションなし)
struct DirectScrollByAction {
    float delta;
};

// ペイン (ファイル/目次) スクロール
struct ScrollPaneAction {
    PaneZone pane;
    float delta;
};

struct CopyClipboardAction {};
struct CopyFormattedClipboardAction {};
struct SelectAllAction {};
struct ClearSelectionAction {};

// サイドペインの切り替え
enum class PaneTarget {
    File,
    Toc
};
struct TogglePaneAction {
    PaneTarget target;
};

// ズーム操作
enum class ZoomDirection {
    In,
    Out,
    Reset
};
struct ZoomAction {
    ZoomDirection direction;
};

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

struct MouseLeaveAction {};
struct MdPaneNavHoverAction {
    NavButtonHover nav_hover;
};
struct MdPaneButtonHoverChangedAction {
    int hovered_copy_node;
    int hovered_save_node;
};
struct SplitterDragStartedAction {
    PaneController::DragTarget target;
};
struct SplitterDragMovedAction {
    PaneController::DragTarget target;
    float dip_x;
    float window_width;
};
struct SplitterDragEndedAction {};
struct SearchInputDragStartedAction {
    int caret_pos;
};
struct SearchInputDragMovedAction {
    int caret_pos;
};
struct SearchInputDragEndedAction {};
struct MdScrollbarDragStartedAction {
    float dip_y;
    float total_height;
};
struct MdScrollbarDragMovedAction {
    float dip_y;
    float total_height;
};
struct MdScrollbarDragEndedAction {};
struct PaneScrollbarDragStartedAction {
    PaneTarget pane;
    float dip_y;
};
struct PaneScrollbarDragMovedAction {
    PaneTarget pane;
    float dip_y;
};
struct PaneScrollbarDragEndedAction {};
struct TextSelectionStartedAction {
    int node_index;
    uint32_t text_pos;
    int click_x;
    int click_y;
};
struct TextSelectionMovedAction {
    int node_index;
    uint32_t text_pos;
};
struct TextSelectionEndedAction {
    int end_node_index;
    uint32_t end_text_pos;
};
struct RightClickGestureStartedAction {
    float dip_x;
    float dip_y;
};
struct RightClickGestureMovedAction {
    float dip_x;
    float dip_y;
};
struct RightClickGestureCompletedAction {
    int screen_x;
    int screen_y;
};
struct FilePaneDirectoryClickedAction {
    std::pmr::wstring full_path;
};
struct FilePaneFileClickedAction {
    std::pmr::wstring full_path;
};
struct TocItemClickedAction {
    std::pmr::wstring anchor_id;
};
// リンクからのアンカーナビゲーション（アンカー ID / スラグ）。
struct NavigateAnchorAction {
    std::pmr::wstring anchor_id;
};

// ファイルロード完了直後のスクロール位置復元。
// reload_diff（内容差分による自動スクロール）が優先、次に pending_restore_node（履歴復帰）、最後に先頭。
struct RestoreScrollAfterLoadAction {
    bool has_reload_diff;
    float reload_diff_scroll_y;
};
struct HWheelAction {
    short delta;
    uint64_t tick;
};
struct DropFilesAction {
    std::pmr::wstring path;
};

// ツールチップ更新（ホバー対象が変わった時に発行）。
// target.IsEmpty() なら非表示要求。px/py はクライアント座標。
struct UpdateTooltipAction {
    TooltipTarget target;
    int px;
    int py;
};

// ツールチップを即時クリア（タイマー停止 + 状態リセット）。
struct ClearTooltipAction {};

// ──── アクション: システムイベント系 ────

struct ResizeAction {
    uint32_t width;
    uint32_t height;
};
struct DpiChangedAction {
    uint32_t dpi;
    PixelRect suggested;
};
struct ActivateAction {
    bool active;
};
struct EnterSizeMoveAction {};
struct ExitSizeMoveAction {};
struct CaptureChangedAction {};
struct DestroyAction {};

// ──── アクション: タイマー・非同期コールバック系 ────

struct TimerAction {
    uintptr_t timer_id;
};
struct FileWatchAction {};
struct ParseCompleteAction {};
struct ImageLoadedAction {};

// ──── アクション: 検索系 ────

struct SearchTextChangedAction {
    std::pmr::wstring text;
};
struct ToggleCaseSensitiveAction {};
struct ToggleHighlightAction {};
struct SearchSelectionAction {
    int sel_start;
    int sel_end;
};
struct ImeCompositionAction {
    std::pmr::wstring text;
};

// ──── AppAction: 全アクションの統一型 ────

using AppAction = std::variant<
    // コマンド系
    NoOpAction,
    KeyScrollAction,
    DirectScrollByAction,
    ScrollPaneAction,
    CopyClipboardAction,
    CopyFormattedClipboardAction,
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
    MouseLeaveAction,
    MdPaneNavHoverAction,
    MdPaneButtonHoverChangedAction,
    SplitterDragStartedAction,
    SplitterDragMovedAction,
    SplitterDragEndedAction,
    SearchInputDragStartedAction,
    SearchInputDragMovedAction,
    SearchInputDragEndedAction,
    MdScrollbarDragStartedAction,
    MdScrollbarDragMovedAction,
    MdScrollbarDragEndedAction,
    PaneScrollbarDragStartedAction,
    PaneScrollbarDragMovedAction,
    PaneScrollbarDragEndedAction,
    TextSelectionStartedAction,
    TextSelectionMovedAction,
    TextSelectionEndedAction,
    RightClickGestureStartedAction,
    RightClickGestureMovedAction,
    RightClickGestureCompletedAction,
    FilePaneDirectoryClickedAction,
    FilePaneFileClickedAction,
    TocItemClickedAction,
    NavigateAnchorAction,
    RestoreScrollAfterLoadAction,
    HWheelAction,
    DropFilesAction,
    UpdateTooltipAction,
    ClearTooltipAction,
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

