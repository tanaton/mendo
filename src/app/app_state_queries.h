#pragma once
#include "app_state.h"
#include "block_h_scroll.h"

// reducer と App (Win32 層) の双方が AppState から導出するペイン/ブロック情報。
// 定義は reducer.cpp。

struct SidePaneContext {
    PaneScrollInfo info;
    ScrollState& scroll;
};
SidePaneContext GetSidePaneContext(AppState& state, PaneTarget pane);

// サイドペイン (rect) で dip_y にある項目の番号。無ければ -1。
inline int SidePaneItemAt(const AppState& state, PaneTarget pane, const PaneRect& rect, float dip_y)
{
    const float item_h = state.theme->pane_item_height;
    const float local_y = SidePaneLocalY(dip_y, PaneContentTop(rect, state.theme->pane_header_height), state.view.panes.SidePaneScroll(pane).scroll_y);
    return pane == PaneTarget::File
               ? state.file_explorer.HitTest(local_y, item_h)
               : state.document.doc.GetToc().HitTest(local_y, item_h);
}

// サイドペインヘッダーのボタン構成。Refresh/Reveal はファイルペインのみ。
struct SidePaneHeaderButtons {
    bool has_file_buttons = false;
    // 無効時は描画のみ行い、ヒットしない。
    bool reveal_enabled = false;
};

inline SidePaneHeaderButtons SidePaneHeaderButtonsFor(const AppState& state, PaneTarget target) noexcept
{
    const bool is_file = target == PaneTarget::File;
    return { .has_file_buttons = is_file, .reveal_enabled = is_file && state.document.doc.HasBackingFile() };
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
