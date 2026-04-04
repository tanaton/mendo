#include "app.h"
#include "i18n.h"
#include "pane_layout.h"
#include "document_utils.h"
#include "ui_constants.h"
#include "resource.h"
#include <shellapi.h>
#include <cwctype>
#include <filesystem>

namespace {

bool IsEditableTextFile(std::wstring_view path)
{
    auto ext = std::filesystem::path(path).extension().wstring();
    for (auto& c : ext) {
        c = std::towlower(c);
    }
    return ext == L".md" || ext == L".markdown" || ext == L".mkd" || ext == L".txt";
}

// ペインヘッダー内のボタンがクリックされたか判定する。
bool HitPaneHeaderButton(float dip_x, float dip_y, const PaneRect& rect, float header_height, D2D1_RECT_F(*button_rect_fn)(float, float) noexcept)
{
    const float local_x = dip_x - rect.x;
    const float local_y = dip_y - rect.y;
    if (local_y >= header_height) {
        return false;
    }
    return PointInRect(local_x, local_y, button_rect_fn(rect.width, header_height));
}

} // namespace

// target が変化した場合にタイマーを再設定し、None なら非表示にする。
void App::UpdateTooltip(const TooltipTarget& target, int px, int py)
{
    POINT screen_pos = { px, py };
    ClientToScreen(hwnd_, &screen_pos);
    if (tooltip_.Update(target, screen_pos)) {
        SetTimer(hwnd_, TIMER_TOOLTIP, 500, nullptr);
    }
    else if (target.IsEmpty()) {
        KillTimer(hwnd_, TIMER_TOOLTIP);
    }
}

void App::ClearTooltip()
{
    KillTimer(hwnd_, TIMER_TOOLTIP);
    tooltip_.Hide();
    tooltip_.ResetTarget();
}

void App::OnMouseLeave()
{
    ClearTooltip();
}

void App::RefreshFilePane()
{
    file_explorer_.Refresh();
    if (!doc_.GetFilePath().empty()) {
        file_explorer_.SetCurrentFile(doc_.GetFilePath());
    }
    renderer_.InvalidateFilePaneCache();
    Invalidate();
}

// ============================================================
// コンテキストメニュー
// ============================================================

void App::OnContextMenu(int screen_x, int screen_y)
{
    POINT client_pt{ screen_x, screen_y };
    ScreenToClient(hwnd_, &client_pt);
    const auto dip = PixelToDip(client_pt.x, client_pt.y);
    const auto zone = PaneAtPoint(dip.x, dip.y);

    ContextMenuParams params;
    params.screen_x = screen_x;
    params.screen_y = screen_y;
    params.dpi_scale = cached_dpi_scale_;
    params.can_go_back = nav_history_.CanGoBack();
    params.can_go_forward = nav_history_.CanGoForward();
    params.has_file = !doc_.GetFilePath().empty();
    params.has_selection = viewport_.GetSelection().active && viewport_.GetSelection().start_node >= 0;
    params.dark_mode_checked = theme_service_.IsDarkMode();
    params.file_pane_checked = panes_.IsFilePaneVisible();
    params.toc_pane_checked = panes_.IsTocPaneVisible();
    params.show_file_items = (zone == PaneZone::MdPane);
    params.theme = &renderer_.GetTheme();

    const int cmd = ctx_menu_.Show(hwnd_, params);

    if (cmd == IDM_NAV_BACK) {
        NavigateBack();
    }
    else if (cmd == IDM_NAV_FORWARD) {
        NavigateForward();
    }
    else if (cmd == IDM_EDIT_FILE) {
        const auto& file_path = doc_.GetFilePath();
        if (IsEditableTextFile(file_path)) {
            ShellExecuteW(hwnd_, L"open", file_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    else if (cmd == IDM_COPY) {
        CopySelectionToClipboard();
    }
    else if (cmd == IDM_TOGGLE_DARK_MODE) {
        ToggleDarkMode();
    }
    else if (cmd == IDM_TOGGLE_FILE_PANE) {
        panes_.ToggleFilePane();
        RefreshPaneLayout();
    }
    else if (cmd == IDM_TOGGLE_TOC_PANE) {
        panes_.ToggleTocPane();
        RefreshPaneLayout();
    }
}

// ============================================================
// 右クリックジェスチャー
// ============================================================

bool App::OnRButtonDown(int px, int py)
{
    if (!renderer_.GetRenderTarget()) {
        return false;
    }
    if (viewport_.IsDragging()) {
        return false;
    }
    const auto dip = PixelToDip(px, py);
    const auto zone = PaneAtPoint(dip.x, dip.y);
    if (zone != PaneZone::MdPane) {
        return false;
    }
    gesture_.OnRButtonDown(dip.x, dip.y);
    SetCapture(hwnd_);
    return true;
}

bool App::OnRButtonUp(int px, int py)
{
    if (gesture_.GetPhase() == GesturePhase::Idle) {
        return false;
    }
    const auto result = gesture_.OnRButtonUp();
    ReleaseCapture();

    switch (result) {
    case GestureResult::ShowContextMenu: {
        gesture_.Reset();
        POINT pt{ px, py };
        ClientToScreen(hwnd_, &pt);
        OnContextMenu(pt.x, pt.y);
        break;
    }
    case GestureResult::Back:
        NavigateBack();
        Invalidate();
        break;
    case GestureResult::Forward:
        NavigateForward();
        Invalidate();
        break;
    case GestureResult::None:
        Invalidate();
        break;
    }
    return true;
}

void App::OnRButtonMove(int px, int py)
{
    if (!renderer_.GetRenderTarget()) {
        return;
    }
    const auto dip = PixelToDip(px, py);
    gesture_.OnMouseMove(dip.x, dip.y);

    if (gesture_.IsGestureActive()) {
        Invalidate();
    }
}

void App::OnXButtonBack()
{
    NavigateBack();
}

void App::OnXButtonForward()
{
    NavigateForward();
}

// ============================================================
// ヒットテスト
// ============================================================

App::HitResult App::HitTest(int screen_x, int screen_y) const
{
    const auto pane_layout = GetPaneLayout();
    return hit_test_.HitTest(
        doc_.GetNodes(),
        layout_cache_,
        renderer_.GetTheme(),
        viewport_.GetScrollY(),
        pane_layout.md_rect.x,
        cached_dpi_scale_,
        screen_x,
        screen_y
    );
}

std::optional<std::pmr::wstring> App::GetLinkAtHit(const HitResult& hit) const
{
    if (hit.node_index < 0 || hit.node_index >= static_cast<int>(doc_.GetNodes().size())) {
        return std::nullopt;
    }

    return FindLinkAtPosition(doc_.GetNodes()[hit.node_index], hit.text_pos);
}

// ============================================================
// マウスイベント
// ============================================================

bool App::TryHandlePaneScrollbarClick(float dip_x, float dip_y, const PaneRect& rect,
    PaneController::DragTarget target,
    const PaneScrollInfo& scroll_info,
    float total_content, ScrollState& scroll,
    void (Renderer::* invalidate)())
{
    const float local_x = dip_x - rect.x;

    if ((local_x >= rect.width - PANE_SCROLLBAR_WIDTH - 4.0f) && (total_content > scroll_info.content_height)) {
        SetCapture(hwnd_);
        panes_.StartDrag(target);
        bool dirty = false;
        HandleScrollbarClick(dip_y, scroll_info, scroll, dirty);
        if (dirty) {
            (renderer_.*invalidate)();
        }
        return true;
    }
    return false;
}

void App::HandleFilePaneClick(float dip_x, float dip_y, const PaneLayout& layout)
{
    const auto& theme = renderer_.GetTheme();

    if (dip_y - layout.file_rect.y < theme.pane_header_height) {
        if (HitPaneHeaderButton(dip_x, dip_y, layout.file_rect, theme.pane_header_height, PaneCloseButtonRect)) {
            panes_.ToggleFilePane();
            RefreshPaneLayout();
        }
        else if (HitPaneHeaderButton(dip_x, dip_y, layout.file_rect, theme.pane_header_height, PaneRefreshButtonRect)) {
            RefreshFilePane();
        }
        return;
    }

    const float total_content = static_cast<float>(file_explorer_.GetEntries().size()) * theme.pane_item_height;
    const auto scroll_info = ComputePaneScrollInfo(layout.file_rect, total_content);

    if (TryHandlePaneScrollbarClick(dip_x, dip_y, layout.file_rect,
        PaneController::DragTarget::FileScrollbar,
        scroll_info, total_content, panes_.FileScroll(),
        &Renderer::InvalidateFilePaneCache)) {
        return;
    }
    const float local_y = dip_y - scroll_info.content_top + panes_.FileScroll().scroll_y;
    const int idx = file_explorer_.HitTest(local_y, theme.pane_item_height);
    if (idx >= 0 && idx < static_cast<int>(file_explorer_.GetEntries().size())) {
        const auto& file_entry = file_explorer_.GetEntries()[idx];
        if (file_entry.is_directory) {
            file_explorer_.SetDirectory(file_entry.full_path);
            if (!doc_.GetFilePath().empty()) {
                file_explorer_.SetCurrentFile(doc_.GetFilePath());
            }
            panes_.FileScroll() = {};
            renderer_.InvalidateFilePaneCache();
            Invalidate();
        }
        else if (!file_entry.is_current) {
            if (GetFileAttributesW(file_entry.full_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                RefreshFilePane();
                ShowToast(i18n::S().toast_file_not_found);
                return;
            }
            PushNavHistory();
            LoadMarkdownFile(file_entry.full_path);
        }
    }
}

void App::HandleTocPaneClick(float dip_x, float dip_y, const PaneLayout& layout)
{
    const auto& theme = renderer_.GetTheme();

    if (dip_y - layout.toc_rect.y < theme.pane_header_height) {
        if (HitPaneHeaderButton(dip_x, dip_y, layout.toc_rect, theme.pane_header_height, PaneCloseButtonRect)) {
            panes_.ToggleTocPane();
            RefreshPaneLayout();
        }
        return;
    }

    const float total_content = static_cast<float>(doc_.GetToc().GetEntries().size()) * theme.pane_item_height;
    const auto scroll_info = ComputePaneScrollInfo(layout.toc_rect, total_content);

    if (TryHandlePaneScrollbarClick(dip_x, dip_y, layout.toc_rect,
        PaneController::DragTarget::TocScrollbar,
        scroll_info, total_content, panes_.TocScroll(),
        &Renderer::InvalidateTocPaneCache)) {
        return;
    }
    const float local_y = dip_y - scroll_info.content_top + panes_.TocScroll().scroll_y;
    const int idx = doc_.GetToc().HitTest(local_y, theme.pane_item_height);
    if (idx >= 0 && idx < static_cast<int>(doc_.GetToc().GetEntries().size())) {
        PushNavHistory();
        NavigateToAnchor(doc_.GetToc().GetEntries()[idx].anchor_id);
    }
}

void App::OnLButtonDown(int px, int py)
{
    if (!renderer_.GetRenderTarget()) {
        return;
    }

    const auto dip = PixelToDip(px, py);

    // タイトルバーボタンのクリック処理
    if (dip.y < titlebar_.GetHeight()) {
        const auto tb_zone = titlebar_.HitTest(dip.x, dip.y);
        if (tb_zone == TitleBarHitZone::Help) {
            if (!doc_.GetFilePath().empty() && !IsHelpPath(doc_.GetFilePath())) {
                PushNavHistory();
            }
            LoadHelpDocument();
            return;
        }
        if (tb_zone == TitleBarHitZone::Search) {
            OnSearchOpen();
            return;
        }
        if (tb_zone == TitleBarHitZone::ThemeToggle) {
            ToggleDarkMode();
            return;
        }
        if (tb_zone == TitleBarHitZone::FileToggle || tb_zone == TitleBarHitZone::TocToggle) {
            if (tb_zone == TitleBarHitZone::FileToggle) {
                panes_.ToggleFilePane();
            }
            else {
                panes_.ToggleTocPane();
            }
            RefreshPaneLayout();
            return;
        }
        if (tb_zone == TitleBarHitZone::Minimize) {
            ShowWindow(hwnd_, SW_MINIMIZE);
            return;
        }
        if (tb_zone == TitleBarHitZone::Maximize) {
            ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
            return;
        }
        if (tb_zone == TitleBarHitZone::Close) {
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            return;
        }
        return;  // タイトルバーの他の領域はWM_NCHITTESTで処理済み
    }

    const auto pane_layout = GetPaneLayout();
    const auto zone = DetectPaneZone(dip.x, pane_layout,
        renderer_.GetTheme().splitter_width,
        panes_.IsFilePaneVisible(), panes_.IsTocPaneVisible());

    switch (zone) {
    case PaneZone::Splitter1:
        SetCapture(hwnd_);
        panes_.StartDrag(PaneController::DragTarget::Splitter1);
        return;
    case PaneZone::Splitter2:
        SetCapture(hwnd_);
        panes_.StartDrag(PaneController::DragTarget::Splitter2);
        return;
    case PaneZone::FilePane:
        HandleFilePaneClick(dip.x, dip.y, pane_layout);
        return;
    case PaneZone::TocPane:
        HandleTocPaneClick(dip.x, dip.y, pane_layout);
        return;
    case PaneZone::MdPane: {
        // 検索バーのクリック処理
        if (search_state_.IsVisible()) {
            const auto& r = pane_layout.md_rect;
            const auto sbl = ComputeSearchBarLayout(r.x, r.width, r.y + r.height, !search_state_.GetQuery().empty());
            if (dip.y >= sbl.bar_top) {
                if (PointInRect(dip.x, dip.y, sbl.up_btn)) {
                    OnSearchPrev();
                    return;
                }
                if (PointInRect(dip.x, dip.y, sbl.down_btn)) {
                    OnSearchNext();
                    return;
                }
                if (PointInRect(dip.x, dip.y, sbl.case_btn)) {
                    OnToggleCaseSensitive();
                    return;
                }
                if (PointInRect(dip.x, dip.y, sbl.highlight_btn)) {
                    OnToggleHighlight();
                    return;
                }
                if (PointInRect(dip.x, dip.y, sbl.close_btn)) {
                    OnSearchClose();
                    return;
                }
                if (PointInRect(dip.x, dip.y, sbl.input_rect)) {
                    const float text_left = sbl.input_rect.left + SEARCH_INPUT_TEXT_PAD_LEFT;
                    const float input_w = sbl.input_rect.right - SEARCH_INPUT_TEXT_PAD_RIGHT - text_left;
                    const int pos = renderer_.HitTestSearchInput(
                        search_state_.GetQuery(), dip.x - text_left, input_w);
                    search_dragging_ = true;
                    search_drag_anchor_ = pos;
                    SetCapture(hwnd_);
                    PostMessage(hwnd_, WM_APP_SEARCH_FOCUS, App::SEARCH_FOCUS_SET_CARET, static_cast<LPARAM>(pos));
                    return;
                }
                PostMessage(hwnd_, WM_APP_SEARCH_FOCUS, App::SEARCH_FOCUS_SELECT_ALL, 0);
                return;
            }
        }

        const auto nav_hit = hit_test_.NavButtonHitTest(dip.x, dip.y, pane_layout.md_rect);
        if (nav_hit == NavButtonHover::Back) {
            NavigateBack();
            return;
        }
        if (nav_hit == NavButtonHover::Forward) {
            NavigateForward();
            return;
        }
        // コピーボタンのクリック判定（クリック位置で再判定）
        const float content_width = renderer_.GetTheme().ContentWidth(pane_layout.md_rect.width);
        const auto copy_node = hit_test_.CopyButtonHitTest(
            doc_.GetNodes(), layout_cache_, renderer_.GetTheme(),
            viewport_.GetScrollY(), pane_layout.md_rect.x,
            content_width, pane_layout.md_rect.height,
            cached_dpi_scale_, px, py);
        if (copy_node >= 0) {
            CopyCodeBlockToClipboard(copy_node);
            return;
        }
        // MDペインスクロールバーのクリック判定
        if (layout_service_) {
            float total_h = layout_service_->GetTotalHeight();
            float viewport_h = pane_layout.md_rect.height;
            if (total_h > viewport_h) {
                const float sb_left = pane_layout.md_rect.x + pane_layout.md_rect.width
                    - PANE_SCROLLBAR_WIDTH - PANE_SCROLLBAR_MARGIN;
                if (dip.x >= sb_left - PANE_SCROLLBAR_HIT_PADDING) {
                    SetCapture(hwnd_);
                    panes_.StartDrag(PaneController::DragTarget::MdScrollbar);
                    viewport_.SetScrollbarTracking(true);
                    const auto info = ComputeScrollInfo(pane_layout.md_rect, 0.0f, total_h);
                    const float thumb_y = ComputeThumbY(info, viewport_.GetScrollY());
                    if (dip.y >= thumb_y && dip.y <= thumb_y + info.thumb_height) {
                        panes_.SetDragScrollOffset(dip.y - thumb_y);
                    }
                    else {
                        panes_.SetDragScrollOffset(info.thumb_height * 0.5f);
                        const float new_thumb_y = dip.y - panes_.GetDragScrollOffset();
                        viewport_.ScrollTo(ScrollFromThumbY(info, new_thumb_y));
                        Invalidate();
                    }
                    return;
                }
            }
        }
        break;
    }
    default:
        return;
    }

    // MDペイン: 選択ロジック
    SetCapture(hwnd_);
    viewport_.SetClickStart(px, py);
    auto hit = HitTest(px, py);
    if (hit.node_index >= 0) {
        viewport_.SetAnchor(hit.node_index, hit.text_pos);
        viewport_.SetDragging(true);
        viewport_.GetSelectionMut().Clear();
        InvalidateMdPane(pane_layout.md_rect);
    }
}

void App::OnLButtonUp(int px, int py)
{
    if (search_dragging_) {
        search_dragging_ = false;
        ReleaseCapture();
        return;
    }

    ReleaseCapture();

    if (panes_.GetDragTarget() != PaneController::DragTarget::None) {
        if (panes_.GetDragTarget() == PaneController::DragTarget::MdScrollbar) {
            viewport_.SetScrollbarTracking(false);
        }
        panes_.EndDrag();
        RECT rc;
        GetClientRect(hwnd_, &rc);
        OnResize(static_cast<UINT>(rc.right - rc.left), static_cast<UINT>(rc.bottom - rc.top));
        return;
    }

    if (viewport_.IsDragging()) {
        const auto hit = HitTest(px, py);
        if (hit.node_index >= 0) {
            viewport_.SetSelection(TextSelection::MakeOrdered(
                viewport_.GetAnchorNode(),
                viewport_.GetAnchorPos(),
                hit.node_index,
                hit.text_pos
            ));
        }
        viewport_.SetDragging(false);

        const int dx = px - viewport_.GetClickStartX();
        const int dy = py - viewport_.GetClickStartY();
        if (!viewport_.GetSelection().active && (dx * dx + dy * dy) < 25) {
            const auto link = GetLinkAtHit(hit);
            if (link.has_value()) {
                HandleLinkClick(link.value());
            }
        }

        const auto layout = GetPaneLayout();
        InvalidateMdPane(layout.md_rect);
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
    const float splitter_w = renderer_.GetTheme().splitter_width;

    // 検索バー内ドラッグ選択
    if (search_dragging_) {
        const auto layout = GetPaneLayout();
        const auto& r = layout.md_rect;
        const auto sbl = ComputeSearchBarLayout(r.x, r.width, r.y + r.height, !search_state_.GetQuery().empty());
        const float text_left = sbl.input_rect.left + SEARCH_INPUT_TEXT_PAD_LEFT;
        const float input_w = sbl.input_rect.right - SEARCH_INPUT_TEXT_PAD_RIGHT - text_left;
        const int pos = renderer_.HitTestSearchInput(
            search_state_.GetQuery(), dip.x - text_left, input_w);
        if (pos != search_caret_pos_ || search_drag_anchor_ != search_selection_start_) {
            PostMessage(hwnd_, WM_APP_SEARCH_FOCUS, App::SEARCH_FOCUS_SET_SELECTION,
                MAKELPARAM(search_drag_anchor_, pos));
        }
        return;
    }

    if (panes_.GetDragTarget() == PaneController::DragTarget::Splitter1) {
        panes_.DragSplitter1To(dip_x, size.width, splitter_w);
        InvalidatePaneLayoutCache();
        Invalidate();
        return;
    }

    if (panes_.GetDragTarget() == PaneController::DragTarget::FileScrollbar) {
        const auto layout = GetPaneLayout();
        const float total_content = static_cast<float>(file_explorer_.GetEntries().size()) * renderer_.GetTheme().pane_item_height;
        const auto info = ComputePaneScrollInfo(layout.file_rect, total_content);
        bool dirty = false;
        HandleScrollbarDrag(dip.y, info, panes_.FileScroll(), dirty);
        if (dirty) { renderer_.InvalidateFilePaneCache(); }
        return;
    }

    if (panes_.GetDragTarget() == PaneController::DragTarget::TocScrollbar) {
        const auto layout = GetPaneLayout();
        const float total_content = static_cast<float>(doc_.GetToc().GetEntries().size()) * renderer_.GetTheme().pane_item_height;
        const auto info = ComputePaneScrollInfo(layout.toc_rect, total_content);
        bool dirty = false;
        HandleScrollbarDrag(dip.y, info, panes_.TocScroll(), dirty);
        if (dirty) { renderer_.InvalidateTocPaneCache(); }
        return;
    }

    if (panes_.GetDragTarget() == PaneController::DragTarget::MdScrollbar) {
        if (layout_service_) {
            const auto layout = GetPaneLayout();
            const float total_h = layout_service_->GetTotalHeight();
            const auto info = ComputeScrollInfo(layout.md_rect, 0.0f, total_h);
            const float new_thumb_y = dip.y - panes_.GetDragScrollOffset();
            viewport_.ScrollTo(ScrollFromThumbY(info, new_thumb_y));
            Invalidate();
        }
        return;
    }

    if (panes_.GetDragTarget() == PaneController::DragTarget::Splitter2) {
        panes_.DragSplitter2To(dip_x, size.width, splitter_w);
        InvalidatePaneLayoutCache();
        Invalidate();
        return;
    }

    // MDペイン: ドラッグ選択
    if (!viewport_.IsDragging()) {
        return;
    }
    const auto hit = HitTest(px, py);
    if (hit.node_index >= 0) {
        viewport_.SetSelection(TextSelection::MakeOrdered(
            viewport_.GetAnchorNode(), viewport_.GetAnchorPos(), hit.node_index, hit.text_pos));
        const auto layout = GetPaneLayout();
        InvalidateMdPane(layout.md_rect);
    }
}

void App::OnMouseHover(int px, int py)
{
    if (!renderer_.GetRenderTarget()) {
        return;
    }

    const auto dip = PixelToDip(px, py);
    const float dip_x = dip.x;
    const float dip_y = dip.y;

    // タイトルバーのホバー処理
    if (dip_y < titlebar_.GetHeight()) {
        const auto tb_zone = titlebar_.HitTest(dip_x, dip_y);
        SetCursor(cursor_arrow_);
        if (titlebar_.SetHovered(tb_zone)) {
            InvalidateTitleBar();
        }
        // タイトルバーボタンのツールチップ
        const auto& ls = i18n::S();
        TooltipTarget tt;
        switch (tb_zone) {
        case TitleBarHitZone::Help:        tt = { TooltipTarget::Zone::TitleBarButton, ls.tooltip_help }; break;
        case TitleBarHitZone::ThemeToggle: tt = { TooltipTarget::Zone::TitleBarButton, ls.tooltip_theme_toggle }; break;
        case TitleBarHitZone::Search:      tt = { TooltipTarget::Zone::TitleBarButton, ls.tooltip_search }; break;
        case TitleBarHitZone::FileToggle:  tt = { TooltipTarget::Zone::TitleBarButton, ls.tooltip_file_pane }; break;
        case TitleBarHitZone::TocToggle:   tt = { TooltipTarget::Zone::TitleBarButton, ls.tooltip_toc_pane }; break;
        case TitleBarHitZone::Minimize:    tt = { TooltipTarget::Zone::TitleBarButton, ls.tooltip_minimize }; break;
        case TitleBarHitZone::Maximize:
            tt = { TooltipTarget::Zone::TitleBarButton, IsZoomed(hwnd_) ? ls.tooltip_restore : ls.tooltip_maximize };
            break;
        case TitleBarHitZone::Close:       tt = { TooltipTarget::Zone::TitleBarButton, ls.tooltip_close }; break;
        default: break;
        }
        UpdateTooltip(tt, px, py);
        return;
    }
    // タイトルバー外に出たらホバーをリセット
    if (titlebar_.SetHovered(TitleBarHitZone::None)) {
        InvalidateTitleBar();
    }

    const auto pane_layout = GetPaneLayout();
    const auto zone = DetectPaneZone(dip_x, pane_layout,
        renderer_.GetTheme().splitter_width,
        panes_.IsFilePaneVisible(), panes_.IsTocPaneVisible());

    int new_file_hover = -1;
    int new_toc_hover = -1;

    // ペインゾーン外に出たら閉じる/更新ボタンのホバーをリセット
    if (zone != PaneZone::FilePane) {
        bool changed = panes_.SetFileCloseHovered(false);
        changed |= panes_.SetFileRefreshHovered(false);
        if (changed) {
            renderer_.InvalidateFilePaneCache();
            InvalidatePane(pane_layout.file_rect);
        }
    }
    if (zone != PaneZone::TocPane && panes_.SetTocCloseHovered(false)) {
        renderer_.InvalidateTocPaneCache();
        InvalidatePane(pane_layout.toc_rect);
    }

    switch (zone) {
    case PaneZone::Splitter1:
    case PaneZone::Splitter2:
        SetCursor(cursor_sizewe_);
        UpdateTooltip({}, px, py);
        break;
    case PaneZone::FilePane: {
        const float header_h = renderer_.GetTheme().pane_header_height;
        const bool close_hit = HitPaneHeaderButton(dip_x, dip_y, pane_layout.file_rect, header_h, PaneCloseButtonRect);
        const bool refresh_hit = HitPaneHeaderButton(dip_x, dip_y, pane_layout.file_rect, header_h, PaneRefreshButtonRect);
        SetCursor((close_hit || refresh_hit) ? cursor_hand_ : cursor_arrow_);
        {
            bool changed = panes_.SetFileCloseHovered(close_hit);
            changed |= panes_.SetFileRefreshHovered(refresh_hit);
            if (changed) {
                renderer_.InvalidateFilePaneCache();
                InvalidatePane(pane_layout.file_rect);
            }
        }
        const float content_top = pane_layout.file_rect.y + header_h;
        const float local_y = dip_y - content_top + panes_.FileScroll().scroll_y;
        new_file_hover = file_explorer_.HitTest(local_y, renderer_.GetTheme().pane_item_height);
        // ファイルペインのツールチップ
        {
            TooltipTarget tt;
            if (close_hit) {
                tt.zone = TooltipTarget::Zone::FilePaneButton;
                tt.text = i18n::S().tooltip_pane_close;
            }
            else if (refresh_hit) {
                tt.zone = TooltipTarget::Zone::FilePaneButton;
                tt.text = i18n::S().tooltip_pane_refresh;
            }
            else if (new_file_hover >= 0 && new_file_hover < static_cast<int>(file_explorer_.GetEntries().size())) {
                tt.zone = TooltipTarget::Zone::FilePaneItem;
                tt.text = file_explorer_.GetEntries()[new_file_hover].full_path;
            }
            UpdateTooltip(tt, px, py);
        }
        break;
    }
    case PaneZone::TocPane: {
        const float header_h = renderer_.GetTheme().pane_header_height;
        const bool close_hit = HitPaneHeaderButton(dip_x, dip_y, pane_layout.toc_rect, header_h, PaneCloseButtonRect);
        SetCursor(close_hit ? cursor_hand_ : cursor_arrow_);
        if (panes_.SetTocCloseHovered(close_hit)) {
            renderer_.InvalidateTocPaneCache();
            InvalidatePane(pane_layout.toc_rect);
        }
        const float content_top = pane_layout.toc_rect.y + header_h;
        const float local_y = dip_y - content_top + panes_.TocScroll().scroll_y;
        new_toc_hover = doc_.GetToc().HitTest(local_y, renderer_.GetTheme().pane_item_height);
        // 目次ペインのツールチップ
        {
            TooltipTarget tt;
            if (close_hit) {
                tt.zone = TooltipTarget::Zone::TocPaneButton;
                tt.text = i18n::S().tooltip_pane_close;
            }
            else if (new_toc_hover >= 0 && new_toc_hover < static_cast<int>(doc_.GetToc().GetEntries().size())) {
                tt.zone = TooltipTarget::Zone::TocPaneItem;
                tt.text = doc_.GetToc().GetEntries()[new_toc_hover].text;
            }
            UpdateTooltip(tt, px, py);
        }
        break;
    }
    case PaneZone::MdPane:
        HandleMdPaneHover(dip_x, dip_y, px, py, pane_layout);
        break;
    default:
        SetCursor(cursor_arrow_);
        UpdateTooltip({}, px, py);
        break;
    }

    if (panes_.SetHoveredFileIndex(new_file_hover)) {
        renderer_.InvalidateFilePaneCache();
        InvalidatePane(pane_layout.file_rect);
    }
    if (panes_.SetHoveredTocIndex(new_toc_hover)) {
        renderer_.InvalidateTocPaneCache();
        InvalidatePane(pane_layout.toc_rect);
    }
}

void App::HandleMdPaneHover(float dip_x, float dip_y, int px, int py, const PaneLayout& pane_layout)
{
    // 検索バー上のホバー処理
    if (search_state_.IsVisible()) {
        const auto& r = pane_layout.md_rect;
        const auto sbl = ComputeSearchBarLayout(r.x, r.width, r.y + r.height, !search_state_.GetQuery().empty());
        const auto old_hover = search_bar_hover_;
        search_bar_hover_ = SearchBarHover::None;

        if (dip_y >= sbl.bar_top) {
            if (PointInRect(dip_x, dip_y, sbl.up_btn)) {
                search_bar_hover_ = SearchBarHover::Up;
            }
            else if (PointInRect(dip_x, dip_y, sbl.down_btn)) {
                search_bar_hover_ = SearchBarHover::Down;
            }
            else if (PointInRect(dip_x, dip_y, sbl.case_btn)) {
                search_bar_hover_ = SearchBarHover::CaseSensitive;
            }
            else if (PointInRect(dip_x, dip_y, sbl.highlight_btn)) {
                search_bar_hover_ = SearchBarHover::Highlight;
            }
            else if (PointInRect(dip_x, dip_y, sbl.close_btn)) {
                search_bar_hover_ = SearchBarHover::Close;
            }

            if (PointInRect(dip_x, dip_y, sbl.input_rect)) {
                SetCursor(cursor_ibeam_);
            }
            else {
                SetCursor(cursor_arrow_);
            }
            if (search_bar_hover_ != old_hover) {
                Invalidate();
            }
            // 検索バーボタンのツールチップ
            {
                const auto& ls = i18n::S();
                TooltipTarget tt;
                switch (search_bar_hover_) {
                case SearchBarHover::Up:            tt = { TooltipTarget::Zone::SearchBarButton, ls.tooltip_search_prev }; break;
                case SearchBarHover::Down:          tt = { TooltipTarget::Zone::SearchBarButton, ls.tooltip_search_next }; break;
                case SearchBarHover::CaseSensitive: tt = { TooltipTarget::Zone::SearchBarButton, ls.tooltip_search_case }; break;
                case SearchBarHover::Highlight:     tt = { TooltipTarget::Zone::SearchBarButton, ls.tooltip_search_highlight }; break;
                case SearchBarHover::Close:         tt = { TooltipTarget::Zone::SearchBarButton, ls.tooltip_search_close }; break;
                default: break;
                }
                UpdateTooltip(tt, px, py);
            }
            return;
        }

        if (old_hover != SearchBarHover::None) {
            Invalidate();
        }
    }

    // スクロールバー領域では矢印カーソル
    if (IsOverMdScrollbar(dip_x, dip_y, pane_layout)) {
        SetCursor(cursor_arrow_);
        UpdateTooltip({}, px, py);
        return;
    }

    const auto nav_hit = hit_test_.NavButtonHitTest(dip_x, dip_y, pane_layout.md_rect);
    const auto old_nav_hover = nav_hover_;
    nav_hover_ = nav_hit;
    if (nav_hit != NavButtonHover::None) {
        hovered_copy_node_ = -1;
        SetCursor(cursor_hand_);
        if (nav_hit != old_nav_hover) {
            InvalidateMdPane(pane_layout.md_rect);
        }
        // ナビゲーションボタンのツールチップ
        {
            TooltipTarget tt;
            tt.zone = TooltipTarget::Zone::NavButton;
            if (nav_hit == NavButtonHover::Back) {
                tt.text = i18n::S().tooltip_nav_back;
            }
            else {
                tt.text = i18n::S().tooltip_nav_forward;
            }
            UpdateTooltip(tt, px, py);
        }
        return;
    }
    if (old_nav_hover != NavButtonHover::None) {
        InvalidateMdPane(pane_layout.md_rect);
    }

    // コピーボタンのホバー判定（距離スロットリングで不要な再計算を回避）
    {
        const int cdx = px - last_copy_hit_pos_.x;
        const int cdy = py - last_copy_hit_pos_.y;
        if (cdx * cdx + cdy * cdy > HOVER_THROTTLE_DISTANCE_SQ) {
            last_copy_hit_pos_ = { px, py };
            const float content_width = renderer_.GetTheme().ContentWidth(pane_layout.md_rect.width);
            const int old_copy_hover = hovered_copy_node_;
            hovered_copy_node_ = hit_test_.CopyButtonHitTest(
                doc_.GetNodes(), layout_cache_, renderer_.GetTheme(),
                viewport_.GetScrollY(), pane_layout.md_rect.x,
                content_width, pane_layout.md_rect.height,
                cached_dpi_scale_, px, py);
            if (hovered_copy_node_ != old_copy_hover) {
                InvalidateMdPane(pane_layout.md_rect);
            }
        }
    }
    if (hovered_copy_node_ >= 0) {
        SetCursor(cursor_hand_);
        UpdateTooltip({ TooltipTarget::Zone::CopyButton, i18n::S().tooltip_copy }, px, py);
        return;
    }

    // リンク・画像のヒットテスト
    const int dx = px - last_md_hit_pos_.x;
    const int dy = py - last_md_hit_pos_.y;
    if (dx * dx + dy * dy > HOVER_THROTTLE_DISTANCE_SQ) {
        const auto hit = HitTest(px, py);
        const auto link = GetLinkAtHit(hit);
        last_md_cursor_hand_ = link.has_value();
        last_md_hit_pos_ = { px, py };

        // リンクまたは画像のツールチップ
        TooltipTarget tt;
        if (link.has_value()) {
            tt.zone = TooltipTarget::Zone::MdLink;
            tt.text = *link;
        }
        else if (hit.node_index >= 0) {
            const auto& nodes = doc_.GetNodes();
            const auto& node = nodes[hit.node_index];
            if (node.type == NodeType::Image && node.has_image()) {
                tt.zone = TooltipTarget::Zone::MdImage;
                if (!node.text.empty()) {
                    tt.text = node.text;
                    tt.text += L"\n";
                }
                tt.text += node.image_data->src;
            }
        }
        UpdateTooltip(tt, px, py);
    }
    SetCursor(last_md_cursor_hand_ ? cursor_hand_ : cursor_ibeam_);
}

void App::OnLButtonDblClk(int px, int py)
{
    if (!renderer_.GetRenderTarget()) {
        return;
    }
    const auto dip = PixelToDip(px, py);
    const auto zone = PaneAtPoint(dip.x, dip.y);
    if (zone != PaneZone::MdPane) {
        return;
    }
    const auto hit = HitTest(px, py);
    if (hit.node_index < 0) {
        return;
    }
    const auto& text = doc_.GetNodes()[hit.node_index].text;
    if (text.empty()) {
        return;
    }
    const auto wb = FindWordBoundaries(text, hit.text_pos);
    if (!wb.found) {
        return;
    }

    viewport_.SetAnchor(hit.node_index, wb.start);
    viewport_.SetSelection(TextSelection::MakeOrdered(hit.node_index, wb.start, hit.node_index, wb.end));
    const auto layout = GetPaneLayout();
    InvalidateMdPane(layout.md_rect);
}

// ============================================================
// 選択 / クリップボード
// ============================================================

void App::ClearSelection()
{
    viewport_.ClearSelection();
    const auto layout = GetPaneLayout();
    InvalidateMdPane(layout.md_rect);
}

void App::SelectAll()
{
    viewport_.SelectAll(doc_.GetNodes());
    const auto layout = GetPaneLayout();
    InvalidateMdPane(layout.md_rect);
}

void App::SetClipboardText(std::wstring_view text) const
{
    if (text.empty()) {
        return;
    }
    if (!OpenClipboard(hwnd_)) {
        return;
    }
    EmptyClipboard();

    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    const HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem) {
        void* ptr = GlobalLock(hMem);
        if (ptr) {
            memcpy(ptr, text.data(), text.size() * sizeof(wchar_t));
            static_cast<wchar_t*>(ptr)[text.size()] = L'\0';
            GlobalUnlock(hMem);
            if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
                GlobalFree(hMem);
            }
        }
        else {
            GlobalFree(hMem);
        }
    }
    CloseClipboard();
}

void App::CopySelectionToClipboard() const
{
    if (!viewport_.GetSelection().active) {
        return;
    }
    const std::pmr::wstring result = ExtractSelectedText(doc_.GetNodes(), viewport_.GetSelection());
    SetClipboardText(result);
}

void App::CopyCodeBlockToClipboard(int node_index) const
{
    const auto& nodes = doc_.GetNodes();
    if (node_index < 0 || node_index >= static_cast<int>(nodes.size())) {
        return;
    }
    SetClipboardText(nodes[node_index].text);
}

bool App::IsOverMdScrollbar(float dip_x, float dip_y, const PaneLayout& layout) const noexcept
{
    if (!layout_service_) {
        return false;
    }
    const float total_h = layout_service_->GetTotalHeight();
    const float viewport_h = layout.md_rect.height;
    if (total_h <= viewport_h || viewport_h <= 0.0f) {
        return false;
    }
    if (dip_y < layout.md_rect.y || dip_y > layout.md_rect.y + viewport_h) {
        return false;
    }
    const float md_right = layout.md_rect.x + layout.md_rect.width;
    const float sb_left = md_right - PANE_SCROLLBAR_WIDTH - PANE_SCROLLBAR_MARGIN;
    const float sb_right = md_right - PANE_SCROLLBAR_MARGIN;
    return dip_x >= sb_left - PANE_SCROLLBAR_HIT_PADDING && dip_x <= sb_right;
}

bool App::IsOverMdScrollbar(float dip_x, float dip_y) const
{
    return IsOverMdScrollbar(dip_x, dip_y, GetPaneLayout());
}
