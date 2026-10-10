#include "reducer_internal.h"
#include "layout_computer.h"

namespace {

PaneScrollInfo MdScrollInfo(const AppState& state) noexcept
{
    return ComputeScrollInfo(state.pane_layout_cache.Get().md_rect, 0.0f, MdScrollableContentHeight(state));
}

// ドラッグ state の初期化は SetCapture より前に行う。
void BeginScrollbarDrag(AppState& state, SideEffectList& effects, PaneController::DragTarget target, float drag_offset)
{
    state.view.panes.StartDrag(target);
    state.view.panes.SetDragScrollOffset(drag_offset);
    PushEffect(effects, effect::SetCapture{});
}

} // namespace

void ReduceMouseLeave(AppState& state, SideEffectList& effects)
{
    state.interaction.last_hover_pos.Reset();
    ClearTooltip(state, effects);
    ClearSidePaneHoverState(state, effects);
}

void ReduceUpdateTooltip(const AppState& state, SideEffectList& effects, const UpdateTooltipAction& a)
{
    if (a.target.SameTarget(state.interaction.tooltip.GetCurrent())) {
        return;
    }
    PushEffect(effects, effect::ShowTooltip{ a.target });
}

void ReduceCaptureChanged(AppState& state, SideEffectList& effects)
{
    state.search.search_bar_ctrl.EndDrag();
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
    const auto& ctrl = state.search.search_bar_ctrl;
    if (!ctrl.IsDragging()) {
        return;
    }
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
    const auto info = MdScrollInfo(state);
    auto& viewport = state.view.viewport;
    const auto grip = ComputeScrollbarDragGrip(ComputeThumbY(info, viewport.GetScrollY()), info.thumb_height, a.dip_y);
    BeginScrollbarDrag(state, effects, PaneController::DragTarget::MdScrollbar, grip.drag_offset);
    // thumb 内クリックなら 1st jump は不要 (thumb-grip オフセット記録だけ)。
    if (!grip.inside_thumb) {
        const float old_scroll = viewport.GetScrollY();
        viewport.ScrollTo(ScrollFromThumbY(info, a.dip_y - grip.drag_offset));
        EmitScrollEffects(state, effects, old_scroll);
    }
}

void ReduceMdScrollbarDragMoved(AppState& state, SideEffectList& effects, const MdScrollbarDragMovedAction& a)
{
    if (state.view.panes.GetDragTarget() != PaneController::DragTarget::MdScrollbar) {
        return;
    }
    const float new_thumb_y = a.dip_y - state.view.panes.GetDragScrollOffset();
    const float old_scroll = state.view.viewport.GetScrollY();
    state.view.viewport.ScrollTo(ScrollFromThumbY(MdScrollInfo(state), new_thumb_y));
    EmitScrollEffects(state, effects, old_scroll);
}

void ReduceMdScrollbarDragEnded(AppState& state, SideEffectList& effects)
{
    if (state.view.panes.GetDragTarget() != PaneController::DragTarget::MdScrollbar) {
        return;
    }
    state.view.panes.EndDrag();
    PushEffect(effects, effect::ReleaseCapture{});
    PushEffect(effects, effect::PerformResizeEnd{});
    PushEffect(effects, effect::BitmapManage{});
}

void ReducePaneScrollbarDragStarted(AppState& state, SideEffectList& effects, const PaneScrollbarDragStartedAction& a)
{
    const auto ctx = GetSidePaneContext(state, a.pane);
    if (ctx.info.total_content <= ctx.info.content_height) {
        return;
    }
    const auto grip = ComputeScrollbarDragGrip(ComputeThumbY(ctx.info, ctx.scroll.scroll_y), ctx.info.thumb_height, a.dip_y);
    BeginScrollbarDrag(state, effects, SidePaneDragTarget(a.pane), grip.drag_offset);
    if (!grip.inside_thumb) {
        ctx.scroll.scroll_y = ScrollFromThumbY(ctx.info, a.dip_y - grip.drag_offset);
        EmitSidePaneScrollChanged(effects, a.pane);
    }
}

void ReducePaneScrollbarDragMoved(AppState& state, SideEffectList& effects, const PaneScrollbarDragMovedAction& a)
{
    // drag target は pane だけから決まるので、ctx 構築前に短絡し
    // 非ドラッグ時の per-event GetEntries().size() + ComputeScrollInfo を回避。
    if (state.view.panes.GetDragTarget() != SidePaneDragTarget(a.pane)) {
        return;
    }
    const auto ctx = GetSidePaneContext(state, a.pane);
    const float new_thumb_y = a.dip_y - state.view.panes.GetDragScrollOffset();
    ctx.scroll.scroll_y = ScrollFromThumbY(ctx.info, new_thumb_y);
    EmitSidePaneScrollChanged(effects, a.pane);
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
