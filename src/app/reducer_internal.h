#pragma once
#include "app_state.h"
#include "app_events.h"
#include "block_h_scroll.h"
#include "side_effect.h"

// reducer.cpp の std::visit ディスパッチから各ドメイン (reducer_scroll.cpp /
// reducer_input.cpp / reducer_navigation.cpp / reducer_ui.cpp / reducer_system.cpp)
// の Reduce* 関数を呼ぶための内部宣言。reducer_internal.h は app 内専用で、
// 公開 API (reducer.h) には含めない。

// ---- 複数ドメイン横断の共有ヘルパー (定義は reducer.cpp) ----

void ClearTooltip(AppState& state, SideEffectList& effects);
void ClearSidePaneHoverState(AppState& state, SideEffectList& effects);

void EmitScrollChangedSideEffects(AppState& state, SideEffectList& effects);
void EmitScrollEffects(AppState& state, SideEffectList& effects, float old_scroll);
void ApplyScrollTargetAndEmit(AppState& state, SideEffectList& effects, int node, float offset);

// つまみ上クリック → 位置維持 (オフセットのみ記録)、つまみ外 → 中心へジャンプ。
struct ScrollbarDragGrip {
    float drag_offset;
    bool inside_thumb;
};
ScrollbarDragGrip ComputeScrollbarDragGrip(float thumb_y, float thumb_height, float click_y) noexcept;

struct SidePaneContext {
    const PaneRect& rect;
    float total_content;
    PaneScrollInfo info;
    ScrollState& scroll;
    PaneController::DragTarget drag_target;
    PaneZone pane_zone;
};
PaneController::DragTarget SidePaneDragTarget(PaneTarget pane) noexcept;
SidePaneContext GetSidePaneContext(AppState& state, PaneTarget pane);

BlockHScrollGeometry ResolveBlockHScrollGeometry(const AppState& state, int node_index) noexcept;
float MdScrollableContentHeight(const AppState& state) noexcept;
bool ApplyBlockHScrollDelta(AppState& state, int node_index, float new_value, float scroll_max);

// ---- スクロール (reducer_scroll.cpp) ----

void ReduceKeyScroll(AppState& state, SideEffectList& effects, const KeyScrollAction& a);
void ReduceDirectScrollBy(AppState& state, SideEffectList& effects, const DirectScrollByAction& a);
void ReduceScrollPane(AppState& state, SideEffectList& effects, const ScrollPaneAction& a);
void ReduceHWheel(AppState& state, SideEffectList& effects, const HWheelAction& a);
void ReduceBlockHHoverChanged(AppState& state, SideEffectList& effects, const BlockHHoverChangedAction& a);
void ReduceBlockHScrollDragStarted(AppState& state, SideEffectList& effects, const BlockHScrollDragStartedAction& a);
void ReduceBlockHScrollDragMoved(AppState& state, SideEffectList& effects, const BlockHScrollDragMovedAction& a);
void ReduceBlockHScrollDragEnded(AppState& state, SideEffectList& effects, const BlockHScrollDragEndedAction& a);

// ---- 検索 (reducer_search.cpp) ----

void ReduceSearchStep(AppState& state, bool forward);

// ---- 入力・マウス (reducer_input.cpp) ----

void ReduceCaptureChanged(AppState& state, SideEffectList& effects);
void ReduceMdPaneNavHover(AppState& state, SideEffectList& effects, const MdPaneNavHoverAction& a);
void ReduceMdPaneButtonHoverChanged(AppState& state, SideEffectList& effects, const MdPaneButtonHoverChangedAction& a);
void ReduceSplitterDragStarted(AppState& state, SideEffectList& effects, const SplitterDragStartedAction& a);
void ReduceSplitterDragMoved(AppState& state, SideEffectList& effects, const SplitterDragMovedAction& a);
void ReduceSplitterDragEnded(AppState& state, SideEffectList& effects);
void ReduceSearchInputDragStarted(AppState& state, SideEffectList& effects, const SearchInputDragStartedAction& a);
void ReduceSearchInputDragMoved(AppState& state, SideEffectList& effects, const SearchInputDragMovedAction& a);
void ReduceSearchInputDragEnded(AppState& state, SideEffectList& effects);
void ReduceMdScrollbarDragStarted(AppState& state, SideEffectList& effects, const MdScrollbarDragStartedAction& a);
void ReduceMdScrollbarDragMoved(AppState& state, SideEffectList& effects, const MdScrollbarDragMovedAction& a);
void ReduceMdScrollbarDragEnded(AppState& state, SideEffectList& effects);
void ReducePaneScrollbarDragStarted(AppState& state, SideEffectList& effects, const PaneScrollbarDragStartedAction& a);
void ReducePaneScrollbarDragMoved(AppState& state, SideEffectList& effects, const PaneScrollbarDragMovedAction& a);
void ReducePaneScrollbarDragEnded(AppState& state, SideEffectList& effects);
void ReduceTextSelectionStarted(AppState& state, SideEffectList& effects, const TextSelectionStartedAction& a);
void ReduceTextSelectionMoved(AppState& state, SideEffectList& effects, const TextSelectionMovedAction& a);
void ReduceTextSelectionEnded(AppState& state, SideEffectList& effects, const TextSelectionEndedAction& a);
void ReduceRightClickGestureStarted(AppState& state, SideEffectList& effects, const RightClickGestureStartedAction& a);
void ReduceRightClickGestureMoved(AppState& state, SideEffectList& effects, const RightClickGestureMovedAction& a);
void ReduceRightClickGestureCompleted(AppState& state, SideEffectList& effects, const RightClickGestureCompletedAction& a);

// ---- ナビゲーション・ファイル操作 (reducer_navigation.cpp) ----

void ReduceNavigateBack(AppState& state, SideEffectList& effects);
void ReduceNavigateForward(AppState& state, SideEffectList& effects);
void ScrollToAnchor(AppState& state, SideEffectList& effects, std::string_view anchor_id);
void ScrollToNormalizedAnchor(AppState& state, SideEffectList& effects, std::string_view anchor_id);
void ReduceFilePaneDirectoryClicked(AppState& state, SideEffectList& effects, const FilePaneDirectoryClickedAction& a);
void ReduceFilePaneFileClicked(AppState& state, SideEffectList& effects, const FilePaneFileClickedAction& a);
void ReduceTocItemClicked(AppState& state, SideEffectList& effects, const TocItemClickedAction& a);
void ReduceNavigateAnchor(AppState& state, SideEffectList& effects, const NavigateAnchorAction& a);
void ReduceRestoreScrollAfterLoad(AppState& state, SideEffectList& effects, const RestoreScrollAfterLoadAction& a);
void ReduceDropFiles(AppState& state, SideEffectList& effects, const DropFilesAction& a);
void ReduceShowHelp(AppState& state, SideEffectList& effects);

// ---- UI・選択・クリップボード (reducer_ui.cpp) ----

void ReduceTogglePane(AppState& state, SideEffectList& effects, const TogglePaneAction& a);
void ReduceSelectAll(AppState& state, SideEffectList& effects);
void ReduceClearSelection(AppState& state, SideEffectList& effects);
void ReduceCopyClipboard(const AppState& state, SideEffectList& effects);
void ReduceCopyFormattedClipboard(const AppState& state, SideEffectList& effects);

// ---- システム・ウィンドウ・テーマ (reducer_system.cpp) ----

void ReduceZoom(AppState& state, SideEffectList& effects, const ZoomAction& a);
void ReduceToggleDarkMode(AppState& state, SideEffectList& effects);
void ReduceActivate(AppState& state, SideEffectList& effects, const ActivateAction& a);
void ReduceResize(AppState& state, SideEffectList& effects, const ResizeAction& a);
void ReduceDpiChanged(AppState& state, SideEffectList& effects, const DpiChangedAction& a);
void ReduceTimer(AppState& state, SideEffectList& effects, const TimerAction& a);
