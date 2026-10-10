#pragma once
#include "app_state.h"
#include "block_h_scroll.h"

// reducer と App (Win32 層) の双方が AppState から導出するペイン/ブロック情報。
// 定義は reducer.cpp。

struct SidePaneContext {
    PaneScrollInfo info;
    ScrollState& scroll;
};
constexpr PaneController::DragTarget SidePaneDragTarget(PaneTarget pane) noexcept
{
    using enum PaneController::DragTarget;
    return (pane == PaneTarget::File) ? FileScrollbar : TocScrollbar;
}
SidePaneContext GetSidePaneContext(AppState& state, PaneTarget pane);

inline int SidePaneHitTest(const AppState& state, PaneTarget pane, float local_y, float item_h)
{
    return pane == PaneTarget::File
               ? state.file_explorer.HitTest(local_y, item_h)
               : state.document.doc.GetToc().HitTest(local_y, item_h);
}

// 左ボタンのドラッグ (テキスト選択・スプリッタ・各スクロールバー・ブロック横スクロール・検索入力) が進行中か。
inline bool IsLeftDragActive(const AppState& state) noexcept
{
    return state.view.viewport.IsDragging() ||
           state.view.panes.GetDragTarget() != PaneController::DragTarget::None ||
           state.view.h_drag_node >= 0 ||
           state.search.search_bar_ctrl.IsDragging();
}

BlockHScrollGeometry ResolveBlockHScrollGeometry(const AppState& state, int node_index) noexcept;
float MdScrollableContentHeight(const AppState& state) noexcept;
