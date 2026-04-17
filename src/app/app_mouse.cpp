// マウス入力処理のディスパッチャ。タイトルバー判定とツールチップ更新も集約。
#include "app.h"
#include "app_constants.h"
#include "app_events.h"
#include "i18n.h"
#include "pane_layout.h"
#include "resource.h"
#include "ui_constants.h"

// target が変化した場合にタイマーを再設定し、None なら非表示にする。
void App::UpdateTooltip(const TooltipTarget& target, int px, int py)
{
    POINT screen_pos = { px, py };
    ClientToScreen(hwnd_, &screen_pos);
    if (state_.interaction.tooltip.Update(target, screen_pos)) {
        SetTimer(hwnd_, app_timer::TOOLTIP, TOOLTIP_DELAY_MS, nullptr);
    }
    else if (target.IsEmpty()) {
        KillTimer(hwnd_, app_timer::TOOLTIP);
    }
}

void App::ClearTooltip()
{
    KillTimer(hwnd_, app_timer::TOOLTIP);
    state_.interaction.tooltip.Hide();
    state_.interaction.tooltip.ResetTarget();
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
        SetCapture(hwnd_);
        state_.view.panes.StartDrag(PaneController::DragTarget::Splitter1);
        return;
    case PaneZone::Splitter2:
        SetCapture(hwnd_);
        state_.view.panes.StartDrag(PaneController::DragTarget::Splitter2);
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
        state_.search.search_bar_ctrl.EndDrag();
        ReleaseCapture();
        return;
    }

    ReleaseCapture();

    if (state_.view.panes.GetDragTarget() != PaneController::DragTarget::None) {
        const bool was_md_scrollbar = (state_.view.panes.GetDragTarget() == PaneController::DragTarget::MdScrollbar);
        if (was_md_scrollbar) {
            state_.view.viewport.SetScrollbarTracking(false);
        }
        state_.view.panes.EndDrag();
        RECT rc;
        GetClientRect(hwnd_, &rc);
        OnResize(static_cast<UINT>(rc.right - rc.left), static_cast<UINT>(rc.bottom - rc.top));
        if (was_md_scrollbar) {
            resource_manager_.ScheduleBitmapManage();
        }
        return;
    }

    if (state_.view.viewport.IsDragging()) {
        const auto hit = HitTest(px, py);
        if (hit.node_index >= 0) {
            state_.view.viewport.SetSelection(TextSelection::MakeOrdered(
                state_.view.viewport.GetAnchorNode(),
                state_.view.viewport.GetAnchorPos(),
                hit.node_index,
                hit.text_pos
            ));
        }
        state_.view.viewport.SetDragging(false);

        const int dx = px - state_.view.viewport.GetClickStartX();
        const int dy = py - state_.view.viewport.GetClickStartY();
        if (!state_.view.viewport.GetSelection().active && (dx * dx + dy * dy) < CLICK_DISTANCE_THRESHOLD_SQ) {
            const auto link = GetLinkAtHit(hit);
            if (link.has_value()) {
                HandleLinkClick(link.value());
            }
        }

        const auto layout = GetPaneLayout();
        InvalidateMdPane(layout.md_rect);
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
    const float splitter_w = renderer_.GetTheme().splitter_width;

    // 検索バー内ドラッグ選択
    if (state_.search.search_bar_ctrl.IsDragging()) {
        const auto layout = GetPaneLayout();
        const auto& r = layout.md_rect;
        const auto sbl = ComputeSearchBarLayout(r.x, r.width, r.y + r.height, !state_.search.search_state.GetQuery().empty());
        const float text_left = sbl.input_rect.left + SEARCH_INPUT_TEXT_PAD_LEFT;
        const float input_w = sbl.input_rect.right - SEARCH_INPUT_TEXT_PAD_RIGHT - text_left;
        const int pos = renderer_.HitTestSearchInput(state_.search.search_state.GetQuery(), dip.x - text_left, input_w);
        if (pos != state_.search.search_bar_ctrl.GetCaretPos() ||
            state_.search.search_bar_ctrl.GetDragAnchor() != state_.search.search_bar_ctrl.GetSelectionStart()) {
            PostMessage(hwnd_, app_msg::SEARCH_FOCUS, app_param::SEARCH_FOCUS_SET_SELECTION, MAKELPARAM(state_.search.search_bar_ctrl.GetDragAnchor(), pos));
        }
        return;
    }

    if (state_.view.panes.GetDragTarget() == PaneController::DragTarget::Splitter1) {
        state_.view.panes.DragSplitter1To(dip_x, size.width, splitter_w);
        InvalidatePaneLayoutCache();
        Invalidate();
        return;
    }

    if (state_.view.panes.GetDragTarget() == PaneController::DragTarget::FileScrollbar) {
        const auto layout = GetPaneLayout();
        const float total = static_cast<float>(state_.file_explorer.GetEntries().size()) * renderer_.GetTheme().pane_item_height;
        HandleSidePaneScrollDrag(dip.y, layout.file_rect, total, state_.view.panes.FileScroll(), &Renderer::InvalidateFilePaneCache);
        return;
    }

    if (state_.view.panes.GetDragTarget() == PaneController::DragTarget::TocScrollbar) {
        const auto layout = GetPaneLayout();
        const float total = static_cast<float>(state_.document.doc.GetToc().GetEntries().size()) * renderer_.GetTheme().pane_item_height;
        HandleSidePaneScrollDrag(dip.y, layout.toc_rect, total, state_.view.panes.TocScroll(), &Renderer::InvalidateTocPaneCache);
        return;
    }

    if (state_.view.panes.GetDragTarget() == PaneController::DragTarget::MdScrollbar) {
        if (layout_service_) {
            const auto layout = GetPaneLayout();
            const float total_h = layout_service_->GetTotalHeight();
            const auto info = ComputeScrollInfo(layout.md_rect, 0.0f, total_h);
            const float new_thumb_y = dip.y - state_.view.panes.GetDragScrollOffset();
            ScrollTo(ScrollFromThumbY(info, new_thumb_y));
            Invalidate();
        }
        return;
    }

    if (state_.view.panes.GetDragTarget() == PaneController::DragTarget::Splitter2) {
        state_.view.panes.DragSplitter2To(dip_x, size.width, splitter_w);
        InvalidatePaneLayoutCache();
        Invalidate();
        return;
    }

    // MDペイン: ドラッグ選択
    if (!state_.view.viewport.IsDragging()) {
        return;
    }
    const auto hit = HitTest(px, py);
    if (hit.node_index >= 0) {
        state_.view.viewport.SetSelection(TextSelection::MakeOrdered(
            state_.view.viewport.GetAnchorNode(),
            state_.view.viewport.GetAnchorPos(),
            hit.node_index,
            hit.text_pos
        ));
        const auto layout = GetPaneLayout();
        InvalidateMdPane(layout.md_rect);
    }
}
