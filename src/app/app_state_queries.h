#pragma once
#include "app_state.h"
#include "block_h_scroll.h"

// reducer と App (Win32 層) の双方が AppState から導出するペイン/ブロック情報。
// 定義は reducer.cpp。

struct SidePaneContext {
    const PaneRect& rect;
    float total_content;
    PaneScrollInfo info;
    ScrollState& scroll;
    PaneController::DragTarget drag_target;
    PaneZone pane_zone;
};
constexpr PaneController::DragTarget SidePaneDragTarget(PaneTarget pane) noexcept
{
    using enum PaneController::DragTarget;
    return (pane == PaneTarget::File) ? FileScrollbar : TocScrollbar;
}
SidePaneContext GetSidePaneContext(AppState& state, PaneTarget pane);

BlockHScrollGeometry ResolveBlockHScrollGeometry(const AppState& state, int node_index) noexcept;
float MdScrollableContentHeight(const AppState& state) noexcept;
