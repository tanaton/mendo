#pragma once
#include "app_constants.h"
#include "pane_layout.h"
#include "ui_types.h"
#include "titlebar.h"
#include "tooltip.h"
#include "ui_constants.h"
#include <array>
#include <variant>
#include <string>
#include <cstdint>
#include <memory_resource>

// DPI 変更時に Win32 RECT から詰め替えて reducer に渡す。
struct PixelRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

struct KeyDownEvent {
    int key;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
};

struct MouseWheelEvent {
    int delta; // 生のWHEEL_DELTA単位
    bool ctrl = false;
    PaneZone zone = PaneZone::MdPane;
    uint64_t tick = 0;
};

enum class ScrollType : uint8_t {
    LineUp,
    LineDown,
    PageUp,
    PageDown,
    Home,
    End
};

struct KeyScrollAction {
    ScrollType type;
};

// 縦ホイールのスクロール。tick でスワイプ検出の軸ロックを更新する。
struct DirectScrollByAction {
    float delta;
    uint64_t tick = 0;
};

struct ScrollPaneAction {
    PaneZone pane;
    float delta;
    uint64_t tick = 0;
};

struct CopyClipboardAction {};
struct CopyFormattedClipboardAction {};
struct SelectAllAction {};
// ダブルクリック位置の単語を選択する。
struct SelectWordAction {
    int node_index;
    uint32_t text_pos;
};
struct ClearSelectionAction {};

struct TogglePaneAction {
    PaneTarget target;
};

enum class ZoomDirection : uint8_t {
    In,
    Out,
    Reset
};
struct ZoomAction {
    ZoomDirection direction;
    // 段数。速く回したホイールのノッチをまとめて 1 回で組み直す。Reset では使わない。
    int steps = 1;
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

// マウス位置でのホバー強調の全体。ViewState::hover が持ち、描画はここから読む。
// 既定値はどこにもホバーしていない状態。
struct HoverHighlights {
    TitleBarHitZone titlebar = TitleBarHitZone::None;
    // Input はホバー強調が無いので None と同じに扱う。
    SearchBarHitZone search_bar = SearchBarHitZone::None;
    NavDirection nav = NavDirection::None;
    OverlayButtonHit overlay_button{};
    // 横スクロールバーを出すブロック。-1 なら無し。
    int h_block = -1;
    // PaneTarget 順 (File, Toc)。
    std::array<int, 2> side_index{ -1, -1 };
    std::array<PaneHeaderButton, 2> side_button{ PaneHeaderButton::None, PaneHeaderButton::None };

    bool operator==(const HoverHighlights&) const = default;
};

// App がヒットテストで求めたホバー状態。reducer がまとめて状態へ反映する。
struct PointerHover : HoverHighlights {
    TooltipTarget tooltip;
};
struct PointerHoverAction {
    PointerHover hover;
};
// ウィンドウ外へ出た。ホバー状態を全て解除しツールチップを閉じる。
struct MouseLeaveAction {};
// edge: 右端のスプリッタを掴んだペイン。
struct SplitterDragStartedAction {
    PaneTarget edge;
};
// ドラッグ中のスプリッタは ViewState::drag、ウィンドウ幅は pane_layout_cache から読む。
struct SplitterDragMovedAction {
    float dip_x;
};
// テキスト選択以外の左ドラッグの終了。テキスト選択は終点で選択を確定する TextSelectionEndedAction。
struct LeftDragEndedAction {};
struct SearchInputDragStartedAction {
    int caret_pos;
};
struct SearchInputDragMovedAction {
    int caret_pos;
};
struct MdScrollbarDragStartedAction {
    float dip_y;
};
struct MdScrollbarDragMovedAction {
    float dip_y;
};
struct PaneScrollbarDragStartedAction {
    PaneTarget pane;
    float dip_y;
};
// ドラッグ中のペインは ViewState::drag から読む。
struct PaneScrollbarDragMovedAction {
    float dip_y;
};
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
struct FilePaneRevealCurrentFileAction {};
struct TocItemClickedAction {
    int node_index;
};
struct NavigateAnchorAction {
    std::pmr::string anchor_id;
};

// pending_restore があればそこへ、なければ先頭へスクロールする。
struct RestoreScrollAfterLoadAction {};
// ファイルロードが失敗した (RestoreScrollAfterLoadAction が来ない) ことの通知。
struct LoadFailedAction {};
struct HWheelAction {
    short delta;
    uint64_t tick;
};

struct BlockHScrollDragStartedAction {
    int node_index;
    float dip_x;
};
struct BlockHScrollDragMovedAction {
    float dip_x;
};
// D&D・ファイルペイン・ファイルダイアログから、履歴を積んでファイルを開く。
struct OpenPathAction {
    std::pmr::wstring path;
};

// ツールチップの同一判定は node / TOC の index で行うため、文書を差し替えたら対象を忘れる。
struct ClearTooltipAction {};

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

struct TimerAction {
    app_timer::Id timer_id;
};

struct SearchTextChangedAction {
    std::pmr::wstring text;
};
// 時間で区切った検索を、メッセージループの空き時間に 1 回分進める。
struct SearchContinueAction {};
struct ToggleCaseSensitiveAction {};
struct ToggleHighlightAction {};
struct SearchSelectionAction {
    int sel_start;
    int sel_end;
};
struct ImeCompositionAction {
    std::pmr::wstring text;
};

using AppAction = std::variant<
    NoOpAction,
    KeyScrollAction,
    DirectScrollByAction,
    ScrollPaneAction,
    CopyClipboardAction,
    CopyFormattedClipboardAction,
    SelectAllAction,
    SelectWordAction,
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
    PointerHoverAction,
    MouseLeaveAction,
    SplitterDragStartedAction,
    SplitterDragMovedAction,
    LeftDragEndedAction,
    SearchInputDragStartedAction,
    SearchInputDragMovedAction,
    MdScrollbarDragStartedAction,
    MdScrollbarDragMovedAction,
    PaneScrollbarDragStartedAction,
    PaneScrollbarDragMovedAction,
    TextSelectionStartedAction,
    TextSelectionMovedAction,
    TextSelectionEndedAction,
    RightClickGestureStartedAction,
    RightClickGestureMovedAction,
    RightClickGestureCompletedAction,
    FilePaneDirectoryClickedAction,
    FilePaneRevealCurrentFileAction,
    TocItemClickedAction,
    NavigateAnchorAction,
    RestoreScrollAfterLoadAction,
    LoadFailedAction,
    HWheelAction,
    BlockHScrollDragStartedAction,
    BlockHScrollDragMovedAction,
    OpenPathAction,
    ClearTooltipAction,
    ResizeAction,
    DpiChangedAction,
    ActivateAction,
    EnterSizeMoveAction,
    ExitSizeMoveAction,
    CaptureChangedAction,
    TimerAction,
    SearchTextChangedAction,
    SearchContinueAction,
    ToggleCaseSensitiveAction,
    ToggleHighlightAction,
    SearchSelectionAction,
    ImeCompositionAction>;
