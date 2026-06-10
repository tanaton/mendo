#include "reducer_internal.h"
#include "layout_computer.h"

void ReduceCaptureChanged(AppState& state, SideEffectList& effects)
{
    state.search.search_bar_ctrl.OnCaptureChanged();
    bool invalidate = false;
    if (state.interaction.gesture.GetPhase() != GesturePhase::Idle) {
        state.interaction.gesture.Reset();
        invalidate = true;
    }
    // キャプチャ喪失時 (Alt+Tab・他アプリの SetCapture 等) は WM_LBUTTONUP が
    // 届かないため、進行中の全ドラッグ状態をここで解除する
    if (state.view.viewport.IsDragging()) {
        state.view.viewport.SetDragging(false);
        invalidate = true;
    }
    if (state.view.viewport.IsScrollbarTracking()) {
        state.view.viewport.SetScrollbarTracking(false);
        invalidate = true;
    }
    if (state.view.panes.GetDragTarget() != PaneController::DragTarget::None) {
        state.view.panes.EndDrag();
        invalidate = true;
    }
    if (state.view.h_drag_node >= 0) {
        state.view.h_drag_node = -1;
        invalidate = true;
    }
    if (invalidate) {
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ReduceMdPaneNavHover(AppState& state, SideEffectList& effects, const MdPaneNavHoverAction& a)
{
    if (state.interaction.nav_hover == a.nav_hover) {
        return;
    }
    state.interaction.nav_hover = a.nav_hover;
    // ナビボタンホバー時はコピー/保存/SVG コピーのホバーをクリアし、ホバーが二重に
    // 表示されないようにする。
    if (a.nav_hover != NavButtonHover::None) {
        state.interaction.hovered = HoveredButtons{};
    }
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceMdPaneButtonHoverChanged(AppState& state, SideEffectList& effects, const MdPaneButtonHoverChangedAction& a)
{
    if (state.interaction.hovered == a.hovered) {
        return;
    }
    state.interaction.hovered = a.hovered;
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceSplitterDragStarted(AppState& state, SideEffectList& effects, const SplitterDragStartedAction& a)
{
    if (!PaneController::IsSplitterDragTarget(a.target)) {
        return;
    }
    state.view.panes.StartDrag(a.target);
    PushEffect(effects, effect::SetCapture{});
}

void ReduceSplitterDragMoved(AppState& state, SideEffectList& effects, const SplitterDragMovedAction& a)
{
    if (!PaneController::IsSplitterDragTarget(a.target)) {
        return;
    }
    const float splitter_w = state.theme->splitter_width;
    const float before_file = state.view.panes.GetSidePaneWidth(PaneTarget::File);
    const float before_toc = state.view.panes.GetSidePaneWidth(PaneTarget::Toc);
    state.view.panes.DragSplitterTo(a.target, a.dip_x, a.window_width, splitter_w);
    if (state.view.panes.GetSidePaneWidth(PaneTarget::File) == before_file && state.view.panes.GetSidePaneWidth(PaneTarget::Toc) == before_toc) {
        return;
    }
    state.pane_layout_cache.Invalidate();
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceSplitterDragEnded(AppState& state, SideEffectList& effects)
{
    const auto drag = state.view.panes.GetDragTarget();
    if (!PaneController::IsSplitterDragTarget(drag)) {
        return;
    }
    state.view.panes.EndDrag();
    state.pane_layout_cache.Invalidate();
    PushEffect(effects, effect::ReleaseCapture{});
    PushEffect(effects, effect::PerformResizeEnd{});
}

void ReduceSearchInputDragStarted(AppState& state, SideEffectList& effects, const SearchInputDragStartedAction& a)
{
    state.search.search_bar_ctrl.StartDrag(a.caret_pos);
    PushEffect(effects, effect::SetCapture{});
    PushEffect(
        effects,
        effect::SearchFocus{ effect::SearchFocus::Mode::SetCaret, 0, a.caret_pos });
}

void ReduceSearchInputDragMoved(AppState& state, SideEffectList& effects, const SearchInputDragMovedAction& a)
{
    if (!state.search.search_bar_ctrl.IsDragging()) {
        return;
    }
    const auto& ctrl = state.search.search_bar_ctrl;
    if (a.caret_pos == ctrl.GetCaretPos() && ctrl.GetDragAnchor() == ctrl.GetSelectionStart()) {
        return;
    }
    PushEffect(
        effects,
        effect::SearchFocus{
            effect::SearchFocus::Mode::SetSelection,
            ctrl.GetDragAnchor(),
            a.caret_pos,
        });
}

void ReduceSearchInputDragEnded(AppState& state, SideEffectList& effects)
{
    state.search.search_bar_ctrl.EndDrag();
    PushEffect(effects, effect::ReleaseCapture{});
}

void ReduceMdScrollbarDragStarted(AppState& state, SideEffectList& effects, const MdScrollbarDragStartedAction& a)
{
    const auto& md_rect = state.pane_layout_cache.Get().md_rect;
    const auto info = ComputeScrollInfo(md_rect, 0.0f, MdScrollableContentHeight(state));
    const float thumb_y = ComputeThumbY(info, state.view.viewport.GetScrollY());
    const auto grip = ComputeScrollbarDragGrip(thumb_y, info.thumb_height, a.dip_y);
    const auto drag_offset = grip.drag_offset;

    // ドラッグ state の初期化は SetCapture より前に行う
    auto& sv = state.view;
    sv.panes.StartDrag(PaneController::DragTarget::MdScrollbar);
    sv.viewport.SetScrollbarTracking(true);
    sv.panes.SetDragScrollOffset(drag_offset);
    PushEffect(effects, effect::SetCapture{});
    // thumb 内クリックなら 1st jump は不要 (thumb-grip オフセット記録だけ)。
    if (!grip.inside_thumb) {
        const float old_scroll = sv.viewport.GetScrollY();
        sv.viewport.ScrollTo(ScrollFromThumbY(info, a.dip_y - drag_offset));
        EmitScrollEffects(state, effects, old_scroll);
    }
}

void ReduceMdScrollbarDragMoved(AppState& state, SideEffectList& effects, const MdScrollbarDragMovedAction& a)
{
    if (state.view.panes.GetDragTarget() != PaneController::DragTarget::MdScrollbar) {
        return;
    }
    const auto& md_rect = state.pane_layout_cache.Get().md_rect;
    const auto info = ComputeScrollInfo(md_rect, 0.0f, MdScrollableContentHeight(state));
    const float new_thumb_y = a.dip_y - state.view.panes.GetDragScrollOffset();
    const float old_scroll = state.view.viewport.GetScrollY();
    state.view.viewport.ScrollTo(ScrollFromThumbY(info, new_thumb_y));
    EmitScrollEffects(state, effects, old_scroll);
}

void ReduceMdScrollbarDragEnded(AppState& state, SideEffectList& effects)
{
    if (state.view.panes.GetDragTarget() != PaneController::DragTarget::MdScrollbar) {
        return;
    }
    state.view.viewport.SetScrollbarTracking(false);
    state.view.panes.EndDrag();
    PushEffect(effects, effect::ReleaseCapture{});
    PushEffect(effects, effect::PerformResizeEnd{});
    PushEffect(effects, effect::BitmapManage{});
}

void ReducePaneScrollbarDragStarted(AppState& state, SideEffectList& effects, const PaneScrollbarDragStartedAction& a)
{
    auto ctx = GetSidePaneContext(state, a.pane);
    if (ctx.total_content <= ctx.info.content_height) {
        return;
    }
    const float thumb_y = ComputeThumbY(ctx.info, ctx.scroll.scroll_y);
    const auto grip = ComputeScrollbarDragGrip(thumb_y, ctx.info.thumb_height, a.dip_y);
    const auto drag_offset = grip.drag_offset;

    // ドラッグ state の初期化は SetCapture より前に行う
    state.view.panes.StartDrag(ctx.drag_target);
    state.view.panes.SetDragScrollOffset(drag_offset);
    PushEffect(effects, effect::SetCapture{});
    if (!grip.inside_thumb) {
        ctx.scroll.scroll_y = ScrollFromThumbY(ctx.info, a.dip_y - drag_offset);
        PushEffect(effects, effect::InvalidatePaneCache{ ctx.pane_zone });
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ReducePaneScrollbarDragMoved(AppState& state, SideEffectList& effects, const PaneScrollbarDragMovedAction& a)
{
    // drag target は pane だけから決まるので、ctx 構築前に短絡し
    // 非ドラッグ時の per-event GetEntries().size() + ComputeScrollInfo を回避。
    if (state.view.panes.GetDragTarget() != SidePaneDragTarget(a.pane)) {
        return;
    }
    auto ctx = GetSidePaneContext(state, a.pane);
    const float new_thumb_y = a.dip_y - state.view.panes.GetDragScrollOffset();
    ctx.scroll.scroll_y = ScrollFromThumbY(ctx.info, new_thumb_y);
    PushEffect(effects, effect::InvalidatePaneCache{ ctx.pane_zone });
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReducePaneScrollbarDragEnded(AppState& state, SideEffectList& effects)
{
    using enum PaneController::DragTarget;
    const auto drag = state.view.panes.GetDragTarget();
    if (drag != FileScrollbar && drag != TocScrollbar) {
        return;
    }
    state.view.panes.EndDrag();
    PushEffect(effects, effect::ReleaseCapture{});
}

void ReduceTextSelectionStarted(AppState& state, SideEffectList& effects, const TextSelectionStartedAction& a)
{
    state.view.viewport.SetClickStart(a.click_x, a.click_y);
    if (a.node_index < 0) {
        return;
    }
    state.view.viewport.SetAnchor(a.node_index, a.text_pos);
    state.view.viewport.SetDragging(true);
    state.view.viewport.GetSelection().Clear();
    PushEffect(effects, effect::SetCapture{});
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceTextSelectionMoved(AppState& state, SideEffectList& effects, const TextSelectionMovedAction& a)
{
    if (!state.view.viewport.IsDragging() || a.node_index < 0) {
        return;
    }
    // WM_MOUSEMOVE は 16ms 周期で連発するため、選択が同値なら早期 return。
    const auto next = TextSelection::MakeOrdered(
        state.view.viewport.GetAnchorNode(),
        state.view.viewport.GetAnchorPos(),
        a.node_index,
        a.text_pos);
    if (next == state.view.viewport.GetSelection()) {
        return;
    }
    state.view.viewport.SetSelection(next);
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceTextSelectionEnded(AppState& state, SideEffectList& effects, const TextSelectionEndedAction& a)
{
    if (!state.view.viewport.IsDragging()) {
        return;
    }
    if (a.end_node_index >= 0) {
        state.view.viewport.SetSelection(TextSelection::MakeOrdered(
            state.view.viewport.GetAnchorNode(),
            state.view.viewport.GetAnchorPos(),
            a.end_node_index,
            a.end_text_pos));
    }
    state.view.viewport.SetDragging(false);
    PushEffect(effects, effect::ReleaseCapture{});
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceRightClickGestureStarted(AppState& state, SideEffectList& effects, const RightClickGestureStartedAction& a)
{
    state.interaction.gesture.OnRButtonDown(a.dip_x, a.dip_y);
    PushEffect(effects, effect::SetCapture{});
}

void ReduceRightClickGestureMoved(AppState& state, SideEffectList& effects, const RightClickGestureMovedAction& a)
{
    state.interaction.gesture.OnMouseMove(a.dip_x, a.dip_y);
    if (state.interaction.gesture.IsGestureActive()) {
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ReduceRightClickGestureCompleted(AppState& state, SideEffectList& effects, const RightClickGestureCompletedAction& a)
{
    if (state.interaction.gesture.GetPhase() == GesturePhase::Idle) {
        return;
    }
    const auto result = state.interaction.gesture.OnRButtonUp();
    PushEffect(effects, effect::ReleaseCapture{});
    switch (result) {
    case GestureResult::ShowContextMenu:
        state.interaction.gesture.Reset();
        PushEffect(effects, effect::ShowContextMenu{ a.screen_x, a.screen_y });
        break;
    case GestureResult::Back:
        ReduceNavigateBack(state, effects);
        break;
    case GestureResult::Forward:
        ReduceNavigateForward(state, effects);
        break;
    case GestureResult::None:
        break;
    }
    PushEffect(effects, effect::InvalidateWindow{});
}
