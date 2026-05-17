#include "reducer.h"
#include "app_constants.h"
#include "block_h_scroll.h"
#include "document_utils.h"
#include "file_io.h"
#include "layout_computer.h"
#include "overloaded.h"
#include "scroll_drag.h"
#include "ui_constants.h"
#include "utility.h"
#include <algorithm>
#include <cmath>

ScrollTarget SnapshotVisibleTarget(const AppState& state) noexcept
{
    const auto& cache = state.document.layout_cache;
    const int node = state.view.viewport.FindFirstVisibleNode(cache, cache.size());
    const float y_before = (node >= 0) ? cache[node].text_top : 0.0f;
    return { node, state.view.viewport.GetScrollY() - y_before };
}

NavEntry CurrentNavEntry(const AppState& state)
{
    const ScrollTarget t = SnapshotVisibleTarget(state);
    return NavEntry{ state.document.doc.GetFilePath(), t.node, t.offset };
}

void PushCurrentNavEntry(AppState& state)
{
    state.view.nav_history.Push(CurrentNavEntry(state));
}

namespace {

void ClearTooltip(AppState& state, SideEffectList& effects)
{
    state.interaction.tooltip.Hide();
    state.interaction.tooltip.ResetTarget();
    PushEffect(effects, effect::ClearTooltip{});
}

// スクロール位置が変わった時に共通で発火する副作用列。
// InvalidateMdPane → BitmapManage の順序は test_reducer の HasEffectInOrder で契約として担保。
// MD ペイン限定無効化により、タイトルバー/サイドペインビットマップキャッシュの再描画を避ける。
void EmitScrollChangedSideEffects(AppState& state, SideEffectList& effects)
{
    state.interaction.hover_throttle.Reset();
    ClearTooltip(state, effects);
    PushEffect(effects, effect::InvalidateMdPane{});
    PushEffect(effects, effect::BitmapManage{});
    PushEffect(effects, effect::SyncTocActive{});
}

void EmitScrollEffects(AppState& state, SideEffectList& effects, float old_scroll)
{
    if (state.view.viewport.GetScrollY() != old_scroll) {
        EmitScrollChangedSideEffects(state, effects);
    }
}

void ApplyScrollTargetAndEmit(AppState& state, SideEffectList& effects, int node, float offset)
{
    state.view.viewport.SetScrollTarget(node, offset);
    state.view.viewport.ApplyScrollTarget(state.document.layout_cache);
    // 末尾セクションへのジャンプで scroll_y > max_scroll になると、後続の DirectScrollBy で
    // 位置が一気に飛ぶ。事前クランプ + target 無効化で範囲外への復帰を抑える。
    state.view.viewport.ClampAndDetach();
    EmitScrollChangedSideEffects(state, effects);
}

struct SidePaneContext {
    const PaneRect& rect;
    float total_content;
    PaneScrollInfo info;
    ScrollState& scroll;
    PaneController::DragTarget drag_target;
    PaneZone pane_zone;
};

constexpr PaneController::DragTarget SidePaneDragTarget(PaneTarget pane) noexcept
{
    using enum PaneController::DragTarget;
    return (pane == PaneTarget::File) ? FileScrollbar : TocScrollbar;
}

SidePaneContext GetSidePaneContext(AppState& state, PaneTarget pane)
{
    const float item_h = state.theme->pane_item_height;
    const float header_h = state.theme->pane_header_height;
    const auto& layout = state.pane_layout_cache.Get();
    const float item_count = static_cast<float>(
        pane == PaneTarget::File
            ? state.file_explorer.GetEntries().size()
            : state.document.doc.GetToc().GetEntries().size());
    const float total = item_count * item_h;
    const PaneRect& rect = layout.Get(pane);
    return {
        rect,
        total,
        ComputeScrollInfo(rect, header_h, total),
        state.view.panes.SidePaneScroll(pane),
        SidePaneDragTarget(pane),
        ToPaneZone(pane),
    };
}

void ApplyNavResult(AppState& state, SideEffectList& effects, NavEntry&& entry)
{
    if (!entry.file_path.empty() && !path_util::iequal(entry.file_path, state.document.doc.GetFilePath())) {
        state.view.scroll_restore.SetNodeRestore(entry.node, static_cast<int>(std::lround(entry.offset)));
        PushEffect(effects, effect::LoadFile{ std::move(entry.file_path) });
    }
    else {
        ApplyScrollTargetAndEmit(state, effects, entry.node, entry.offset);
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

void ReduceKeyScroll(AppState& state, SideEffectList& effects, const KeyScrollAction& a)
{
    const float old_scroll = state.view.viewport.GetScrollY();
    const float page_size = state.pane_layout_cache.Get().md_rect.height;
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
        // 末尾は max_scroll に依存するためピクセル指定。scroll target は無効化される。
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
    const auto target = ToPaneTarget(a.pane);
    if (!target) {
        return;
    }
    const auto ctx = GetSidePaneContext(state, *target);
    if (state.view.panes.ScrollSidePaneBy(*target, a.delta, ctx.info.max_scroll)) {
        PushEffect(effects, effect::InvalidatePaneCache{ ctx.pane_zone });
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ReduceTogglePane(AppState& state, SideEffectList& effects, const TogglePaneAction& a)
{
    state.view.panes.ToggleSidePane(a.target);
    state.pane_layout_cache.Invalidate();
    PushEffect(effects, effect::RefreshPaneLayout{});
    if (a.target == PaneTarget::Toc) {
        PushEffect(effects, effect::SyncTocActive{});
    }
}

void ReduceSelectAll(AppState& state, SideEffectList& effects)
{
    state.view.viewport.SelectAll(state.document.doc.GetNodes());
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceClearSelection(AppState& state, SideEffectList& effects)
{
    if (state.search.search_state.IsVisible()) {
        state.search.search_state.Hide();
    }
    else {
        state.view.viewport.ClearSelection();
    }
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceCopyClipboard(const AppState& state, SideEffectList& effects)
{
    if (state.view.viewport.GetSelection().active) {
        PushEffect(effects, effect::ClipboardWrite{ ExtractSelectedText(state.document.doc.GetNodes(), state.view.viewport.GetSelection()) });
    }
}

void ReduceCopyFormattedClipboard(const AppState& state, SideEffectList& effects)
{
    const auto& sel = state.view.viewport.GetSelection();
    if (!sel.active) {
        return;
    }
    const auto& nodes = state.document.doc.GetNodes();
    PushEffect(effects, effect::ClipboardWriteHtml{ ExtractSelectedTextAsHtml(nodes, sel, state.theme->IsDark()), ExtractSelectedText(nodes, sel) });
}

void ReduceZoom(AppState& state, SideEffectList& effects, const ZoomAction& a)
{
    {
        bool changed = false;
        switch (a.direction) {
        case ZoomDirection::In:
            changed = state.view.viewport.ZoomIn();
            break;
        case ZoomDirection::Out:
            changed = state.view.viewport.ZoomOut();
            break;
        case ZoomDirection::Reset:
            changed = state.view.viewport.ZoomReset();
            break;
        }
        if (!changed) {
            return;
        }
    }
    const auto anchor = SnapshotVisibleTarget(state);
    state.pane_layout_cache.Invalidate();
    const float new_zoom = state.view.viewport.GetCurrentZoom();
    const float zoom_ratio = new_zoom / state.theme->zoom;
    state.view.panes.ApplyZoom(zoom_ratio);
    state.document.layout_cache.InvalidateAllLayouts();
    if (anchor.IsValid()) {
        // offset もズーム比でスケールしないと、ノード内の同じ位置が可視先頭に残らない。
        state.view.viewport.SetScrollTarget(anchor.node, anchor.offset * zoom_ratio);
    }
    PushEffect(
        effects,
        effect::ApplyThemeChange{
            .type = effect::ApplyThemeChange::Type::Zoom,
            .zoom_index = static_cast<uint8_t>(state.view.viewport.GetZoomIndex()),
        });
}

void ReduceToggleDarkMode(AppState& state, SideEffectList& effects)
{
    const auto anchor = SnapshotVisibleTarget(state);
    state.pane_layout_cache.Invalidate();
    // 色のみの変更なのでテキストレイアウトは維持し、effects と Mermaid bitmap のみ破棄する。
    state.document.layout_cache.InvalidateEffectsAndDiagramBitmaps(state.document.doc.GetNodes());
    if (anchor.IsValid()) {
        // Mermaid 再レンダリングで微小な高さ変化が起きるので target で追従する。
        state.view.viewport.SetScrollTarget(anchor.node, anchor.offset);
    }
    PushEffect(
        effects,
        effect::ApplyThemeChange{
            .type = effect::ApplyThemeChange::Type::DarkMode,
            .zoom_index = static_cast<uint8_t>(state.view.viewport.GetZoomIndex()),
        });
}

void ReduceActivate(AppState& state, SideEffectList& effects, const ActivateAction& a)
{
    if (state.window.window_active != a.active) {
        state.window.window_active = a.active;
        PushEffect(effects, effect::InvalidateTitleBar{});
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
    state.pane_layout_cache.Invalidate();
    PushEffect(effects, effect::RendererResize{ a.width, a.height });
    const float window_w_dip = a.width / state.window.cached_dpi_scale;
    state.window.titlebar.UpdateLayout(window_w_dip);
    if (state.window.is_sizing) {
        PushEffect(effects, effect::PerformSizingUpdate{});
    }
    else {
        PushEffect(effects, effect::PerformResizeEnd{});
    }
}

void ReduceDpiChanged(AppState& state, SideEffectList& effects, const DpiChangedAction& a)
{
    state.window.cached_dpi_scale = static_cast<float>(a.dpi) / 96.0f;
    if (state.window.cached_dpi_scale <= 0.0f) {
        state.window.cached_dpi_scale = 1.0f;
    }
    state.pane_layout_cache.Invalidate();
    // DPI 変更では IDWriteTextLayout (DIP 単位) は不変。effects_generation のみ進める。
    state.document.layout_cache.NotifyDpiChanged();
    PushEffect(effects, effect::RendererSetDpi{ static_cast<float>(a.dpi) });
    PushEffect(effects, effect::ClearFileCache{});
    PushEffect(
        effects,
        effect::SetWindowPosition{
            static_cast<int>(a.suggested.left),
            static_cast<int>(a.suggested.top),
            static_cast<int>(a.suggested.right - a.suggested.left),
            static_cast<int>(a.suggested.bottom - a.suggested.top),
        });
}

namespace {

BlockHScrollGeometry ResolveBlockHScrollGeometry(const AppState& state, int node_index) noexcept
{
    if (node_index < 0 || !state.theme || !state.pane_layout_cache.IsValid()) {
        return {};
    }
    const auto& nodes = state.document.doc.GetNodes();
    const auto& cache = state.document.layout_cache;
    if (node_index >= static_cast<int>(nodes.size()) || node_index >= static_cast<int>(cache.size())) {
        return {};
    }
    return GetBlockHScrollGeometry(
        nodes[node_index],
        cache[node_index],
        *state.theme,
        state.pane_layout_cache.Get().md_rect.width);
}

// scroll_x を [0, max] に詰めて map に反映する。0 になった場合はエントリを削除する。
// HitTestService 側のキャッシュキーには block_scroll_x が含まれないため、
// 値が変わったタイミングで effects_generation を進めて last_md_hit_ の再計算を強制する。
void ApplyBlockHScrollDelta(AppState& state, int node_index, float new_value, float scroll_max)
{
    const float clamped = std::clamp(new_value, 0.0f, scroll_max);
    const float prev = state.view.GetBlockScrollX(node_index);
    if (std::abs(prev - clamped) < 1e-3f) {
        return;
    }
    if (clamped <= 0.0f) {
        state.view.block_scroll_x.erase(node_index);
    }
    else {
        state.view.block_scroll_x[node_index] = clamped;
    }
    state.document.layout_cache.IncrementEffectsGeneration();
}

} // namespace

void ReduceHWheel(AppState& state, SideEffectList& effects, const HWheelAction& a)
{
    // ホバー or ドラッグ中のブロックが横スクロール可能ならそこに適用し、スワイプオーバーレイには流さない。
    const int target = (state.view.h_drag_node >= 0) ? state.view.h_drag_node : state.view.hovered_h_block;
    if (target >= 0) {
        const auto geom = ResolveBlockHScrollGeometry(state, target);
        if (geom.can_scroll()) {
            const float dx = static_cast<float>(a.delta) / WHEEL_DELTA * HSCROLL_DIP_PER_NOTCH;
            const float cur = state.view.GetBlockScrollX(target);
            ApplyBlockHScrollDelta(state, target, cur + dx, geom.scroll_max());
            PushEffect(effects, effect::InvalidateWindow{});
            return;
        }
    }

    const bool had_overlay = state.interaction.swipe_detector.IsOverlayVisible();
    const int old_direction = state.interaction.swipe_detector.GetOverlayDirection();
    state.interaction.swipe_detector.OnHWheel(a.delta, a.tick);
    PushEffect(effects, effect::SetTimer{ app_timer::Id::SWIPE_OVERLAY, static_cast<UINT>(SwipeDetector::COMMIT_TIMEOUT_MS) });
    if (had_overlay != state.interaction.swipe_detector.IsOverlayVisible() || old_direction != state.interaction.swipe_detector.GetOverlayDirection()) {
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
    // clang-format off
    mendo::RunScrollDragStarted(
        effects,
        [&sv = state.view, node_index = a.node_index, dip_x = a.dip_x] {
            sv.h_drag_node = node_index;
            sv.h_drag_start_x = dip_x;
            sv.h_drag_start_scroll = sv.GetBlockScrollX(node_index);
        },
        [&effects] {
            PushEffect(effects, effect::InvalidateWindow{});
        }
    );
    // clang-format on
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
    ApplyBlockHScrollDelta(state, sv.h_drag_node, sv.h_drag_start_scroll + dip_delta * scroll_max / drag_range, scroll_max);
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceBlockHScrollDragEnded(AppState& state, SideEffectList& effects, const BlockHScrollDragEndedAction&)
{
    if (state.view.h_drag_node < 0) {
        return;
    }
    // clang-format off
    mendo::RunScrollDragEnded(
        effects,
        [&state] {
            state.view.h_drag_node = -1;
        },
        [&effects] {
            PushEffect(effects, effect::InvalidateWindow{});
        }
    );
    // clang-format on
}

void ReduceSearchStep(AppState& state, bool forward)
{
    auto& ss = state.search;
    if (ss.search_state.IsVisible()) {
        if (forward) {
            ss.search_bar_ctrl.OnNext();
        }
        else {
            ss.search_bar_ctrl.OnPrev();
        }
    }
    else {
        ss.search_bar_ctrl.OnOpen(state.document.doc.GetNodes());
    }
}

void ReduceCaptureChanged(AppState& state, SideEffectList& effects)
{
    state.search.search_bar_ctrl.OnCaptureChanged();
    if (state.interaction.gesture.GetPhase() != GesturePhase::Idle) {
        state.interaction.gesture.Reset();
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
    if (!state.search.search_bar_ctrl.IsDragging()) {
        return;
    }
    const auto& ctrl = state.search.search_bar_ctrl;
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

namespace {

// つまみ上クリック → 位置維持 (オフセットのみ記録)、つまみ外 → 中心へジャンプ。
struct ScrollbarDragGrip {
    float drag_offset;
    bool inside_thumb;
};

constexpr ScrollbarDragGrip ComputeScrollbarDragGrip(
    float thumb_y, float thumb_height, float click_y) noexcept
{
    if (click_y >= thumb_y && click_y <= thumb_y + thumb_height) {
        return { click_y - thumb_y, true };
    }
    return { thumb_height * 0.5f, false };
}

} // namespace

void ReduceMdScrollbarDragStarted(AppState& state, SideEffectList& effects, const MdScrollbarDragStartedAction& a)
{
    const auto& md_rect = state.pane_layout_cache.Get().md_rect;
    const auto info = ComputeScrollInfo(md_rect, 0.0f, a.total_height);
    const float thumb_y = ComputeThumbY(info, state.view.viewport.GetScrollY());
    const auto grip = ComputeScrollbarDragGrip(thumb_y, info.thumb_height, a.dip_y);
    const auto drag_offset = grip.drag_offset;

    auto begin = [&sv = state.view, drag_offset] {
        sv.panes.StartDrag(PaneController::DragTarget::MdScrollbar);
        sv.viewport.SetScrollbarTracking(true);
        sv.panes.SetDragScrollOffset(drag_offset);
    };
    // thumb 内クリックなら 1st jump は不要 (thumb-grip オフセット記録だけ)。
    if (grip.inside_thumb) {
        mendo::RunScrollDragStarted(effects, begin);
    }
    else {
        auto jump = [&state, &effects, info, new_thumb_y = a.dip_y - drag_offset] {
            const float old_scroll = state.view.viewport.GetScrollY();
            state.view.viewport.ScrollTo(ScrollFromThumbY(info, new_thumb_y));
            EmitScrollEffects(state, effects, old_scroll);
        };
        mendo::RunScrollDragStarted(effects, begin, jump);
    }
}

void ReduceMdScrollbarDragMoved(AppState& state, SideEffectList& effects, const MdScrollbarDragMovedAction& a)
{
    if (state.view.panes.GetDragTarget() != PaneController::DragTarget::MdScrollbar) {
        return;
    }
    const auto& md_rect = state.pane_layout_cache.Get().md_rect;
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
    // clang-format off
    mendo::RunScrollDragEnded(
        effects,
        [&state] {
            state.view.viewport.SetScrollbarTracking(false);
            state.view.panes.EndDrag();
        },
        [&effects] {
            PushEffect(effects, effect::PerformResizeEnd{});
            PushEffect(effects, effect::BitmapManage{});
        }
    );
    // clang-format on
}

void ReducePaneScrollbarDragStarted(AppState& state, SideEffectList& effects, const PaneScrollbarDragStartedAction& a)
{
    auto ctx = GetSidePaneContext(state, a.pane);
    if (ctx.total_content <= ctx.info.content_height) {
        return;
    }
    const float thumb_y = ComputeThumbY(ctx.info, ctx.scroll.scroll_y);
    const auto grip = ComputeScrollbarDragGrip(thumb_y, ctx.info.thumb_height, a.dip_y);
    const auto drag_offset = grip.drag_offset;

    auto begin = [&state, drag_target = ctx.drag_target, drag_offset] {
        state.view.panes.StartDrag(drag_target);
        state.view.panes.SetDragScrollOffset(drag_offset);
    };
    if (grip.inside_thumb) {
        mendo::RunScrollDragStarted(effects, begin);
    }
    else {
        auto jump = [&effects, &ctx, new_thumb_y = a.dip_y - drag_offset] {
            ctx.scroll.scroll_y = ScrollFromThumbY(ctx.info, new_thumb_y);
            ctx.scroll.max_scroll = ctx.info.max_scroll;
            PushEffect(effects, effect::InvalidatePaneCache{ ctx.pane_zone });
            PushEffect(effects, effect::InvalidateWindow{});
        };
        mendo::RunScrollDragStarted(effects, begin, jump);
    }
}

void ReducePaneScrollbarDragMoved(AppState& state, SideEffectList& effects, const PaneScrollbarDragMovedAction& a)
{
    // drag target は pane だけから決まるので、ctx 構築前に短絡し
    // 非ドラッグ時の per-event GetEntries().size() + ComputeScrollInfo を回避。
    if (state.view.panes.GetDragTarget() != SidePaneDragTarget(a.pane)) {
        return;
    }
    auto ctx = GetSidePaneContext(state, a.pane);
    const float new_thumb_y = a.dip_y - state.view.panes.GetDragScrollOffset();
    ctx.scroll.scroll_y = ScrollFromThumbY(ctx.info, new_thumb_y);
    ctx.scroll.max_scroll = ctx.info.max_scroll;
    PushEffect(effects, effect::InvalidatePaneCache{ ctx.pane_zone });
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReducePaneScrollbarDragEnded(AppState& state, SideEffectList& effects)
{
    using enum PaneController::DragTarget;
    const auto drag = state.view.panes.GetDragTarget();
    if (drag != FileScrollbar && drag != TocScrollbar) {
        return;
    }
    mendo::RunScrollDragEnded(effects, [&state] {
        state.view.panes.EndDrag();
    });
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
    state.view.viewport.SetSelection(TextSelection::MakeOrdered(
        state.view.viewport.GetAnchorNode(),
        state.view.viewport.GetAnchorPos(),
        a.node_index,
        a.text_pos));
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

void ReduceFilePaneDirectoryClicked(AppState& state, SideEffectList& effects, const FilePaneDirectoryClickedAction& a)
{
    state.file_explorer.SetDirectory(a.full_path);
    if (!state.document.doc.GetFilePath().empty()) {
        state.file_explorer.SetCurrentFile(state.document.doc.GetFilePath());
    }
    state.view.panes.SidePaneScroll(PaneTarget::File) = {};
    PushEffect(effects, effect::InvalidatePaneCache{ PaneZone::FilePane });
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceFilePaneFileClicked(AppState& state, SideEffectList& effects, const FilePaneFileClickedAction& a)
{
    PushCurrentNavEntry(state);
    PushEffect(effects, effect::LoadFile{ a.full_path });
}

namespace {
void ScrollToResolvedAnchor(AppState& state, SideEffectList& effects, int idx)
{
    if (idx < 0) {
        return;
    }
    const auto target = MakeHeadingTopTarget(
        idx,
        state.theme->heading_spacing_above,
        state.pane_layout_cache.Get().md_rect.y);
    ApplyScrollTargetAndEmit(state, effects, target.node, target.offset);
}
} // namespace

void ScrollToAnchor(AppState& state, SideEffectList& effects, std::string_view anchor_id)
{
    ScrollToResolvedAnchor(state, effects, state.document.doc.FindAnchorIndex(anchor_id));
}

// anchor_id() 由来など、既に正規化済み入力向け（ToLowerAscii の確保を回避する）。
void ScrollToNormalizedAnchor(AppState& state, SideEffectList& effects, std::string_view anchor_id)
{
    ScrollToResolvedAnchor(state, effects, state.document.doc.FindNormalizedAnchorIndex(anchor_id));
}

void ReduceTocItemClicked(AppState& state, SideEffectList& effects, const TocItemClickedAction& a)
{
    PushCurrentNavEntry(state);
    // TOC は anchor_id() を直接渡すため、改めての正規化は不要。
    ScrollToNormalizedAnchor(state, effects, a.anchor_id);
}

void ReduceNavigateAnchor(AppState& state, SideEffectList& effects, const NavigateAnchorAction& a)
{
    // nav 履歴の push は呼び出し側 (App::HandleLinkClick) で行われる。
    ScrollToAnchor(state, effects, a.anchor_id);
}

void ReduceRestoreScrollAfterLoad(AppState& state, SideEffectList& /*effects*/, const RestoreScrollAfterLoadAction& a)
{
    // ノードインデックスはセッション内の識別子で、再パース後は別ノードを指しうる。
    state.view.block_scroll_x.clear();
    state.view.hovered_h_block = -1;
    state.view.h_drag_node = -1;

    state.view.viewport.ClearScrollTarget();
    if (a.has_reload_diff) {
        state.view.viewport.SetScrollY(a.reload_diff_scroll_y);
        state.reload_diff_pos = std::string_view::npos;
    }
    else if (state.view.scroll_restore.HasNodeRestore()) {
        state.view.viewport.SetScrollTarget(
            state.view.scroll_restore.pending_restore_node,
            static_cast<float>(state.view.scroll_restore.pending_restore_offset));
        state.view.viewport.ApplyScrollTarget(state.document.layout_cache);
        state.view.scroll_restore.ClearNodeRestore();
    }
    else {
        state.view.viewport.SetScrollY(0.0f);
    }
}

void ReduceDropFiles(AppState& state, SideEffectList& effects, const DropFilesAction& a)
{
    if (!state.document.doc.GetFilePath().empty()) {
        PushCurrentNavEntry(state);
    }
    PushEffect(effects, effect::LoadFile{ a.path });
}

void ReduceShowHelp(AppState& state, SideEffectList& effects)
{
    if (!state.document.doc.GetFilePath().empty() && !IsHelpPath(state.document.doc.GetFilePath())) {
        PushCurrentNavEntry(state);
    }
    PushEffect(effects, effect::LoadFile{ std::pmr::wstring(HELP_PATH) });
}

void ReduceTimer(AppState& state, SideEffectList& effects, const TimerAction& a)
{
    switch (a.timer_id) {
    case app_timer::Id::TOAST:
        if (!state.interaction.toast.Tick()) {
            PushEffect(effects, effect::KillTimer{ app_timer::Id::TOAST });
        }
        PushEffect(effects, effect::InvalidateWindow{});
        return;
    case app_timer::Id::SEARCH_CARET:
        state.search.search_bar_ctrl.OnCaretBlinkTimer();
        return;
    case app_timer::Id::TOOLTIP:
        PushEffect(effects, effect::KillTimer{ app_timer::Id::TOOLTIP });
        state.interaction.tooltip.Show();
        return;
    case app_timer::Id::SEARCH_DEBOUNCE:
        state.search.search_bar_ctrl.OnDebounceTimer(state.document.doc.GetNodes());
        return;
    case app_timer::Id::SWIPE_OVERLAY: {
        const auto result = state.interaction.swipe_detector.Commit();
        PushEffect(effects, effect::KillTimer{ app_timer::Id::SWIPE_OVERLAY });
        switch (result) {
        case SwipeResult::None:
            return;
        case SwipeResult::Back:
            ReduceNavigateBack(state, effects);
            PushEffect(effects, effect::InvalidateWindow{});
            return;
        case SwipeResult::Forward:
            ReduceNavigateForward(state, effects);
            PushEffect(effects, effect::InvalidateWindow{});
            return;
        }
        std::unreachable();
    }
    case app_timer::Id::DEFERRED_LAYOUT:
        PushEffect(effects, effect::ProcessDeferredLayout{});
        return;
    case app_timer::Id::LOADING_ANIM:
        PushEffect(effects, effect::TickLoadingAnimation{});
        PushEffect(effects, effect::InvalidateWindow{});
        return;
    case app_timer::Id::MERMAID_BATCH:
        PushEffect(effects, effect::ProcessMermaidBatchTimer{});
        return;
    case app_timer::Id::BITMAP_MANAGE:
        PushEffect(effects, effect::ProcessBitmapManage{});
        return;
    case app_timer::Id::MERMAID_INIT_RETRY:
        PushEffect(effects, effect::MermaidInitRetry{});
        return;
    case app_timer::Id::FILE_RELOAD_DEBOUNCE:
        PushEffect(effects, effect::KillTimer{ app_timer::Id::FILE_RELOAD_DEBOUNCE });
        PushEffect(effects, effect::ReloadFile{});
        return;
    }
    std::unreachable();
}

} // namespace

SideEffectList Reduce(AppState& state, const AppAction& action)
{
    SideEffectList effects;

    // clang-format off
    std::visit(mendo::overloaded{
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
            PushEffect(effects, effect::PerformResizeEnd{});
        },
        [&](const ResizeAction& a) { ReduceResize(state, effects, a); },
        [&](const DpiChangedAction& a) { ReduceDpiChanged(state, effects, a); },
        [&](const HWheelAction& a) { ReduceHWheel(state, effects, a); },
        [&](const BlockHHoverChangedAction& a) { ReduceBlockHHoverChanged(state, effects, a); },
        [&](const BlockHScrollDragStartedAction& a) { ReduceBlockHScrollDragStarted(state, effects, a); },
        [&](const BlockHScrollDragMovedAction& a) { ReduceBlockHScrollDragMoved(state, effects, a); },
        [&](const BlockHScrollDragEndedAction& a) { ReduceBlockHScrollDragEnded(state, effects, a); },

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
        [&](const NavigateAnchorAction& a) { ReduceNavigateAnchor(state, effects, a); },
        [&](const RestoreScrollAfterLoadAction& a) { ReduceRestoreScrollAfterLoad(state, effects, a); },
        [&](const UpdateTooltipAction& a) {
            if (a.target == state.interaction.tooltip.GetCurrent()) {
                return;
            }
            PushEffect(effects, effect::ShowTooltip{ a.target, a.px, a.py });
        },
        [&](const ClearTooltipAction&) { ClearTooltip(state, effects); },

        // ---- ナビゲーション ----
        [&](const NavigateBackAction&) { ReduceNavigateBack(state, effects); },
        [&](const NavigateForwardAction&) { ReduceNavigateForward(state, effects); },

        // ---- ファイル操作 ----
        [&](const DropFilesAction& a) { ReduceDropFiles(state, effects, a); },
        [&](const ShowHelpAction&) { ReduceShowHelp(state, effects); },
        [&](const OpenFileAction&) { PushEffect(effects, effect::OpenFileDialog{}); },
        [&](const ReloadFileAction&) { PushEffect(effects, effect::ReloadFile{}); },

        // ---- 非同期・タイマー ----
        [&](const TimerAction& a) { ReduceTimer(state, effects, a); },
        [&](const FileWatchAction&) { PushEffect(effects, effect::CheckFileChanges{}); },
        [&](const ImageLoadedAction&) { PushEffect(effects, effect::NotifyImageLoaded{}); },
        [&](const ParseCompleteAction&) { PushEffect(effects, effect::HandleParseComplete{}); },

        // ---- ライフサイクル ----
        [&](const DestroyAction&) { PushEffect(effects, effect::Destroy{}); },

        // ---- 未処理のアクション ----
        [](const auto&) {},
    }, action);
    // clang-format on
    return effects;
}
