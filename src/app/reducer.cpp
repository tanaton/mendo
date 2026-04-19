#include "reducer.h"
#include "timer_ids.h"
#include "document_utils.h"
#include "ui_constants.h"
#include "utility.h"

AnchorState SaveAnchorFromState(const AppState& state) noexcept
{
    AnchorState a;
    a.idx = state.view.viewport.FindFirstVisibleNode(state.document.layout_cache, state.document.doc.GetNodes().size());
    a.y_before = (a.idx >= 0) ? state.document.layout_cache[a.idx].y_position : 0.0f;
    a.offset = state.view.viewport.GetScrollY() - a.y_before;
    return a;
}

// ============================================================
// 共通ヘルパー
// ============================================================

namespace {

void ClearTooltip(AppState& state, SideEffectList& effects)
{
    state.interaction.tooltip.Hide();
    state.interaction.tooltip.ResetTarget();
    effects.emplace_back(effect::ClearTooltip{});
}

void EmitScrollEffects(AppState& state, SideEffectList& effects, float old_scroll)
{
    if (state.view.viewport.GetScrollY() != old_scroll) {
        ClearTooltip(state, effects);
        state.interaction.hover_throttle.Reset();
        effects.emplace_back(effect::InvalidateWindow{});
        effects.emplace_back(effect::BitmapManage{});
    }
}

float CalcPaneMaxScroll(const AppState& state, PaneZone pane)
{
    const float item_h = state.window.cached_theme.pane_item_height;
    const float header_h = state.window.cached_theme.pane_header_height;
    if (pane == PaneZone::FilePane) {
        const float total = static_cast<float>(state.file_explorer.GetEntries().size()) * item_h;
        return std::max(0.0f, total - (state.cached_pane_layout.file_rect.height - header_h));
    }
    else {
        const float total = static_cast<float>(state.document.doc.GetToc().GetEntries().size()) * item_h;
        return std::max(0.0f, total - (state.cached_pane_layout.toc_rect.height - header_h));
    }
}

// ============================================================
// ナビゲーション
// ============================================================

void ApplyNavResult(AppState& state, SideEffectList& effects, NavEntry&& entry)
{
    if (entry.file_path != state.document.doc.GetFilePath() && !entry.file_path.empty()) {
        state.view.scroll_restore.pending_nav_scroll_y = entry.scroll_y;
        effects.emplace_back(effect::LoadFile{ std::move(entry.file_path) });
    }
    else {
        state.view.scroll_restore.pending_restore_scroll_y = -1;
        state.view.viewport.ScrollTo(entry.scroll_y);
        state.interaction.hover_throttle.Reset();
        ClearTooltip(state, effects);
        effects.emplace_back(effect::InvalidateWindow{});
        effects.emplace_back(effect::BitmapManage{});
    }
}

void ReduceNavigateBack(AppState& state, SideEffectList& effects)
{
    NavEntry out;
    if (state.view.nav_history.GoBack(NavEntry{ state.document.doc.GetFilePath(), state.view.viewport.GetScrollY() }, out)) {
        ApplyNavResult(state, effects, std::move(out));
    }
}

void ReduceNavigateForward(AppState& state, SideEffectList& effects)
{
    NavEntry out;
    if (state.view.nav_history.GoForward(NavEntry{ state.document.doc.GetFilePath(), state.view.viewport.GetScrollY() }, out)) {
        ApplyNavResult(state, effects, std::move(out));
    }
}

// ============================================================
// スクロール系アクション
// ============================================================

void ReduceKeyScroll(AppState& state, SideEffectList& effects, const KeyScrollAction& a)
{
    state.view.scroll_restore.pending_restore_scroll_y = -1;
    const float old_scroll = state.view.viewport.GetScrollY();
    const float page_size = state.cached_pane_layout.md_rect.height;
    switch (a.type) {
    case ScrollType::LineUp:
        state.view.viewport.DirectScrollBy(-SCROLL_LINE_AMOUNT);
        break;
    case ScrollType::LineDown:
        state.view.viewport.DirectScrollBy(SCROLL_LINE_AMOUNT);
        break;
    case ScrollType::PageUp:
        state.view.viewport.DirectScrollBy(-page_size * SCROLL_PAGE_FACTOR);
        break;
    case ScrollType::PageDown:
        state.view.viewport.DirectScrollBy(page_size * SCROLL_PAGE_FACTOR);
        break;
    case ScrollType::Home:
        state.view.viewport.ScrollTo(0.0f);
        break;
    case ScrollType::End:
        state.view.viewport.ScrollTo(state.view.viewport.GetMaxScroll());
        break;
    default:
        break;
    }
    EmitScrollEffects(state, effects, old_scroll);
}

void ReduceDirectScrollBy(AppState& state, SideEffectList& effects, const DirectScrollByAction& a)
{
    state.view.scroll_restore.pending_restore_scroll_y = -1;
    const float old_scroll = state.view.viewport.GetScrollY();
    state.view.viewport.DirectScrollBy(a.delta);
    EmitScrollEffects(state, effects, old_scroll);
}

void ReduceScrollPane(AppState& state, SideEffectList& effects, const ScrollPaneAction& a)
{
    if (a.pane == PaneZone::FilePane) {
        if (state.view.panes.ScrollFilePaneBy(a.delta, CalcPaneMaxScroll(state, PaneZone::FilePane))) {
            effects.emplace_back(effect::InvalidatePaneCache{ PaneZone::FilePane });
            effects.emplace_back(effect::InvalidateWindow{});
        }
    }
    else if (a.pane == PaneZone::TocPane) {
        if (state.view.panes.ScrollTocPaneBy(a.delta, CalcPaneMaxScroll(state, PaneZone::TocPane))) {
            effects.emplace_back(effect::InvalidatePaneCache{ PaneZone::TocPane });
            effects.emplace_back(effect::InvalidateWindow{});
        }
    }
}

// ============================================================
// ペイン・選択・クリップボード
// ============================================================

void ReduceTogglePane(AppState& state, SideEffectList& effects, const TogglePaneAction& a)
{
    switch (a.target) {
    case PaneTarget::File: state.view.panes.ToggleFilePane(); break;
    case PaneTarget::Toc:  state.view.panes.ToggleTocPane();  break;
    }
    state.pane_layout_valid = false;
    effects.emplace_back(effect::RefreshPaneLayout{});
}

void ReduceSelectAll(AppState& state, SideEffectList& effects)
{
    state.view.viewport.SelectAll(state.document.doc.GetNodes());
    effects.emplace_back(effect::InvalidateWindow{});
}

void ReduceClearSelection(AppState& state, SideEffectList& effects)
{
    if (state.search.search_state.IsVisible()) {
        state.search.search_state.Hide();
    }
    else {
        state.view.viewport.ClearSelection();
    }
    effects.emplace_back(effect::InvalidateWindow{});
}

void ReduceCopyClipboard(const AppState& state, SideEffectList& effects)
{
    if (state.view.viewport.GetSelection().active) {
        effects.emplace_back(effect::ClipboardWrite{
            ExtractSelectedText(state.document.doc.GetNodes(), state.view.viewport.GetSelection()) });
    }
}

void ReduceCopyFormattedClipboard(const AppState& state, SideEffectList& effects)
{
    const auto& sel = state.view.viewport.GetSelection();
    if (!sel.active) {
        return;
    }
    const auto& nodes = state.document.doc.GetNodes();
    effects.emplace_back(effect::ClipboardWriteHtml{
        ExtractSelectedTextAsHtml(nodes, sel),
        ExtractSelectedText(nodes, sel)
    });
}

// ============================================================
// ズーム・テーマ
// ============================================================

void ReduceZoom(AppState& state, SideEffectList& effects, const ZoomAction& a)
{
    float new_zoom = 0.0f;
    switch (a.direction) {
    case ZoomDirection::In:
        new_zoom = state.view.viewport.ZoomIn();
        break;
    case ZoomDirection::Out:
        new_zoom = state.view.viewport.ZoomOut();
        break;
    case ZoomDirection::Reset:
        new_zoom = state.view.viewport.ZoomReset();
        break;
    }
    if (new_zoom <= 0.0f) {
        return;
    }
    const auto anchor = SaveAnchorFromState(state);
    state.pane_layout_valid = false;
    const float zoom_ratio = new_zoom / state.window.cached_theme.zoom;
    state.view.panes.ApplyZoom(zoom_ratio);
    state.document.layout_cache.InvalidateAllLayouts();
    effects.emplace_back(effect::ApplyThemeChange{
        .type = effect::ApplyThemeChange::Type::Zoom,
        .anchor_idx = anchor.idx,
        .anchor_y_before = anchor.y_before,
        .anchor_offset = anchor.offset,
        .offset_scale = zoom_ratio,
        .new_zoom = new_zoom,
        .zoom_index = state.view.viewport.GetZoomIndex(),
        });
}

void ReduceToggleDarkMode(AppState& state, SideEffectList& effects)
{
    const auto anchor = SaveAnchorFromState(state);
    state.pane_layout_valid = false;
    state.document.layout_cache.InvalidateAllWithDiagrams(state.document.doc.GetNodes());
    effects.emplace_back(effect::ApplyThemeChange{
        .type = effect::ApplyThemeChange::Type::DarkMode,
        .anchor_idx = anchor.idx,
        .anchor_y_before = anchor.y_before,
        .anchor_offset = anchor.offset,
        .offset_scale = 1.0f,
        .new_zoom = 0.0f,
        .zoom_index = state.view.viewport.GetZoomIndex(),
        });
}

// ============================================================
// ウィンドウ・システムイベント
// ============================================================

void ReduceActivate(AppState& state, SideEffectList& effects, const ActivateAction& a)
{
    if (state.window.window_active != a.active) {
        state.window.window_active = a.active;
        effects.emplace_back(effect::InvalidateTitleBar{});
    }
    if (!a.active) {
        ClearTooltip(state, effects);
    }
}

void ReduceResize(AppState& state, SideEffectList& effects, const ResizeAction& a)
{
    if (a.width == 0 || a.height == 0) {
        return;
    }
    state.pane_layout_valid = false;
    effects.emplace_back(effect::RendererResize{ a.width, a.height });
    const float window_w_dip = a.width / state.window.cached_dpi_scale;
    state.window.titlebar.UpdateLayout(window_w_dip);
    if (state.window.is_sizing) {
        effects.emplace_back(effect::PerformSizingUpdate{});
    }
    else {
        effects.emplace_back(effect::PerformResizeEnd{});
    }
}

void ReduceDpiChanged(AppState& state, SideEffectList& effects, const DpiChangedAction& a)
{
    state.window.cached_dpi_scale = static_cast<float>(a.dpi) / 96.0f;
    if (state.window.cached_dpi_scale <= 0.0f) {
        state.window.cached_dpi_scale = 1.0f;
    }
    state.pane_layout_valid = false;
    state.document.layout_cache.MarkAllDirty();
    effects.emplace_back(effect::RendererSetDpi{ static_cast<float>(a.dpi) });
    effects.emplace_back(effect::ClearFileCache{});
    effects.emplace_back(effect::SetWindowPosition{
        static_cast<int>(a.suggested.left),
        static_cast<int>(a.suggested.top),
        static_cast<int>(a.suggested.right - a.suggested.left),
        static_cast<int>(a.suggested.bottom - a.suggested.top),
        });
}

void ReduceHWheel(AppState& state, SideEffectList& effects, const HWheelAction& a)
{
    const bool had_overlay = state.interaction.swipe_detector.IsOverlayVisible();
    const int old_direction = state.interaction.swipe_detector.GetOverlayDirection();
    state.interaction.swipe_detector.OnHWheel(a.delta, a.tick);
    effects.emplace_back(effect::SetTimer{ app_timer::SWIPE_OVERLAY,
        static_cast<UINT>(SwipeDetector::COMMIT_TIMEOUT_MS) });
    if (had_overlay != state.interaction.swipe_detector.IsOverlayVisible()
        || old_direction != state.interaction.swipe_detector.GetOverlayDirection()) {
        effects.emplace_back(effect::InvalidateWindow{});
    }
}

// ============================================================
// 検索系アクション
// ============================================================

void ReduceSearchStep(AppState& state, bool forward)
{
    if (state.search.search_state.IsVisible()) {
        forward ? state.search.search_bar_ctrl.OnNext()
                : state.search.search_bar_ctrl.OnPrev();
    }
    else {
        state.search.search_bar_ctrl.OnOpen(state.document.doc.GetNodes());
    }
}

void ReduceCaptureChanged(AppState& state, SideEffectList& effects)
{
    state.search.search_bar_ctrl.OnCaptureChanged();
    if (state.interaction.gesture.GetPhase() != GesturePhase::Idle) {
        state.interaction.gesture.Reset();
        effects.emplace_back(effect::InvalidateWindow{});
    }
}

// ============================================================
// ファイル・ナビゲーション系アクション
// ============================================================

void ReduceDropFiles(AppState& state, SideEffectList& effects, const DropFilesAction& a)
{
    if (!state.document.doc.GetFilePath().empty()) {
        state.view.nav_history.Push(NavEntry{ state.document.doc.GetFilePath(), state.view.viewport.GetScrollY() });
    }
    effects.emplace_back(effect::LoadFile{ std::pmr::wstring(a.path) });
}

void ReduceShowHelp(AppState& state, SideEffectList& effects)
{
    if (!state.document.doc.GetFilePath().empty() && !IsHelpPath(state.document.doc.GetFilePath())) {
        state.view.nav_history.Push(NavEntry{ state.document.doc.GetFilePath(), state.view.viewport.GetScrollY() });
    }
    effects.emplace_back(effect::LoadFile{ std::pmr::wstring(HELP_PATH) });
}

// ============================================================
// タイマー
// ============================================================

void ReduceTimer(AppState& state, SideEffectList& effects, const TimerAction& a)
{
    switch (a.timer_id) {
    case app_timer::TOAST:
        if (!state.interaction.toast.Tick()) {
            effects.emplace_back(effect::KillTimer{ app_timer::TOAST });
        }
        effects.emplace_back(effect::InvalidateWindow{});
        break;
    case app_timer::SEARCH_CARET:
        state.search.search_bar_ctrl.OnCaretBlinkTimer();
        break;
    case app_timer::TOOLTIP:
        effects.emplace_back(effect::KillTimer{ app_timer::TOOLTIP });
        state.interaction.tooltip.Show();
        break;
    case app_timer::SEARCH_DEBOUNCE:
        state.search.search_bar_ctrl.OnDebounceTimer(state.document.doc.GetNodes());
        break;
    case app_timer::SWIPE_OVERLAY: {
        const auto result = state.interaction.swipe_detector.Commit();
        effects.emplace_back(effect::KillTimer{ app_timer::SWIPE_OVERLAY });
        switch (result) {
        case SwipeResult::Back:
            ReduceNavigateBack(state, effects);
            effects.emplace_back(effect::InvalidateWindow{});
            break;
        case SwipeResult::Forward:
            ReduceNavigateForward(state, effects);
            effects.emplace_back(effect::InvalidateWindow{});
            break;
        default:
            break;
        }
        break;
    }
    case app_timer::DEFERRED_LAYOUT:
        effects.emplace_back(effect::ProcessDeferredLayout{});
        break;
    case app_timer::LOADING_ANIM:
        effects.emplace_back(effect::TickLoadingAnimation{});
        effects.emplace_back(effect::InvalidateWindow{});
        break;
    case app_timer::MERMAID_BATCH:
        effects.emplace_back(effect::ProcessMermaidBatchTimer{});
        break;
    case app_timer::BITMAP_MANAGE:
        effects.emplace_back(effect::ProcessBitmapManage{});
        break;
    case app_timer::MERMAID_INIT_RETRY:
        effects.emplace_back(effect::MermaidInitRetry{});
        break;
    case app_timer::FILE_RELOAD_DEBOUNCE:
        effects.emplace_back(effect::KillTimer{ app_timer::FILE_RELOAD_DEBOUNCE });
        effects.emplace_back(effect::ReloadFile{});
        break;
    default:
        break;
    }
}

} // namespace

// ============================================================
// Reducer: メインディスパッチ
// ============================================================

SideEffectList Reduce(AppState& state, const AppAction& action)
{
    SideEffectList effects;

    std::visit(overloaded{
        [](const NoOpAction&) {},

        // ---- スクロール ----
        [&](const KeyScrollAction& a) { ReduceKeyScroll(state, effects, a); },
        [&](const DirectScrollByAction& a) { ReduceDirectScrollBy(state, effects, a); },
        [&](const ScrollPaneAction& a) { ReduceScrollPane(state, effects, a); },

        // ---- ペイン・選択・クリップボード ----
        [&](const TogglePaneAction& a) { ReduceTogglePane(state, effects, a); },
        [&](const SelectAllAction&) { ReduceSelectAll(state, effects); },
        [&](const ClearSelectionAction&) { ReduceClearSelection(state, effects); },
        [&](const CopyClipboardAction&) { ReduceCopyClipboard(state, effects); },
        [&](const CopyFormattedClipboardAction&) { ReduceCopyFormattedClipboard(state, effects); },

        // ---- ズーム・テーマ ----
        [&](const ZoomAction& a) { ReduceZoom(state, effects, a); },
        [&](const ToggleDarkModeAction&) { ReduceToggleDarkMode(state, effects); },

        // ---- ウィンドウ・システムイベント ----
        [&](const ActivateAction& a) { ReduceActivate(state, effects, a); },
        [&](const EnterSizeMoveAction&) { state.window.is_sizing = true; },
        [&](const ExitSizeMoveAction&) {
            state.window.is_sizing = false;
            effects.emplace_back(effect::PerformResizeEnd{});
        },
        [&](const ResizeAction& a) { ReduceResize(state, effects, a); },
        [&](const DpiChangedAction& a) { ReduceDpiChanged(state, effects, a); },
        [&](const HWheelAction& a) { ReduceHWheel(state, effects, a); },

        // ---- 検索 ----
        [&](const OpenSearchBarAction&) { state.search.search_bar_ctrl.OnOpen(state.document.doc.GetNodes()); },
        [&](const CloseSearchBarAction&) { state.search.search_bar_ctrl.OnClose(); },
        [&](const SearchNextAction&) { ReduceSearchStep(state, true); },
        [&](const SearchPrevAction&) { ReduceSearchStep(state, false); },
        [&](const SearchTextChangedAction& a) { state.search.search_bar_ctrl.OnTextChanged(a.text, state.document.doc.GetNodes()); },
        [&](const ToggleCaseSensitiveAction&) { state.search.search_bar_ctrl.OnToggleCaseSensitive(state.document.doc.GetNodes()); },
        [&](const ToggleHighlightAction&) { state.search.search_bar_ctrl.OnToggleHighlight(); },
        [&](const SearchSelectionAction& a) { state.search.search_bar_ctrl.SetSelection(a.sel_start, a.sel_end); },
        [&](const ImeCompositionAction& a) { state.search.search_bar_ctrl.SetImeComposition(a.text); },

        // ---- マウスイベント（SideEffect経由で委譲） ----
        [&](const MouseLeaveAction&) {
            // 再侵入時に同一座標の最初の WM_MOUSEMOVE が ShouldSkipSameDispatch で
            // 弾かれるとカーソル/ツールチップの復帰が遅れるため、hover 状態もリセットする
            state.interaction.hover_throttle.Reset();
            ClearTooltip(state, effects);
        },
        [&](const CaptureChangedAction&) { ReduceCaptureChanged(state, effects); },
        [&](const LButtonDownAction& a) { effects.emplace_back(effect::HandleMouseEvent{ effect::MouseEventType::LButtonDown, a.px, a.py }); },
        [&](const LButtonUpAction& a) { effects.emplace_back(effect::HandleMouseEvent{ effect::MouseEventType::LButtonUp, a.px, a.py }); },
        [&](const MouseMoveAction& a) { effects.emplace_back(effect::HandleMouseEvent{ effect::MouseEventType::MouseMove, a.px, a.py }); },
        [&](const MouseHoverAction& a) { effects.emplace_back(effect::HandleMouseEvent{ effect::MouseEventType::MouseHover, a.px, a.py }); },
        [&](const LButtonDblClkAction& a) { effects.emplace_back(effect::HandleMouseEvent{ effect::MouseEventType::LButtonDblClk, a.px, a.py }); },
        [&](const RButtonDownAction& a) { effects.emplace_back(effect::HandleMouseEvent{ effect::MouseEventType::RButtonDown, a.px, a.py }); },
        [&](const RButtonUpAction& a) { effects.emplace_back(effect::HandleMouseEvent{ effect::MouseEventType::RButtonUp, a.px, a.py }); },
        [&](const RButtonMoveAction& a) { effects.emplace_back(effect::HandleMouseEvent{ effect::MouseEventType::RButtonMove, a.px, a.py }); },
        [&](const ContextMenuAction& a) { effects.emplace_back(effect::HandleContextMenu{ a.screen_x, a.screen_y }); },

        // ---- ナビゲーション ----
        [&](const NavigateBackAction&) { ReduceNavigateBack(state, effects); },
        [&](const NavigateForwardAction&) { ReduceNavigateForward(state, effects); },
        [&](const XButtonBackAction&) { ReduceNavigateBack(state, effects); },
        [&](const XButtonForwardAction&) { ReduceNavigateForward(state, effects); },

        // ---- ファイル操作 ----
        [&](const DropFilesAction& a) { ReduceDropFiles(state, effects, a); },
        [&](const ShowHelpAction&) { ReduceShowHelp(state, effects); },
        [&](const OpenFileAction&) { effects.emplace_back(effect::OpenFileDialog{}); },
        [&](const ReloadFileAction&) { effects.emplace_back(effect::ReloadFile{}); },

        // ---- 非同期・タイマー ----
        [&](const TimerAction& a) { ReduceTimer(state, effects, a); },
        [&](const FileWatchAction&) { effects.emplace_back(effect::CheckFileChanges{}); },
        [&](const ImageLoadedAction&) { effects.emplace_back(effect::NotifyImageLoaded{}); },
        [&](const ParseCompleteAction&) { effects.emplace_back(effect::HandleParseComplete{}); },

        // ---- ライフサイクル ----
        [&](const DestroyAction&) { effects.emplace_back(effect::Destroy{}); },

        // ---- 未処理のアクション ----
        [](const auto&) {},
        }, action);

    return effects;
}
