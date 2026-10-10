#include "app.h"
#include "app_constants.h"
#include "app_controller.h"
#include "app_state_queries.h"
#include "pane_layout.h"
#include "selection_html.h"
#include "ui_constants.h"

App::HitResult App::HitTest(int screen_x, int screen_y)
{
    return HitTest(BuildMdPaneHitContext(screen_x, screen_y, GetPaneLayout()));
}

App::HitResult App::HitTest(const MdPaneHitContext& ctx)
{
    return hit_test_.HitTest(ctx);
}

MdPaneHitContext App::BuildMdPaneHitContext(int px, int py, const PaneLayout& pane_layout) const noexcept
{
    const auto& theme = renderer_.GetTheme();
    return MdPaneHitContext{
        .nodes = state_.document.doc.GetNodes(),
        .cache = state_.document.layout_cache,
        .theme = theme,
        .scroll_y = state_.view.viewport.GetScrollY(),
        .md_rect = pane_layout.md_rect,
        .dpi_scale = state_.window.cached_dpi_scale,
        .screen_x = px,
        .screen_y = py,
        .content_width = theme.ContentWidth(pane_layout.md_rect.width),
        .block_scroll_x = &state_.view.block_scroll_x,
    };
}

std::optional<std::string_view> App::GetLinkAtHit(const HitResult& hit) const
{
    const auto& nodes = state_.document.doc.GetNodes();
    if (hit.node_index < 0 || hit.node_index >= static_cast<int>(nodes.size())) {
        return std::nullopt;
    }
    return FindLinkAtPosition(nodes[hit.node_index], hit.text_pos);
}

void App::OnLButtonDown(int px, int py)
{
    if (!IsRenderReady()) {
        return;
    }

    const auto dip = PixelToDip(px, py);

    if (HandleTitleBarClick(dip.x, dip.y)) {
        return;
    }

    const auto pane_layout = GetPaneLayout();
    const auto zone = ZoneAt(dip.x, pane_layout);

    switch (zone) {
    case PaneZone::None:
        return;
    case PaneZone::FilePane:
    case PaneZone::TocPane:
        HandleSidePaneClick(*ToPaneTarget(zone), dip.x, dip.y, pane_layout);
        return;
    case PaneZone::Splitter1:
        Dispatch(SplitterDragStartedAction{ PaneController::DragTarget::Splitter1 });
        return;
    case PaneZone::Splitter2:
        Dispatch(SplitterDragStartedAction{ PaneController::DragTarget::Splitter2 });
        return;
    case PaneZone::MdPane:
        HandleMdPaneClick(dip.x, dip.y, px, py, pane_layout);
        return;
    }
    std::unreachable();
}

void App::OnLButtonUp(int px, int py)
{
    if (state_.view.h_drag_node >= 0) {
        Dispatch(BlockHScrollDragEndedAction{});
        return;
    }

    if (state_.search.search_bar_ctrl.IsDragging()) {
        Dispatch(SearchInputDragEndedAction{});
        return;
    }

    switch (state_.view.panes.GetDragTarget()) {
    case PaneController::DragTarget::Splitter1:
    case PaneController::DragTarget::Splitter2:
        Dispatch(SplitterDragEndedAction{});
        return;
    case PaneController::DragTarget::MdScrollbar:
        Dispatch(MdScrollbarDragEndedAction{});
        return;
    case PaneController::DragTarget::FileScrollbar:
    case PaneController::DragTarget::TocScrollbar:
        Dispatch(PaneScrollbarDragEndedAction{});
        return;
    case PaneController::DragTarget::None:
        break;
    }

    if (!state_.view.viewport.IsDragging()) {
        return;
    }
    const auto hit = HitTest(px, py);
    const int dx = px - state_.view.viewport.GetClickStartX();
    const int dy = py - state_.view.viewport.GetClickStartY();
    const bool small_click = (dx * dx + dy * dy) < CLICK_DISTANCE_THRESHOLD_SQ;
    Dispatch(TextSelectionEndedAction{ hit.node_index, hit.text_pos });
    if (!small_click || state_.view.viewport.GetSelection().active) {
        return;
    }
    if (const auto link = GetLinkAtHit(hit)) {
        HandleLinkClick(*link);
    }
}

void App::OnLButtonDblClk(int px, int py)
{
    if (!IsRenderReady()) {
        return;
    }
    const auto dip = PixelToDip(px, py);
    // CS_DBLCLKS により連続クリックの2回目は WM_LBUTTONDBLCLK になるため、
    // タイトルバーボタンのクリックを先に処理する。
    if (HandleTitleBarClick(dip.x, dip.y)) {
        return;
    }
    if (PaneAtPoint(dip.x) != PaneZone::MdPane) {
        return;
    }
    // 検索バーのボタンも連打として扱う (タイトルバーボタンと同じ理由)。
    if (HandleSearchBarClick(dip.x, dip.y, GetPaneLayout(), true)) {
        return;
    }
    const auto hit = HitTest(px, py);
    Dispatch(SelectWordAction{ hit.node_index, hit.text_pos });
}

void App::OnMouseMove(int px, int py)
{
    auto* rt = renderer_.GetRenderTarget();
    if (!rt) {
        return;
    }

    const auto dip = PixelToDip(px, py);

    if (state_.search.search_bar_ctrl.IsDragging()) {
        const auto sbl = ComputeSearchBarLayoutForMd(GetPaneLayout().md_rect);
        const int pos = HitTestSearchInputPos(sbl, state_.search.search_bar_ctrl.GetQueryWide(), dip.x);
        Dispatch(SearchInputDragMovedAction{ pos });
        return;
    }

    switch (const auto drag = state_.view.panes.GetDragTarget()) {
    case PaneController::DragTarget::Splitter1:
    case PaneController::DragTarget::Splitter2:
        Dispatch(SplitterDragMovedAction{ drag, dip.x, rt->GetSize().width });
        return;
    case PaneController::DragTarget::FileScrollbar:
        Dispatch(PaneScrollbarDragMovedAction{ PaneTarget::File, dip.y });
        return;
    case PaneController::DragTarget::TocScrollbar:
        Dispatch(PaneScrollbarDragMovedAction{ PaneTarget::Toc, dip.y });
        return;
    case PaneController::DragTarget::MdScrollbar:
        Dispatch(MdScrollbarDragMovedAction{ dip.y });
        return;
    case PaneController::DragTarget::None:
        break;
    }

    if (state_.view.h_drag_node >= 0) {
        Dispatch(BlockHScrollDragMovedAction{ dip.x });
        return;
    }

    if (!state_.view.viewport.IsDragging()) {
        return;
    }
    const auto hit = HitTest(px, py);
    if (hit.node_index < 0) {
        return;
    }
    Dispatch(TextSelectionMovedAction{ hit.node_index, hit.text_pos });
}

void App::OnMouseWheel(int px, int py, short delta, bool ctrl)
{
    if (!IsRenderReady()) {
        return;
    }

    if (ctrl) {
        Dispatch(app_controller::HandleMouseWheel(MouseWheelEvent{ delta, true, PaneZone::MdPane }));
        return;
    }

    // 縦スクロールが発生した時点で SwipeDetector の軸ロックを更新（再武装）し、
    // 直後の水平ホイールがスワイプとして誤検出されないようにする。
    const bool had_overlay = state_.interaction.swipe_detector.IsOverlayVisible();
    state_.interaction.swipe_detector.NotifyVScroll(GetTickCount64());
    if (had_overlay) {
        EmitEffect(effect::KillTimer{ app_timer::Id::SWIPE_OVERLAY });
        Invalidate();
    }

    const auto dip = PixelToDip(px, py);
    Dispatch(app_controller::HandleMouseWheel(MouseWheelEvent{ delta, false, ZoneAt(dip.x, GetPaneLayout()) }));
}

void App::OnMouseHWheel(short delta)
{
    Dispatch(HWheelAction{ delta, GetTickCount64() });
}

bool App::OnRButtonDown(int px, int py)
{
    if (!IsRenderReady()) {
        return false;
    }
    // 左ドラッグ進行中にジェスチャを開始すると、完了時の ReleaseCapture が
    // 進行中ドラッグのキャプチャを破壊する。
    if (IsLeftDragActive(state_)) {
        return false;
    }
    const auto dip = PixelToDip(px, py);
    if (PaneAtPoint(dip.x) != PaneZone::MdPane) {
        return false;
    }
    Dispatch(RightClickGestureStartedAction{ dip.x, dip.y });
    return true;
}

bool App::OnRButtonUp(int px, int py)
{
    if (state_.interaction.gesture.GetPhase() == GesturePhase::Idle) {
        return false;
    }
    POINT pt{ px, py };
    ClientToScreen(hwnd_, &pt);
    Dispatch(RightClickGestureCompletedAction{ pt.x, pt.y });
    return true;
}

void App::OnRButtonMove(int px, int py)
{
    if (!IsRenderReady()) {
        return;
    }
    const auto dip = PixelToDip(px, py);
    Dispatch(RightClickGestureMovedAction{ dip.x, dip.y });
}
