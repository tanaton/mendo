#include "reducer.h"
#include "reducer_internal.h"
#include "block_h_scroll.h"
#include "layout_computer.h"
#include "overloaded.h"
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
    // doc 未ロード (パス空) の状態は戻り先にならないため積まない
    if (state.document.doc.GetFilePath().empty()) {
        return;
    }
    state.view.nav_history.Push(CurrentNavEntry(state));
}

// ---- 複数ドメイン横断の共有ヘルパー (宣言は reducer_internal.h) ----

void ClearTooltip(AppState& state, SideEffectList& effects)
{
    state.interaction.tooltip.Hide();
    state.interaction.tooltip.ResetTarget();
    PushEffect(effects, effect::ClearTooltip{});
}

// マウスがペイン上を経由せずに離れた経路 (ウィンドウ外退出等) で呼ばないと
// ホバーハイライトが残留する。
void ClearSidePaneHoverState(AppState& state, SideEffectList& effects)
{
    bool changed = false;
    for (auto t : { PaneTarget::File, PaneTarget::Toc }) {
        bool pane_changed = state.view.panes.SetHoveredSideIndex(t, -1);
        pane_changed |= state.view.panes.SetSideCloseHovered(t, false);
        if (t == PaneTarget::File) {
            pane_changed |= state.view.panes.SetSideRefreshHovered(t, false);
        }
        if (pane_changed) {
            PushEffect(effects, effect::InvalidatePaneCache{ t == PaneTarget::File ? PaneZone::FilePane : PaneZone::TocPane });
            changed = true;
        }
    }
    if (changed) {
        PushEffect(effects, effect::InvalidateWindow{});
    }
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

constexpr PaneController::DragTarget SidePaneDragTargetImpl(PaneTarget pane) noexcept
{
    using enum PaneController::DragTarget;
    return (pane == PaneTarget::File) ? FileScrollbar : TocScrollbar;
}

PaneController::DragTarget SidePaneDragTarget(PaneTarget pane) noexcept
{
    return SidePaneDragTargetImpl(pane);
}

SidePaneContext GetSidePaneContext(AppState& state, PaneTarget pane)
{
    const float item_h = state.theme->pane_item_height;
    const float header_h = state.theme->pane_header_height;
    const auto& layout = state.pane_layout_cache.Get();
    const size_t item_count =
        pane == PaneTarget::File
            ? state.file_explorer.GetEntries().size()
            : state.document.doc.GetToc().GetEntries().size();
    const float total = SidePaneContentHeight(item_count, item_h);
    const PaneRect& rect = layout.Get(pane);
    return {
        rect,
        total,
        ComputeScrollInfo(rect, header_h, total),
        state.view.panes.SidePaneScroll(pane),
        SidePaneDragTargetImpl(pane),
        ToPaneZone(pane),
    };
}

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

// MD ペインの総コンテンツ高 (LayoutService::GetScrollableContentHeight と同じ式)。
// アクションのペイロードで運ぶと、遅延レイアウト進行中にドラッグ開始と移動で
// 別計測の値が混ざりサム位置が跳ねるため、reducer 側で都度導出する。
float MdScrollableContentHeight(const AppState& state) noexcept
{
    return ComputeTotalContentHeight(
        state.document.layout_cache,
        state.document.doc.GetNodes().size(),
        state.theme->margin_top);
}

// HitTestService 側のキャッシュは対象ノードの block_scroll_x をキーに含むため、
// ここで effects_generation を進める必要はない (進めると Renderer の effects
// 再適用まで横スクロール 1 ノッチごとに巻き添えになる)。
bool ApplyBlockHScrollDelta(AppState& state, int node_index, float new_value, float scroll_max)
{
    const float clamped = std::clamp(new_value, 0.0f, scroll_max);
    const float prev = state.view.GetBlockScrollX(node_index);
    if (std::abs(prev - clamped) < 1e-3f) {
        return false;
    }
    if (clamped <= 0.0f) {
        state.view.block_scroll_x.erase(node_index);
    }
    else {
        state.view.block_scroll_x[node_index] = clamped;
    }
    return true;
}

// つまみ上クリック → 位置維持 (オフセットのみ記録)、つまみ外 → 中心へジャンプ。
ScrollbarDragGrip ComputeScrollbarDragGrip(
    float thumb_y, float thumb_height, float click_y) noexcept
{
    if (click_y >= thumb_y && click_y <= thumb_y + thumb_height) {
        return { click_y - thumb_y, true };
    }
    return { thumb_height * 0.5f, false };
}

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
            ClearSidePaneHoverState(state, effects);
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
