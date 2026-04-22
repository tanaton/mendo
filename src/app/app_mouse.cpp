#include "app.h"
#include "app_constants.h"
#include "app_events.h"
#include "document_utils.h"
#include "pane_layout.h"
#include "resource.h"
#include "ui_constants.h"

// ============================================================
// ヒットテスト / リンク抽出 (クリック・ホバー双方で使用)
// ============================================================

App::HitResult App::HitTest(int screen_x, int screen_y)
{
    const auto& layout = GetPaneLayout();
    return hit_test_.HitTest({
        state_.document.doc.GetNodes(), state_.document.layout_cache, renderer_.GetTheme(),
        state_.view.viewport.GetScrollY(), layout.md_rect.x,
        state_.window.cached_dpi_scale, screen_x, screen_y
        });
}

std::optional<std::pmr::wstring> App::GetLinkAtHit(const HitResult& hit) const
{
    if (hit.node_index < 0 || hit.node_index >= static_cast<int>(state_.document.doc.GetNodes().size())) {
        return std::nullopt;
    }

    return FindLinkAtPosition(state_.document.doc.GetNodes()[hit.node_index], hit.text_pos);
}

// ============================================================
// タイトルバー クリック
// ============================================================

bool App::HandleTitleBarClick(float dip_x, float dip_y)
{
    if (dip_y >= state_.window.titlebar.GetHeight()) {
        return false;
    }

    const auto tb_zone = state_.window.titlebar.HitTest(dip_x, dip_y);
    switch (tb_zone) {
    case TitleBarHitZone::OpenFile:
        Dispatch(OpenFileAction{});
        break;
    case TitleBarHitZone::Help:
        Dispatch(ShowHelpAction{});
        break;
    case TitleBarHitZone::Search:
        Dispatch(OpenSearchBarAction{});
        break;
    case TitleBarHitZone::ThemeToggle:
        Dispatch(ToggleDarkModeAction{});
        break;
    case TitleBarHitZone::FileToggle:
        Dispatch(TogglePaneAction{ PaneTarget::File });
        break;
    case TitleBarHitZone::TocToggle:
        Dispatch(TogglePaneAction{ PaneTarget::Toc });
        break;
    case TitleBarHitZone::Minimize:
        ShowWindow(hwnd_, SW_MINIMIZE);
        break;
    case TitleBarHitZone::Maximize:
        ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
        break;
    case TitleBarHitZone::Close:
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        break;
    default: // タイトルバーの他の領域はWM_NCHITTESTで処理済み
        break;
    }
    return true;
}

// ============================================================
// L ボタン ディスパッチャ
// ============================================================

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
        state_.view.panes.IsFilePaneVisible(),
        state_.view.panes.IsTocPaneVisible()
    );

    switch (zone) {
    case PaneZone::Splitter1:
        Dispatch(SplitterDragStartedAction{ PaneController::DragTarget::Splitter1 });
        return;
    case PaneZone::Splitter2:
        Dispatch(SplitterDragStartedAction{ PaneController::DragTarget::Splitter2 });
        return;
    case PaneZone::FilePane:
        HandleFilePaneClick(dip.x, dip.y, pane_layout);
        return;
    case PaneZone::TocPane:
        HandleTocPaneClick(dip.x, dip.y, pane_layout);
        return;
    case PaneZone::MdPane:
        HandleMdPaneClick(dip.x, dip.y, px, py, pane_layout);
        return;
    default:
        return;
    }
}

void App::OnLButtonUp(int px, int py)
{
    if (state_.search.search_bar_ctrl.IsDragging()) {
        Dispatch(SearchInputDragEndedAction{});
        return;
    }

    const auto drag_target = state_.view.panes.GetDragTarget();
    if (drag_target == PaneController::DragTarget::Splitter1
        || drag_target == PaneController::DragTarget::Splitter2) {
        Dispatch(SplitterDragEndedAction{});
        return;
    }
    if (drag_target == PaneController::DragTarget::MdScrollbar) {
        Dispatch(MdScrollbarDragEndedAction{});
        return;
    }
    if (drag_target == PaneController::DragTarget::FileScrollbar
        || drag_target == PaneController::DragTarget::TocScrollbar) {
        Dispatch(PaneScrollbarDragEndedAction{});
        return;
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

// ============================================================
// マウス移動ディスパッチャ
// ============================================================

void App::OnMouseMove(int px, int py)
{
    auto* rt = renderer_.GetRenderTarget();
    if (!rt) {
        return;
    }

    const auto dip = PixelToDip(px, py);
    const float dip_x = dip.x;
    const auto size = rt->GetSize();

    // 検索バー内ドラッグ選択
    if (state_.search.search_bar_ctrl.IsDragging()) {
        const auto layout = GetPaneLayout();
        const auto& r = layout.md_rect;
        const auto sbl = ComputeSearchBarLayout(r.x, r.width, r.y + r.height, !state_.search.search_state.GetQuery().empty());
        const float text_left = sbl.input_rect.left + SEARCH_INPUT_TEXT_PAD_LEFT;
        const float input_w = sbl.input_rect.right - SEARCH_INPUT_TEXT_PAD_RIGHT - text_left;
        const int pos = renderer_.HitTestSearchInput(state_.search.search_state.GetQuery(), dip.x - text_left, input_w);
        Dispatch(SearchInputDragMovedAction{ pos });
        return;
    }

    if (state_.view.panes.GetDragTarget() == PaneController::DragTarget::Splitter1) {
        Dispatch(SplitterDragMovedAction{ PaneController::DragTarget::Splitter1, dip_x, size.width });
        return;
    }

    if (state_.view.panes.GetDragTarget() == PaneController::DragTarget::FileScrollbar) {
        Dispatch(PaneScrollbarDragMovedAction{ PaneTarget::File, dip.y });
        return;
    }

    if (state_.view.panes.GetDragTarget() == PaneController::DragTarget::TocScrollbar) {
        Dispatch(PaneScrollbarDragMovedAction{ PaneTarget::Toc, dip.y });
        return;
    }

    if (state_.view.panes.GetDragTarget() == PaneController::DragTarget::MdScrollbar) {
        if (layout_service_) {
            Dispatch(MdScrollbarDragMovedAction{ dip.y, layout_service_->GetTotalHeight() });
        }
        return;
    }

    if (state_.view.panes.GetDragTarget() == PaneController::DragTarget::Splitter2) {
        Dispatch(SplitterDragMovedAction{ PaneController::DragTarget::Splitter2, dip_x, size.width });
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

// ============================================================
// R ボタン (ジェスチャー) / X ボタン (ナビゲーション)
// ============================================================

bool App::OnRButtonDown(int px, int py)
{
    if (!IsRenderReady()) {
        return false;
    }
    if (state_.view.viewport.IsDragging()) {
        return false;
    }
    const auto dip = PixelToDip(px, py);
    const auto zone = PaneAtPoint(dip.x, dip.y);
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

