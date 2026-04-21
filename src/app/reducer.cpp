#include "reducer.h"
#include "app_messages.h"
#include "timer_ids.h"
#include "document_utils.h"
#include "ui_constants.h"
#include "utility.h"
#include <cmath>

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

struct SidePaneContext {
    const PaneRect& rect;
    float total_content;
    PaneScrollInfo info;
    ScrollState& scroll;
    PaneController::DragTarget drag_target;
    PaneZone pane_zone;
};

SidePaneContext GetSidePaneContext(AppState& state, PaneTarget pane)
{
    const float item_h = state.window.cached_theme.pane_item_height;
    const float header_h = state.window.cached_theme.pane_header_height;
    if (pane == PaneTarget::File) {
        const float total = static_cast<float>(state.file_explorer.GetEntries().size()) * item_h;
        return {
            state.cached_pane_layout.file_rect,
            total,
            ComputeScrollInfo(state.cached_pane_layout.file_rect, header_h, total),
            state.view.panes.FileScroll(),
            PaneController::DragTarget::FileScrollbar,
            PaneZone::FilePane,
        };
    }
    const float total = static_cast<float>(state.document.doc.GetToc().GetEntries().size()) * item_h;
    return {
        state.cached_pane_layout.toc_rect,
        total,
        ComputeScrollInfo(state.cached_pane_layout.toc_rect, header_h, total),
        state.view.panes.TocScroll(),
        PaneController::DragTarget::TocScrollbar,
        PaneZone::TocPane,
    };
}

// ============================================================
// ナビゲーション
// ============================================================

void ApplyNavResult(AppState& state, SideEffectList& effects, NavEntry&& entry)
{
    if (entry.file_path != state.document.doc.GetFilePath() && !entry.file_path.empty()) {
        state.view.scroll_restore.SetNodeRestore(entry.node, static_cast<int>(std::lround(entry.offset)));
        effects.emplace_back(effect::LoadFile{ std::move(entry.file_path) });
    }
    else {
        auto& cache = state.document.layout_cache;
        state.view.viewport.SetScrollTarget(entry.node, entry.offset);
        state.view.viewport.ApplyScrollTarget(cache);
        state.interaction.hover_throttle.Reset();
        ClearTooltip(state, effects);
        effects.emplace_back(effect::InvalidateWindow{});
        effects.emplace_back(effect::BitmapManage{});
    }
}

void ReduceNavigateBack(AppState& state, SideEffectList& effects)
{
    NavEntry out;
    if (state.view.nav_history.GoBack(CurrentNavEntry(state), out)) {
        ApplyNavResult(state, effects, std::move(out));
    }
}

void ReduceNavigateForward(AppState& state, SideEffectList& effects)
{
    NavEntry out;
    if (state.view.nav_history.GoForward(CurrentNavEntry(state), out)) {
        ApplyNavResult(state, effects, std::move(out));
    }
}

// ============================================================
// スクロール系アクション
// ============================================================

void ReduceKeyScroll(AppState& state, SideEffectList& effects, const KeyScrollAction& a)
{
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
        state.view.viewport.SetScrollTarget(0, 0.0f);
        state.view.viewport.ApplyScrollTarget(state.document.layout_cache);
        break;
    case ScrollType::End:
        // 末尾は max_scroll に依存するためピクセル指定。target は無効化される
        state.view.viewport.ScrollTo(state.view.viewport.GetMaxScroll());
        break;
    default:
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
    PaneTarget target;
    if (a.pane == PaneZone::FilePane) {
        target = PaneTarget::File;
    }
    else if (a.pane == PaneZone::TocPane) {
        target = PaneTarget::Toc;
    }
    else {
        return;
    }
    const auto ctx = GetSidePaneContext(state, target);
    const bool scrolled = (target == PaneTarget::File)
        ? state.view.panes.ScrollFilePaneBy(a.delta, ctx.info.max_scroll)
        : state.view.panes.ScrollTocPaneBy(a.delta, ctx.info.max_scroll);
    if (scrolled) {
        effects.emplace_back(effect::InvalidatePaneCache{ ctx.pane_zone });
        effects.emplace_back(effect::InvalidateWindow{});
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
        ExtractSelectedTextAsHtml(nodes, sel, state.window.cached_theme.is_dark),
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
    const auto anchor = SnapshotVisibleTarget(state);
    state.pane_layout_valid = false;
    const float zoom_ratio = new_zoom / state.window.cached_theme.zoom;
    state.view.panes.ApplyZoom(zoom_ratio);
    state.document.layout_cache.InvalidateAllLayouts();
    if (anchor.IsValid()) {
        // offset もズーム比でスケールし、ノード内の同じ位置が可視先頭に留まるようにする
        state.view.viewport.SetScrollTarget(anchor.node, anchor.offset * zoom_ratio);
    }
    effects.emplace_back(effect::ApplyThemeChange{
        .type = effect::ApplyThemeChange::Type::Zoom,
        .new_zoom = new_zoom,
        .zoom_index = state.view.viewport.GetZoomIndex(),
        });
}

void ReduceToggleDarkMode(AppState& state, SideEffectList& effects)
{
    const auto anchor = SnapshotVisibleTarget(state);
    state.pane_layout_valid = false;
    state.document.layout_cache.InvalidateAllWithDiagrams(state.document.doc.GetNodes());
    if (anchor.IsValid()) {
        // Mermaid 再レンダリングで微小な高さ変化が起きるので target で追従する
        state.view.viewport.SetScrollTarget(anchor.node, anchor.offset);
    }
    effects.emplace_back(effect::ApplyThemeChange{
        .type = effect::ApplyThemeChange::Type::DarkMode,
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

void ReduceMdPaneNavHover(AppState& state, SideEffectList& effects, const MdPaneNavHoverAction& a)
{
    if (state.interaction.nav_hover == a.nav_hover) {
        return;
    }
    state.interaction.nav_hover = a.nav_hover;
    // ナビボタンホバー時は、コピー/保存ボタンホバー状態をクリア（重ならないように）
    if (a.nav_hover != NavButtonHover::None) {
        state.interaction.hovered_copy_node = -1;
        state.interaction.hovered_save_node = -1;
    }
    effects.emplace_back(effect::InvalidateWindow{});
}

void ReduceMdPaneButtonHoverChanged(AppState& state, SideEffectList& effects, const MdPaneButtonHoverChangedAction& a)
{
    if (state.interaction.hovered_copy_node == a.hovered_copy_node
        && state.interaction.hovered_save_node == a.hovered_save_node) {
        return;
    }
    state.interaction.hovered_copy_node = a.hovered_copy_node;
    state.interaction.hovered_save_node = a.hovered_save_node;
    effects.emplace_back(effect::InvalidateWindow{});
}

// ============================================================
// スプリッタードラッグ
// ============================================================

void ReduceSplitterDragStarted(AppState& state, SideEffectList& effects, const SplitterDragStartedAction& a)
{
    if (a.target != PaneController::DragTarget::Splitter1
        && a.target != PaneController::DragTarget::Splitter2) {
        return;
    }
    state.view.panes.StartDrag(a.target);
    effects.emplace_back(effect::SetCapture{});
}

void ReduceSplitterDragMoved(AppState& state, SideEffectList& effects, const SplitterDragMovedAction& a)
{
    const float splitter_w = state.window.cached_theme.splitter_width;
    const float before_file = state.view.panes.GetFilePaneWidth();
    const float before_toc = state.view.panes.GetTocPaneWidth();
    if (a.target == PaneController::DragTarget::Splitter1) {
        state.view.panes.DragSplitter1To(a.dip_x, a.window_width, splitter_w);
    }
    else if (a.target == PaneController::DragTarget::Splitter2) {
        state.view.panes.DragSplitter2To(a.dip_x, a.window_width, splitter_w);
    }
    else {
        return;
    }
    if (state.view.panes.GetFilePaneWidth() == before_file
        && state.view.panes.GetTocPaneWidth() == before_toc) {
        return;
    }
    state.pane_layout_valid = false;
    effects.emplace_back(effect::InvalidateWindow{});
}

void ReduceSplitterDragEnded(AppState& state, SideEffectList& effects)
{
    const auto drag = state.view.panes.GetDragTarget();
    if (drag != PaneController::DragTarget::Splitter1
        && drag != PaneController::DragTarget::Splitter2) {
        return;
    }
    state.view.panes.EndDrag();
    state.pane_layout_valid = false;
    effects.emplace_back(effect::ReleaseCapture{});
    effects.emplace_back(effect::PerformResizeEnd{});
}

// ============================================================
// 検索バー入力ドラッグ
// ============================================================

void ReduceSearchInputDragStarted(AppState& state, SideEffectList& effects, const SearchInputDragStartedAction& a)
{
    state.search.search_bar_ctrl.StartDrag(a.caret_pos);
    effects.emplace_back(effect::SetCapture{});
    effects.emplace_back(effect::PostMessage{
        app_msg::SEARCH_FOCUS,
        app_param::SEARCH_FOCUS_SET_CARET,
        static_cast<LPARAM>(a.caret_pos)
        });
}

void ReduceSearchInputDragMoved(AppState& state, SideEffectList& effects, const SearchInputDragMovedAction& a)
{
    if (!state.search.search_bar_ctrl.IsDragging()) {
        return;
    }
    const auto& ctrl = state.search.search_bar_ctrl;
    if (a.caret_pos == ctrl.GetCaretPos()
        && ctrl.GetDragAnchor() == ctrl.GetSelectionStart()) {
        return;
    }
    effects.emplace_back(effect::PostMessage{
        app_msg::SEARCH_FOCUS,
        app_param::SEARCH_FOCUS_SET_SELECTION,
        MAKELPARAM(ctrl.GetDragAnchor(), a.caret_pos)
        });
}

void ReduceSearchInputDragEnded(AppState& state, SideEffectList& effects)
{
    state.search.search_bar_ctrl.EndDrag();
    effects.emplace_back(effect::ReleaseCapture{});
}

// ============================================================
// MD ペイン スクロールバードラッグ
// ============================================================

void ReduceMdScrollbarDragStarted(AppState& state, SideEffectList& effects, const MdScrollbarDragStartedAction& a)
{
    state.view.panes.StartDrag(PaneController::DragTarget::MdScrollbar);
    state.view.viewport.SetScrollbarTracking(true);
    effects.emplace_back(effect::SetCapture{});

    const auto& md_rect = state.cached_pane_layout.md_rect;
    const auto info = ComputeScrollInfo(md_rect, 0.0f, a.total_height);
    const float thumb_y = ComputeThumbY(info, state.view.viewport.GetScrollY());
    if (a.dip_y >= thumb_y && a.dip_y <= thumb_y + info.thumb_height) {
        // つまみ上を掴んだ場合はつかみ位置のオフセットのみ記録してスクロール位置は据え置き。
        state.view.panes.SetDragScrollOffset(a.dip_y - thumb_y);
        return;
    }
    // つまみ外クリック: つまみ中心にジャンプしてスクロール位置を更新する。
    state.view.panes.SetDragScrollOffset(info.thumb_height * 0.5f);
    const float new_thumb_y = a.dip_y - state.view.panes.GetDragScrollOffset();
    const float old_scroll = state.view.viewport.GetScrollY();
    state.view.viewport.ScrollTo(ScrollFromThumbY(info, new_thumb_y));
    EmitScrollEffects(state, effects, old_scroll);
}

void ReduceMdScrollbarDragMoved(AppState& state, SideEffectList& effects, const MdScrollbarDragMovedAction& a)
{
    if (state.view.panes.GetDragTarget() != PaneController::DragTarget::MdScrollbar) {
        return;
    }
    const auto& md_rect = state.cached_pane_layout.md_rect;
    const auto info = ComputeScrollInfo(md_rect, 0.0f, a.total_height);
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
    effects.emplace_back(effect::ReleaseCapture{});
    effects.emplace_back(effect::PerformResizeEnd{});
    effects.emplace_back(effect::BitmapManage{});
}

// ============================================================
// サイドペイン (File/Toc) スクロールバードラッグ
// ============================================================

void ReducePaneScrollbarDragStarted(AppState& state, SideEffectList& effects, const PaneScrollbarDragStartedAction& a)
{
    auto ctx = GetSidePaneContext(state, a.pane);
    if (ctx.total_content <= ctx.info.content_height) {
        return;
    }
    state.view.panes.StartDrag(ctx.drag_target);
    effects.emplace_back(effect::SetCapture{});

    const float thumb_y = ComputeThumbY(ctx.info, ctx.scroll.scroll_y);
    if (a.dip_y >= thumb_y && a.dip_y <= thumb_y + ctx.info.thumb_height) {
        state.view.panes.SetDragScrollOffset(a.dip_y - thumb_y);
        return;
    }
    state.view.panes.SetDragScrollOffset(ctx.info.thumb_height * 0.5f);
    const float new_thumb_y = a.dip_y - state.view.panes.GetDragScrollOffset();
    ctx.scroll.scroll_y = ScrollFromThumbY(ctx.info, new_thumb_y);
    ctx.scroll.max_scroll = ctx.info.max_scroll;
    effects.emplace_back(effect::InvalidatePaneCache{ ctx.pane_zone });
    effects.emplace_back(effect::InvalidateWindow{});
}

void ReducePaneScrollbarDragMoved(AppState& state, SideEffectList& effects, const PaneScrollbarDragMovedAction& a)
{
    auto ctx = GetSidePaneContext(state, a.pane);
    if (state.view.panes.GetDragTarget() != ctx.drag_target) {
        return;
    }
    const float new_thumb_y = a.dip_y - state.view.panes.GetDragScrollOffset();
    ctx.scroll.scroll_y = ScrollFromThumbY(ctx.info, new_thumb_y);
    ctx.scroll.max_scroll = ctx.info.max_scroll;
    effects.emplace_back(effect::InvalidatePaneCache{ ctx.pane_zone });
    effects.emplace_back(effect::InvalidateWindow{});
}

void ReducePaneScrollbarDragEnded(AppState& state, SideEffectList& effects)
{
    const auto drag = state.view.panes.GetDragTarget();
    if (drag != PaneController::DragTarget::FileScrollbar
        && drag != PaneController::DragTarget::TocScrollbar) {
        return;
    }
    state.view.panes.EndDrag();
    effects.emplace_back(effect::ReleaseCapture{});
}

// ============================================================
// 本文ペイン テキスト選択ドラッグ
// ============================================================

void ReduceTextSelectionStarted(AppState& state, SideEffectList& effects, const TextSelectionStartedAction& a)
{
    state.view.viewport.SetClickStart(a.click_x, a.click_y);
    if (a.node_index < 0) {
        return;
    }
    state.view.viewport.SetAnchor(a.node_index, a.text_pos);
    state.view.viewport.SetDragging(true);
    state.view.viewport.GetSelectionMut().Clear();
    effects.emplace_back(effect::SetCapture{});
    effects.emplace_back(effect::InvalidateWindow{});
}

void ReduceTextSelectionMoved(AppState& state, SideEffectList& effects, const TextSelectionMovedAction& a)
{
    if (!state.view.viewport.IsDragging() || a.node_index < 0) {
        return;
    }
    state.view.viewport.SetSelection(TextSelection::MakeOrdered(
        state.view.viewport.GetAnchorNode(),
        state.view.viewport.GetAnchorPos(),
        a.node_index,
        a.text_pos
    ));
    effects.emplace_back(effect::InvalidateWindow{});
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
            a.end_text_pos
        ));
    }
    state.view.viewport.SetDragging(false);
    effects.emplace_back(effect::ReleaseCapture{});
    effects.emplace_back(effect::InvalidateWindow{});
}

// ============================================================
// 右クリックジェスチャー
// ============================================================

void ReduceRightClickGestureStarted(AppState& state, SideEffectList& effects, const RightClickGestureStartedAction& a)
{
    state.interaction.gesture.OnRButtonDown(a.dip_x, a.dip_y);
    effects.emplace_back(effect::SetCapture{});
}

void ReduceRightClickGestureMoved(AppState& state, SideEffectList& effects, const RightClickGestureMovedAction& a)
{
    state.interaction.gesture.OnMouseMove(a.dip_x, a.dip_y);
    if (state.interaction.gesture.IsGestureActive()) {
        effects.emplace_back(effect::InvalidateWindow{});
    }
}

void ReduceRightClickGestureCompleted(AppState& state, SideEffectList& effects, const RightClickGestureCompletedAction& a)
{
    if (state.interaction.gesture.GetPhase() == GesturePhase::Idle) {
        return;
    }
    const auto result = state.interaction.gesture.OnRButtonUp();
    effects.emplace_back(effect::ReleaseCapture{});
    switch (result) {
    case GestureResult::ShowContextMenu:
        state.interaction.gesture.Reset();
        effects.emplace_back(effect::ShowContextMenu{ a.screen_x, a.screen_y });
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
    effects.emplace_back(effect::InvalidateWindow{});
}

// ============================================================
// ファイルペイン アイテムクリック
// ============================================================

void ReduceFilePaneDirectoryClicked(AppState& state, SideEffectList& effects, const FilePaneDirectoryClickedAction& a)
{
    state.file_explorer.SetDirectory(a.full_path);
    if (!state.document.doc.GetFilePath().empty()) {
        state.file_explorer.SetCurrentFile(state.document.doc.GetFilePath());
    }
    state.view.panes.FileScroll() = {};
    effects.emplace_back(effect::InvalidatePaneCache{ PaneZone::FilePane });
    effects.emplace_back(effect::InvalidateWindow{});
}

void ReduceFilePaneFileClicked(AppState& state, SideEffectList& effects, const FilePaneFileClickedAction& a)
{
    PushCurrentNavEntry(state);
    effects.emplace_back(effect::LoadFile{ a.full_path });
}

// ============================================================
// TOC アイテムクリック
// ============================================================

void ReduceTocItemClicked(AppState& state, SideEffectList& effects, const TocItemClickedAction& a)
{
    PushCurrentNavEntry(state);

    const int idx = state.document.doc.FindAnchorIndex(a.anchor_id);
    if (idx < 0) {
        return;
    }
    const auto target = MakeHeadingTopTarget(idx,
        state.window.cached_theme.heading_spacing_above,
        state.cached_pane_layout.md_rect.y);
    state.view.viewport.SetScrollTarget(target.node, target.offset);
    state.view.viewport.ApplyScrollTarget(state.document.layout_cache);
    effects.emplace_back(effect::InvalidateWindow{});
    effects.emplace_back(effect::BitmapManage{});
}

// ============================================================
// ファイル・ナビゲーション系アクション
// ============================================================

void ReduceDropFiles(AppState& state, SideEffectList& effects, const DropFilesAction& a)
{
    if (!state.document.doc.GetFilePath().empty()) {
        PushCurrentNavEntry(state);
    }
    effects.emplace_back(effect::LoadFile{ a.path });
}

void ReduceShowHelp(AppState& state, SideEffectList& effects)
{
    if (!state.document.doc.GetFilePath().empty() && !IsHelpPath(state.document.doc.GetFilePath())) {
        PushCurrentNavEntry(state);
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

        // ---- マウス関連 ----
        [&](const MouseLeaveAction&) {
            // 再侵入時に同一座標の最初の WM_MOUSEMOVE が ShouldSkipSameDispatch で
            // 弾かれるとカーソル/ツールチップの復帰が遅れるため、hover 状態もリセットする
            state.interaction.hover_throttle.Reset();
            ClearTooltip(state, effects);
        },
        [&](const CaptureChangedAction&) { ReduceCaptureChanged(state, effects); },
        [&](const MdPaneNavHoverAction& a) { ReduceMdPaneNavHover(state, effects, a); },
        [&](const MdPaneButtonHoverChangedAction& a) { ReduceMdPaneButtonHoverChanged(state, effects, a); },
        [&](const SplitterDragStartedAction& a) { ReduceSplitterDragStarted(state, effects, a); },
        [&](const SplitterDragMovedAction& a) { ReduceSplitterDragMoved(state, effects, a); },
        [&](const SplitterDragEndedAction&) { ReduceSplitterDragEnded(state, effects); },
        [&](const SearchInputDragStartedAction& a) { ReduceSearchInputDragStarted(state, effects, a); },
        [&](const SearchInputDragMovedAction& a) { ReduceSearchInputDragMoved(state, effects, a); },
        [&](const SearchInputDragEndedAction&) { ReduceSearchInputDragEnded(state, effects); },
        [&](const MdScrollbarDragStartedAction& a) { ReduceMdScrollbarDragStarted(state, effects, a); },
        [&](const MdScrollbarDragMovedAction& a) { ReduceMdScrollbarDragMoved(state, effects, a); },
        [&](const MdScrollbarDragEndedAction&) { ReduceMdScrollbarDragEnded(state, effects); },
        [&](const PaneScrollbarDragStartedAction& a) { ReducePaneScrollbarDragStarted(state, effects, a); },
        [&](const PaneScrollbarDragMovedAction& a) { ReducePaneScrollbarDragMoved(state, effects, a); },
        [&](const PaneScrollbarDragEndedAction&) { ReducePaneScrollbarDragEnded(state, effects); },
        [&](const TextSelectionStartedAction& a) { ReduceTextSelectionStarted(state, effects, a); },
        [&](const TextSelectionMovedAction& a) { ReduceTextSelectionMoved(state, effects, a); },
        [&](const TextSelectionEndedAction& a) { ReduceTextSelectionEnded(state, effects, a); },
        [&](const RightClickGestureStartedAction& a) { ReduceRightClickGestureStarted(state, effects, a); },
        [&](const RightClickGestureMovedAction& a) { ReduceRightClickGestureMoved(state, effects, a); },
        [&](const RightClickGestureCompletedAction& a) { ReduceRightClickGestureCompleted(state, effects, a); },
        [&](const FilePaneDirectoryClickedAction& a) { ReduceFilePaneDirectoryClicked(state, effects, a); },
        [&](const FilePaneFileClickedAction& a) { ReduceFilePaneFileClicked(state, effects, a); },
        [&](const TocItemClickedAction& a) { ReduceTocItemClicked(state, effects, a); },
        [&](const UpdateTooltipAction& a) {
            if (a.target == state.interaction.tooltip.GetCurrent()) {
                return;
            }
            effects.emplace_back(effect::ShowTooltip{ a.target, a.px, a.py });
        },
        [&](const ClearTooltipAction&) { ClearTooltip(state, effects); },

        // ---- ナビゲーション ----
        [&](const NavigateBackAction&) { ReduceNavigateBack(state, effects); },
        [&](const NavigateForwardAction&) { ReduceNavigateForward(state, effects); },

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
