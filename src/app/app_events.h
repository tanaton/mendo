#pragma once
#include "pane_layout.h"
#include "ui_types.h"
#include "pane_controller.h"
#include "tooltip.h"
#include <variant>
#include <string>
#include <cstdint>
#include <memory_resource>
#include <optional>

// プラットフォーム非依存のピクセル矩形。App::OnDpiChanged 境界で
// Win32 の RECT から詰め替えて reducer に渡す。
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

struct DirectScrollByAction {
    float delta;
};

struct ScrollPaneAction {
    PaneZone pane;
    float delta;
};

struct CopyClipboardAction {};
struct CopyFormattedClipboardAction {};
struct SelectAllAction {};
struct ClearSelectionAction {};

enum class PaneTarget : uint8_t {
    File,
    Toc
};
struct TogglePaneAction {
    PaneTarget target;
};

// PaneZone (座標ヒット判定の結果) を、サイドペイン操作の対象を表す
// PaneTarget へ変換する。File/Toc 以外の領域 (None / Splitter / MdPane)
// は対象外なので nullopt を返す。
constexpr std::optional<PaneTarget> ToPaneTarget(PaneZone zone) noexcept
{
    switch (zone) {
    case PaneZone::FilePane:
        return PaneTarget::File;
    case PaneZone::TocPane:
        return PaneTarget::Toc;
    default:
        return std::nullopt;
    }
}

constexpr PaneZone ToPaneZone(PaneTarget target) noexcept
{
    return target == PaneTarget::File ? PaneZone::FilePane : PaneZone::TocPane;
}

enum class ZoomDirection : uint8_t {
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

struct MouseLeaveAction {};
struct MdPaneNavHoverAction {
    NavButtonHover nav_hover;
};
struct MdPaneButtonHoverChangedAction {
    HoveredButtons hovered;
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

// target.IsEmpty() なら非表示要求。px/py はクライアント座標。
struct UpdateTooltipAction {
    TooltipTarget target;
    int px;
    int py;
};

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
struct DestroyAction {};

struct TimerAction {
    uintptr_t timer_id;
};
struct FileWatchAction {};
struct ParseCompleteAction {};
struct ImageLoadedAction {};

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

using AppAction = std::variant<
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
    ResizeAction,
    DpiChangedAction,
    ActivateAction,
    EnterSizeMoveAction,
    ExitSizeMoveAction,
    CaptureChangedAction,
    DestroyAction,
    TimerAction,
    FileWatchAction,
    ParseCompleteAction,
    ImageLoadedAction,
    SearchTextChangedAction,
    ToggleCaseSensitiveAction,
    ToggleHighlightAction,
    SearchSelectionAction,
    ImeCompositionAction>;
