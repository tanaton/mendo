#include "app.h"
#include "app_constants.h"
#include "app_events.h"
#include "i18n.h"
#include "pane_layout.h"
#include "document_utils.h"
#include "ui_constants.h"
#include "resource.h"
#include <cwctype>

namespace {

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

// タイトルバーボタンに対応するツールチップを返す。
TooltipTarget BuildTitleBarTooltip(TitleBarHitZone zone, bool is_maximized) noexcept
{
    const auto& ls = i18n::S();
    switch (zone) {
    case TitleBarHitZone::OpenFile:    return { TooltipTarget::Zone::TitleBarButton, ls.tooltip_open_file };
    case TitleBarHitZone::Help:        return { TooltipTarget::Zone::TitleBarButton, ls.tooltip_help };
    case TitleBarHitZone::ThemeToggle: return { TooltipTarget::Zone::TitleBarButton, ls.tooltip_theme_toggle };
    case TitleBarHitZone::Search:      return { TooltipTarget::Zone::TitleBarButton, ls.tooltip_search };
    case TitleBarHitZone::FileToggle:  return { TooltipTarget::Zone::TitleBarButton, ls.tooltip_file_pane };
    case TitleBarHitZone::TocToggle:   return { TooltipTarget::Zone::TitleBarButton, ls.tooltip_toc_pane };
    case TitleBarHitZone::Minimize:    return { TooltipTarget::Zone::TitleBarButton, ls.tooltip_minimize };
    case TitleBarHitZone::Maximize:    return { TooltipTarget::Zone::TitleBarButton, is_maximized ? ls.tooltip_restore : ls.tooltip_maximize };
    case TitleBarHitZone::Close:       return { TooltipTarget::Zone::TitleBarButton, ls.tooltip_close };
    default: return {};
    }
}

// サイドペインのヘッダーボタンホバー処理結果。
struct PaneHoverResult {
    int hovered_index = -1;
    TooltipTarget tooltip;
    bool button_changed = false;
    bool any_button_hit = false;
};

// サイドペインの共通ホバー処理。
// ヘッダーボタンのホバー状態を更新し、コンテンツ領域のアイテムヒットテストを行う。
template<typename SetCloseHoveredFn, typename SetRefreshHoveredFn,
    typename HitTestFn, typename BuildTooltipFn>
PaneHoverResult ProcessSidePaneHover(
    float dip_x, float dip_y,
    const PaneRect& rect, float header_h, float item_height,
    bool has_refresh_btn, float scroll_y,
    SetCloseHoveredFn&& set_close_hovered,
    SetRefreshHoveredFn&& set_refresh_hovered,
    HitTestFn&& hit_test_fn,
    BuildTooltipFn&& build_tooltip)
{
    PaneHoverResult result;

    const bool close_hit = HitPaneHeaderButton(dip_x, dip_y, rect, header_h, PaneCloseButtonRect);
    bool refresh_hit = false;
    if (has_refresh_btn) {
        refresh_hit = HitPaneHeaderButton(dip_x, dip_y, rect, header_h, PaneRefreshButtonRect);
    }
    result.any_button_hit = close_hit || refresh_hit;

    bool changed = set_close_hovered(close_hit);
    if (has_refresh_btn) {
        changed |= set_refresh_hovered(refresh_hit);
    }
    result.button_changed = changed;

    const float content_top = rect.y + header_h;
    const float local_y = dip_y - content_top + scroll_y;
    result.hovered_index = hit_test_fn(local_y, item_height);

    result.tooltip = build_tooltip(close_hit, refresh_hit, result.hovered_index);

    return result;
}

// サイドペインのヘッダー領域のクリック処理を共通化。
// ヘッダー領域内のクリックを消費した場合 true を返す（ボタン外も含む）。
template<typename ToggleFn, typename RefreshFn>
bool ProcessSidePaneHeaderClick(
    float dip_x, float dip_y,
    const PaneRect& rect, float header_h,
    bool has_refresh_btn,
    ToggleFn&& toggle_fn,
    RefreshFn&& refresh_fn)
{
    if (dip_y - rect.y >= header_h) {
        return false;
    }
    if (HitPaneHeaderButton(dip_x, dip_y, rect, header_h, PaneCloseButtonRect)) {
        toggle_fn();
        return true;
    }
    if (has_refresh_btn && HitPaneHeaderButton(dip_x, dip_y, rect, header_h, PaneRefreshButtonRect)) {
        refresh_fn();
        return true;
    }
    return true; // ヘッダー領域内だがボタン外 — 消費済みとして扱う
}

} // namespace

// target が変化した場合にタイマーを再設定し、None なら非表示にする。
void App::UpdateTooltip(const TooltipTarget& target, int px, int py)
{
    POINT screen_pos = { px, py };
    ClientToScreen(hwnd_, &screen_pos);
    if (tooltip_.Update(target, screen_pos)) {
        SetTimer(hwnd_, app_timer::TOOLTIP, TOOLTIP_DELAY_MS, nullptr);
    }
    else if (target.IsEmpty()) {
        KillTimer(hwnd_, app_timer::TOOLTIP);
    }
}

void App::ClearTooltip()
{
    KillTimer(hwnd_, app_timer::TOOLTIP);
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

    if (ProcessSidePaneHeaderClick(dip_x, dip_y, layout.file_rect, theme.pane_header_height, true,
        [this]() { panes_.ToggleFilePane(); RefreshPaneLayout(); },
        [this]() { RefreshFilePane(); })) {
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

    if (ProcessSidePaneHeaderClick(dip_x, dip_y, layout.toc_rect, theme.pane_header_height, false,
        [this]() { panes_.ToggleTocPane(); RefreshPaneLayout(); },
        []() {})) {
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

bool App::HandleTitleBarClick(float dip_x, float dip_y)
{
    if (dip_y >= titlebar_.GetHeight()) {
        return false;
    }

    const auto tb_zone = titlebar_.HitTest(dip_x, dip_y);
    switch (tb_zone) {
    case TitleBarHitZone::OpenFile:    ExecuteAction(OpenFileAction{}); break;
    case TitleBarHitZone::Help:        ExecuteAction(ShowHelpAction{}); break;
    case TitleBarHitZone::Search:      ExecuteAction(OpenSearchBarAction{}); break;
    case TitleBarHitZone::ThemeToggle: ExecuteAction(ToggleDarkModeAction{}); break;
    case TitleBarHitZone::FileToggle:  ExecuteAction(TogglePaneAction{ PaneTarget::File }); break;
    case TitleBarHitZone::TocToggle:   ExecuteAction(TogglePaneAction{ PaneTarget::Toc }); break;
    case TitleBarHitZone::Minimize:    ShowWindow(hwnd_, SW_MINIMIZE); break;
    case TitleBarHitZone::Maximize:    ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE); break;
    case TitleBarHitZone::Close:       PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    default: break;  // タイトルバーの他の領域はWM_NCHITTESTで処理済み
    }
    return true;
}

void App::HandleMdPaneClick(float dip_x, float dip_y, int px, int py, const PaneLayout& pane_layout)
{
    // 検索バーのクリック処理
    if (search_state_.IsVisible()) {
        const auto& r = pane_layout.md_rect;
        const auto sbl = ComputeSearchBarLayout(r.x, r.width, r.y + r.height, !search_state_.GetQuery().empty());
        if (dip_y >= sbl.bar_top) {
            if (PointInRect(dip_x, dip_y, sbl.up_btn)) {
                OnSearchPrev();
                return;
            }
            if (PointInRect(dip_x, dip_y, sbl.down_btn)) {
                OnSearchNext();
                return;
            }
            if (PointInRect(dip_x, dip_y, sbl.case_btn)) {
                OnToggleCaseSensitive();
                return;
            }
            if (PointInRect(dip_x, dip_y, sbl.highlight_btn)) {
                OnToggleHighlight();
                return;
            }
            if (PointInRect(dip_x, dip_y, sbl.close_btn)) {
                OnSearchClose();
                return;
            }
            if (PointInRect(dip_x, dip_y, sbl.input_rect)) {
                const float text_left = sbl.input_rect.left + SEARCH_INPUT_TEXT_PAD_LEFT;
                const float input_w = sbl.input_rect.right - SEARCH_INPUT_TEXT_PAD_RIGHT - text_left;
                const int pos = renderer_.HitTestSearchInput(search_state_.GetQuery(), dip_x - text_left, input_w);
                search_bar_ctrl_.StartDrag(pos);
                SetCapture(hwnd_);
                PostMessage(hwnd_, app_msg::SEARCH_FOCUS, app_param::SEARCH_FOCUS_SET_CARET, static_cast<LPARAM>(pos));
                return;
            }
            PostMessage(hwnd_, app_msg::SEARCH_FOCUS, app_param::SEARCH_FOCUS_SELECT_ALL, 0);
            return;
        }
    }

    const auto nav_hit = hit_test_.NavButtonHitTest(dip_x, dip_y, pane_layout.md_rect);
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
    // 保存ボタンのクリック判定
    const auto save_node = hit_test_.SaveButtonHitTest(
        doc_.GetNodes(), layout_cache_, renderer_.GetTheme(),
        viewport_.GetScrollY(), pane_layout.md_rect.x,
        content_width, pane_layout.md_rect.height,
        cached_dpi_scale_, px, py);
    if (save_node >= 0) {
        SaveDiagramAsPng(save_node);
        return;
    }
    // MDペインスクロールバーのクリック判定
    if (layout_service_) {
        float total_h = layout_service_->GetTotalHeight();
        float viewport_h = pane_layout.md_rect.height;
        if (total_h > viewport_h) {
            const float sb_left = pane_layout.md_rect.x + pane_layout.md_rect.width - PANE_SCROLLBAR_WIDTH - PANE_SCROLLBAR_MARGIN;
            if (dip_x >= sb_left - PANE_SCROLLBAR_HIT_PADDING) {
                SetCapture(hwnd_);
                panes_.StartDrag(PaneController::DragTarget::MdScrollbar);
                viewport_.SetScrollbarTracking(true);
                const auto info = ComputeScrollInfo(pane_layout.md_rect, 0.0f, total_h);
                const float thumb_y = ComputeThumbY(info, viewport_.GetScrollY());
                if (dip_y >= thumb_y && dip_y <= thumb_y + info.thumb_height) {
                    panes_.SetDragScrollOffset(dip_y - thumb_y);
                }
                else {
                    panes_.SetDragScrollOffset(info.thumb_height * 0.5f);
                    const float new_thumb_y = dip_y - panes_.GetDragScrollOffset();
                    ScrollTo(ScrollFromThumbY(info, new_thumb_y));
                    Invalidate();
                }
                return;
            }
        }
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

void App::OnLButtonDown(int px, int py)
{
    if (!renderer_.GetRenderTarget()) {
        return;
    }

    const auto dip = PixelToDip(px, py);

    if (HandleTitleBarClick(dip.x, dip.y)) {
        return;
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
    case PaneZone::MdPane:
        HandleMdPaneClick(dip.x, dip.y, px, py, pane_layout);
        return;
    default:
        return;
    }
}

void App::OnLButtonUp(int px, int py)
{
    if (search_bar_ctrl_.IsDragging()) {
        search_bar_ctrl_.EndDrag();
        ReleaseCapture();
        return;
    }

    ReleaseCapture();

    if (panes_.GetDragTarget() != PaneController::DragTarget::None) {
        const bool was_md_scrollbar = (panes_.GetDragTarget() == PaneController::DragTarget::MdScrollbar);
        if (was_md_scrollbar) {
            viewport_.SetScrollbarTracking(false);
        }
        panes_.EndDrag();
        RECT rc;
        GetClientRect(hwnd_, &rc);
        OnResize(static_cast<UINT>(rc.right - rc.left), static_cast<UINT>(rc.bottom - rc.top));
        if (was_md_scrollbar) {
            resource_manager_.ScheduleBitmapManage();
        }
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
        if (!viewport_.GetSelection().active && (dx * dx + dy * dy) < CLICK_DISTANCE_THRESHOLD_SQ) {
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
    if (search_bar_ctrl_.IsDragging()) {
        const auto layout = GetPaneLayout();
        const auto& r = layout.md_rect;
        const auto sbl = ComputeSearchBarLayout(r.x, r.width, r.y + r.height, !search_state_.GetQuery().empty());
        const float text_left = sbl.input_rect.left + SEARCH_INPUT_TEXT_PAD_LEFT;
        const float input_w = sbl.input_rect.right - SEARCH_INPUT_TEXT_PAD_RIGHT - text_left;
        const int pos = renderer_.HitTestSearchInput(
            search_state_.GetQuery(), dip.x - text_left, input_w);
        if (pos != search_bar_ctrl_.GetCaretPos() ||
            search_bar_ctrl_.GetDragAnchor() != search_bar_ctrl_.GetSelectionStart()) {
            PostMessage(hwnd_, app_msg::SEARCH_FOCUS, app_param::SEARCH_FOCUS_SET_SELECTION,
                MAKELPARAM(search_bar_ctrl_.GetDragAnchor(), pos));
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
        const float total = static_cast<float>(file_explorer_.GetEntries().size()) * renderer_.GetTheme().pane_item_height;
        HandleSidePaneScrollDrag(dip.y, layout.file_rect, total, panes_.FileScroll(), &Renderer::InvalidateFilePaneCache);
        return;
    }

    if (panes_.GetDragTarget() == PaneController::DragTarget::TocScrollbar) {
        const auto layout = GetPaneLayout();
        const float total = static_cast<float>(doc_.GetToc().GetEntries().size()) * renderer_.GetTheme().pane_item_height;
        HandleSidePaneScrollDrag(dip.y, layout.toc_rect, total, panes_.TocScroll(), &Renderer::InvalidateTocPaneCache);
        return;
    }

    if (panes_.GetDragTarget() == PaneController::DragTarget::MdScrollbar) {
        if (layout_service_) {
            const auto layout = GetPaneLayout();
            const float total_h = layout_service_->GetTotalHeight();
            const auto info = ComputeScrollInfo(layout.md_rect, 0.0f, total_h);
            const float new_thumb_y = dip.y - panes_.GetDragScrollOffset();
            ScrollTo(ScrollFromThumbY(info, new_thumb_y));
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
        SetCursor(cursors_.Arrow());
        if (titlebar_.SetHovered(tb_zone)) {
            InvalidateTitleBar();
        }
        UpdateTooltip(BuildTitleBarTooltip(tb_zone, IsZoomed(hwnd_)), px, py);
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
        SetCursor(cursors_.SizeWE());
        UpdateTooltip({}, px, py);
        break;
    case PaneZone::FilePane: {
        const float header_h = renderer_.GetTheme().pane_header_height;
        const auto& entries = file_explorer_.GetEntries();
        const auto hr = ProcessSidePaneHover(dip_x, dip_y,
            pane_layout.file_rect, header_h, renderer_.GetTheme().pane_item_height,
            true, panes_.FileScroll().scroll_y,
            [this](bool v) { return panes_.SetFileCloseHovered(v); },
            [this](bool v) { return panes_.SetFileRefreshHovered(v); },
            [this](float y, float h) { return file_explorer_.HitTest(y, h); },
            [&](bool close_hit, bool refresh_hit, int idx) -> TooltipTarget {
            if (close_hit) {
                return {
                    TooltipTarget::Zone::FilePaneButton,
                    i18n::S().tooltip_pane_close
                };
            }
            if (refresh_hit) {
                return {
                    TooltipTarget::Zone::FilePaneButton,
                    i18n::S().tooltip_pane_refresh
                };
            }
            if (idx >= 0 && idx < static_cast<int>(entries.size())) {
                return {
                    TooltipTarget::Zone::FilePaneItem,
                    entries[idx].full_path
                };
            }
            return {};
        });
        SetCursor(hr.any_button_hit ? cursors_.Hand() : cursors_.Arrow());
        if (hr.button_changed) {
            renderer_.InvalidateFilePaneCache();
            InvalidatePane(pane_layout.file_rect);
        }
        new_file_hover = hr.hovered_index;
        UpdateTooltip(hr.tooltip, px, py);
        break;
    }
    case PaneZone::TocPane: {
        const float header_h = renderer_.GetTheme().pane_header_height;
        const auto& toc_entries = doc_.GetToc().GetEntries();
        const auto hr = ProcessSidePaneHover(dip_x, dip_y,
            pane_layout.toc_rect, header_h, renderer_.GetTheme().pane_item_height,
            false, panes_.TocScroll().scroll_y,
            [this](bool v) { return panes_.SetTocCloseHovered(v); },
            [](bool) { return false; },
            [this](float y, float h) { return doc_.GetToc().HitTest(y, h); },
            [&](bool close_hit, bool, int idx) -> TooltipTarget {
            if (close_hit) { return { TooltipTarget::Zone::TocPaneButton, i18n::S().tooltip_pane_close }; }
            if (idx >= 0 && idx < static_cast<int>(toc_entries.size())) {
                return { TooltipTarget::Zone::TocPaneItem, toc_entries[idx].text };
            }
            return {};
        });
        SetCursor(hr.any_button_hit ? cursors_.Hand() : cursors_.Arrow());
        if (hr.button_changed) {
            renderer_.InvalidateTocPaneCache();
            InvalidatePane(pane_layout.toc_rect);
        }
        new_toc_hover = hr.hovered_index;
        UpdateTooltip(hr.tooltip, px, py);
        break;
    }
    case PaneZone::MdPane:
        HandleMdPaneHover(dip_x, dip_y, px, py, pane_layout);
        break;
    default:
        SetCursor(cursors_.Arrow());
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
        const auto old_hover = search_bar_ctrl_.GetHover();

        if (dip_y >= sbl.bar_top) {
            const auto hover = search_bar_ctrl_.UpdateHover(dip_x, dip_y, sbl);

            if (PointInRect(dip_x, dip_y, sbl.input_rect)) {
                SetCursor(cursors_.IBeam());
            }
            else {
                SetCursor(cursors_.Arrow());
            }
            // 検索バーボタンのツールチップ
            {
                using HZ = SearchBarController::HoverZone;
                const auto& ls = i18n::S();
                TooltipTarget tt;
                switch (hover) {
                case HZ::Up:            tt = { TooltipTarget::Zone::SearchBarButton, ls.tooltip_search_prev }; break;
                case HZ::Down:          tt = { TooltipTarget::Zone::SearchBarButton, ls.tooltip_search_next }; break;
                case HZ::CaseSensitive: tt = { TooltipTarget::Zone::SearchBarButton, ls.tooltip_search_case }; break;
                case HZ::Highlight:     tt = { TooltipTarget::Zone::SearchBarButton, ls.tooltip_search_highlight }; break;
                case HZ::Close:         tt = { TooltipTarget::Zone::SearchBarButton, ls.tooltip_search_close }; break;
                default: break;
                }
                UpdateTooltip(tt, px, py);
            }
            return;
        }

        if (old_hover != SearchBarController::HoverZone::None) {
            search_bar_ctrl_.ResetHover();
            Invalidate();
        }
    }

    // スクロールバー領域では矢印カーソル
    if (IsOverMdScrollbar(dip_x, dip_y, pane_layout)) {
        SetCursor(cursors_.Arrow());
        UpdateTooltip({}, px, py);
        return;
    }

    const auto nav_hit = hit_test_.NavButtonHitTest(dip_x, dip_y, pane_layout.md_rect);
    const auto old_nav_hover = nav_hover_;
    nav_hover_ = nav_hit;
    if (nav_hit != NavButtonHover::None) {
        hovered_copy_node_ = -1;
        hovered_save_node_ = -1;
        SetCursor(cursors_.Hand());
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

    // コピー/保存ボタンのホバー判定（距離スロットリングで不要な再計算を回避）
    const float content_width = renderer_.GetTheme().ContentWidth(pane_layout.md_rect.width);
    {
        const int cdx = px - hover_throttle_.last_copy_hit_pos.x;
        const int cdy = py - hover_throttle_.last_copy_hit_pos.y;
        if (cdx * cdx + cdy * cdy > HOVER_THROTTLE_DISTANCE_SQ) {
            hover_throttle_.last_copy_hit_pos = { px, py };
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
        SetCursor(cursors_.Hand());
        UpdateTooltip({ TooltipTarget::Zone::CopyButton, i18n::S().tooltip_copy }, px, py);
        return;
    }

    {
        const int sdx = px - hover_throttle_.last_save_hit_pos.x;
        const int sdy = py - hover_throttle_.last_save_hit_pos.y;
        if (sdx * sdx + sdy * sdy > HOVER_THROTTLE_DISTANCE_SQ) {
            hover_throttle_.last_save_hit_pos = { px, py };
            const int old_save_hover = hovered_save_node_;
            hovered_save_node_ = hit_test_.SaveButtonHitTest(
                doc_.GetNodes(), layout_cache_, renderer_.GetTheme(),
                viewport_.GetScrollY(), pane_layout.md_rect.x,
                content_width, pane_layout.md_rect.height,
                cached_dpi_scale_, px, py);
            if (hovered_save_node_ != old_save_hover) {
                InvalidateMdPane(pane_layout.md_rect);
            }
        }
    }
    if (hovered_save_node_ >= 0) {
        SetCursor(cursors_.Hand());
        UpdateTooltip({ TooltipTarget::Zone::SaveButton, i18n::S().tooltip_save_image }, px, py);
        return;
    }

    // リンク・画像のヒットテスト
    const int dx = px - hover_throttle_.last_md_hit_pos.x;
    const int dy = py - hover_throttle_.last_md_hit_pos.y;
    if (dx * dx + dy * dy > HOVER_THROTTLE_DISTANCE_SQ) {
        const auto hit = HitTest(px, py);
        const auto link = GetLinkAtHit(hit);
        hover_throttle_.last_md_cursor_hand = link.has_value();
        hover_throttle_.last_md_hit_pos = { px, py };

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
                const auto& alt = node.GetText();
                if (!alt.empty()) {
                    tt.text = alt;
                    tt.text += L"\n";
                }
                tt.text += node.image_data->src;
            }
        }
        UpdateTooltip(tt, px, py);
    }
    SetCursor(hover_throttle_.last_md_cursor_hand ? cursors_.Hand() : cursors_.IBeam());
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
