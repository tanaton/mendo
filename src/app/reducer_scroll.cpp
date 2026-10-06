#include "reducer_internal.h"
#include "app_constants.h"
#include "block_h_scroll.h"
#include "layout_computer.h"
#include <algorithm>

void ReduceKeyScroll(AppState& state, SideEffectList& effects, const KeyScrollAction& a)
{
    auto& viewport = state.view.viewport;
    const float old_scroll = viewport.GetScrollY();
    const float page_size = state.pane_layout_cache.Get().md_rect.height;
    switch (a.type) {
    case ScrollType::LineUp:
        viewport.DirectScrollBy(-SCROLL_LINE_AMOUNT);
        break;
    case ScrollType::LineDown:
        viewport.DirectScrollBy(SCROLL_LINE_AMOUNT);
        break;
    case ScrollType::PageUp:
        viewport.DirectScrollBy(-page_size * SCROLL_PAGE_FACTOR);
        break;
    case ScrollType::PageDown:
        viewport.DirectScrollBy(page_size * SCROLL_PAGE_FACTOR);
        break;
    case ScrollType::Home:
        viewport.SetScrollTarget(0, 0.0f);
        viewport.ApplyScrollTarget(state.document.layout_cache);
        break;
    case ScrollType::End:
        // 末尾は max_scroll に依存するためピクセル指定。scroll target は無効化される。
        viewport.ScrollTo(viewport.GetMaxScroll());
        break;
    }
    EmitScrollEffects(state, effects, old_scroll);
}

void ReduceDirectScrollBy(AppState& state, SideEffectList& effects, const DirectScrollByAction& a)
{
    const float old_scroll = state.view.viewport.GetScrollY();
    state.view.viewport.DirectScrollBy(a.delta);
    EmitScrollEffects(state, effects, old_scroll);
}

void ReduceScrollPane(AppState& state, SideEffectList& effects, const ScrollPaneAction& a)
{
    const auto target = ToPaneTarget(a.pane);
    if (!target) {
        return;
    }
    const auto ctx = GetSidePaneContext(state, *target);
    if (state.view.panes.ScrollSidePaneBy(*target, a.delta, ctx.info.max_scroll)) {
        EmitSidePaneScrollChanged(effects, *target);
    }
}

void ReduceHWheel(AppState& state, SideEffectList& effects, const HWheelAction& a)
{
    // ホバー or ドラッグ中のブロックが横スクロール可能ならそこに適用し、スワイプオーバーレイには流さない。
    const int target = (state.view.h_drag_node >= 0) ? state.view.h_drag_node : state.view.hovered_h_block;
    if (target >= 0) {
        // pane_layout_cache 未確立時は ResolveBlockHScrollGeometry が can_scroll()=false を返す。
        const auto geom = ResolveBlockHScrollGeometry(state, target);
        if (!geom.can_scroll()) {
            return;
        }
        const float dx = static_cast<float>(a.delta) / WHEEL_DELTA * HSCROLL_DIP_PER_NOTCH;
        if (ApplyBlockHScrollDelta(state, target, state.view.GetBlockScrollX(target) + dx, geom.scroll_max())) {
            PushEffect(effects, effect::InvalidateWindow{});
        }
        return;
    }

    auto& swipe = state.interaction.swipe_detector;
    const bool had_overlay = swipe.IsOverlayVisible();
    const int old_direction = swipe.GetOverlayDirection();
    swipe.OnHWheel(a.delta, a.tick);
    PushEffect(effects, effect::SetTimer{ app_timer::Id::SWIPE_OVERLAY, static_cast<UINT>(SwipeDetector::COMMIT_TIMEOUT_MS) });
    if (had_overlay != swipe.IsOverlayVisible() || old_direction != swipe.GetOverlayDirection()) {
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ReduceBlockHHoverChanged(AppState& state, SideEffectList& effects, const BlockHHoverChangedAction& a)
{
    if (state.view.hovered_h_block == a.node_index) {
        return;
    }
    state.view.hovered_h_block = a.node_index;
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceBlockHScrollDragStarted(AppState& state, SideEffectList& effects, const BlockHScrollDragStartedAction& a)
{
    if (!ResolveBlockHScrollGeometry(state, a.node_index).can_scroll()) {
        return;
    }
    // ドラッグ state の初期化は SetCapture より前に行う
    auto& sv = state.view;
    sv.h_drag_node = a.node_index;
    sv.h_drag_start_x = a.dip_x;
    sv.h_drag_start_scroll = sv.GetBlockScrollX(a.node_index);
    PushEffect(effects, effect::SetCapture{});
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceBlockHScrollDragMoved(AppState& state, SideEffectList& effects, const BlockHScrollDragMovedAction& a)
{
    auto& sv = state.view;
    if (sv.h_drag_node < 0) {
        return;
    }
    const auto geom = ResolveBlockHScrollGeometry(state, sv.h_drag_node);
    if (!geom.can_scroll()) {
        return;
    }
    // dip_delta はドラッグの sum = サムが動く DIP。サムが (track - thumb_w) 動いたとき
    // コンテンツが scroll_max 動くべきなので、scroll_delta = dip_delta * scroll_max / drag_range。
    // thumb_w が PANE_SCROLLBAR_THUMB_MIN に張り付いた長大コンテンツでも 1:1 で対応する。
    const float dip_delta = a.dip_x - sv.h_drag_start_x;
    const float thumb_w = BlockHScrollbarThumbWidth(geom.visible_width, geom.natural_width);
    const float drag_range = std::max(1.0f, geom.visible_width - thumb_w);
    const float scroll_max = geom.scroll_max();
    if (ApplyBlockHScrollDelta(state, sv.h_drag_node, sv.h_drag_start_scroll + dip_delta * scroll_max / drag_range, scroll_max)) {
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ReduceBlockHScrollDragEnded(AppState& state, SideEffectList& effects)
{
    if (state.view.h_drag_node < 0) {
        return;
    }
    state.view.h_drag_node = -1;
    PushEffect(effects, effect::ReleaseCapture{});
    PushEffect(effects, effect::InvalidateWindow{});
}
