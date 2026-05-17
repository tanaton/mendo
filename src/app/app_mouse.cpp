#include "app.h"
#include "app_constants.h"
#include "app_events.h"
#include "document_utils.h"
#include "pane_layout.h"
#include "resource.h"
#include "ui_constants.h"

App::HitResult App::HitTest(int screen_x, int screen_y)
{
    return hit_test_.HitTest(BuildMdPaneHitContext(screen_x, screen_y, GetPaneLayout()));
}

MdPaneHitContext App::BuildMdPaneHitContext(int px, int py, const PaneLayout& pane_layout) const noexcept
{
    const auto& theme = renderer_.GetTheme();
    return MdPaneHitContext{
        state_.document.doc.GetNodes(), state_.document.layout_cache, theme,
        state_.view.viewport.GetScrollY(), pane_layout.md_rect.x,
        state_.window.cached_dpi_scale, px, py,
        theme.ContentWidth(pane_layout.md_rect.width), pane_layout.md_rect.height,
        &state_.view.block_scroll_x
    };
}

std::optional<std::pmr::string> App::GetLinkAtHit(const HitResult& hit) const
{
    if (hit.node_index < 0 || hit.node_index >= static_cast<int>(state_.document.doc.GetNodes().size())) {
        return std::nullopt;
    }

    return FindLinkAtPosition(state_.document.doc.GetNodes()[hit.node_index], hit.text_pos);
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
    const auto zone = DetectPaneZone(
        dip.x,
        pane_layout,
        renderer_.GetTheme().splitter_width,
        state_.view.panes.IsSidePaneVisible(PaneTarget::File),
        state_.view.panes.IsSidePaneVisible(PaneTarget::Toc));

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

    if (state_.view.viewport.IsDragging()) {
        const auto hit = HitTest(px, py);
        const int dx = px - state_.view.viewport.GetClickStartX();
        const int dy = py - state_.view.viewport.GetClickStartY();
        const bool small_click = (dx * dx + dy * dy) < CLICK_DISTANCE_THRESHOLD_SQ;
        Dispatch(TextSelectionEndedAction{ hit.node_index, hit.text_pos });
        if (small_click && !state_.view.viewport.GetSelection().active) {
            const auto link = GetLinkAtHit(hit);
            if (link.has_value()) {
                HandleLinkClick(link.value());
            }
        }
    }
}

void App::OnMouseMove(int px, int py)
{
    auto* rt = renderer_.GetRenderTarget();
    if (!rt) {
        return;
    }

    const auto dip = PixelToDip(px, py);
    const float dip_x = dip.x;
    const auto size = rt->GetSize();

    if (state_.search.search_bar_ctrl.IsDragging()) {
        const auto layout = GetPaneLayout();
        const auto& r = layout.md_rect;
        const auto& query_wide = state_.search.search_bar_ctrl.GetQueryWide();
        const auto sbl = ComputeSearchBarLayout(r.x, r.width, r.y + r.height, !query_wide.empty());
        const float text_left = sbl.input_rect.left + SEARCH_INPUT_TEXT_PAD_LEFT;
        const float input_w = sbl.input_rect.right - SEARCH_INPUT_TEXT_PAD_RIGHT - text_left;
        const int pos = renderer_.HitTestSearchInput(query_wide, dip.x - text_left, input_w);
        Dispatch(SearchInputDragMovedAction{ pos });
        return;
    }

    switch (state_.view.panes.GetDragTarget()) {
    case PaneController::DragTarget::Splitter1:
        Dispatch(SplitterDragMovedAction{ PaneController::DragTarget::Splitter1, dip_x, size.width });
        return;
    case PaneController::DragTarget::Splitter2:
        Dispatch(SplitterDragMovedAction{ PaneController::DragTarget::Splitter2, dip_x, size.width });
        return;
    case PaneController::DragTarget::FileScrollbar:
        Dispatch(PaneScrollbarDragMovedAction{ PaneTarget::File, dip.y });
        return;
    case PaneController::DragTarget::TocScrollbar:
        Dispatch(PaneScrollbarDragMovedAction{ PaneTarget::Toc, dip.y });
        return;
    case PaneController::DragTarget::MdScrollbar:
        if (layout_service_) {
            Dispatch(MdScrollbarDragMovedAction{ dip.y, layout_service_->GetTotalHeight() });
        }
        return;
    case PaneController::DragTarget::None:
        break;
    }

    if (state_.view.h_drag_node >= 0) {
        Dispatch(BlockHScrollDragMovedAction{ dip_x });
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

bool App::OnRButtonDown(int px, int py)
{
    if (!IsRenderReady()) {
        return false;
    }
    if (state_.view.viewport.IsDragging()) {
        return false;
    }
    const auto dip = PixelToDip(px, py);
    const auto zone = PaneAtPoint(dip.x);
    if (zone != PaneZone::MdPane) {
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
