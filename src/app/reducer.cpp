#include "reducer.h"
#include "app_constants.h"
#include "block_h_scroll.h"
#include "document_utils.h"
#include "file_io.h"
#include "selection_html.h"
#include "ui_constants.h"
#include "utility.h"
#include <algorithm>
#include <cmath>
#include <concepts>
#include <utility>

ScrollTarget SnapshotVisibleTarget(const AppState& state) noexcept
{
    const auto& cache = state.document.layout_cache;
    const int node = state.view.viewport.FindFirstVisibleNode(cache);
    const float y_before = (node >= 0) ? cache.Top(static_cast<size_t>(node)) : 0.0f;
    return { node, state.view.viewport.GetScrollY() - y_before };
}

NavEntry CurrentNavEntry(const AppState& state)
{
    const ScrollTarget t = SnapshotVisibleTarget(state);
    return NavEntry{ state.document.doc.GetFilePath(), t.node, t.offset };
}

void PushCurrentNavEntry(AppState& state, std::wstring_view load_path)
{
    const auto& current_path = state.document.doc.GetFilePath();
    // doc 未ロード (パス空) の状態は戻り先にならないため積まない
    if (current_path.empty()) {
        return;
    }
    const NavEntry entry = CurrentNavEntry(state);
    // 同じファイルの開き直しはリロードとして終わり、確定 (RestoreScrollAfterLoadAction) が来ない。
    if (!load_path.empty() && !path_util::iequal(load_path, current_path)) {
        state.view.nav_history.PushPending(entry);
    }
    else {
        state.view.nav_history.Push(entry);
    }
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
    return {
        ComputeScrollInfo(layout.Get(pane), header_h, SidePaneContentHeight(item_count, item_h)),
        state.view.panes.SidePaneScroll(pane),
    };
}

BlockHScrollGeometry ResolveBlockHScrollGeometry(const AppState& state, int node_index) noexcept
{
    if (node_index < 0 || !state.theme || !state.pane_layout_cache.IsValid()) {
        return {};
    }
    const auto& nodes = state.document.doc.GetNodes();
    const auto& cache = state.document.layout_cache;
    if (node_index >= static_cast<int>(cache.size())) {
        return {};
    }
    return GetBlockHScrollGeometry(
        nodes[node_index],
        cache[node_index],
        *state.theme,
        state.pane_layout_cache.Get().md_rect.width);
}

// MD ペインの総コンテンツ高。詳細は layout_cache.h の ComputeTotalContentHeight 注記参照。
// アクションのペイロードで運ぶと、遅延レイアウト進行中にドラッグ開始と移動で
// 別計測の値が混ざりサム位置が跳ねるため、reducer 側で都度導出する。
float MdScrollableContentHeight(const AppState& state) noexcept
{
    return ComputeTotalContentHeight(
        state.document.layout_cache,
        state.theme->margin_bottom);
}

namespace {

void ClearTooltip(AppState& state, SideEffectList& effects)
{
    state.interaction.tooltip.Hide();
    state.interaction.tooltip.ResetTarget();
    PushEffect(effects, effect::ClearTooltip{});
}

// 同座標スキップも解除し、次の WM_MOUSEMOVE でホバーを引き直させる。
void InvalidateHover(AppState& state, SideEffectList& effects)
{
    state.interaction.last_hover_pos.Reset();
    ClearTooltip(state, effects);
}

// スクロール位置が変わった時に共通で発火する副作用列。
// toc_auto_scroll は発火元がスクロール先を決める: 目次ペイン操作由来のみ false (issue#259)。
// デフォルト引数にすると新経路が暗黙で true を拾うため、呼び出し側で必ず明示する。
void EmitScrollChangedSideEffects(AppState& state, SideEffectList& effects, bool toc_auto_scroll)
{
    InvalidateHover(state, effects);
    PushEffect(effects, effect::InvalidateWindow{});
    PushEffect(effects, effect::BitmapManage{});
    PushEffect(effects, effect::SyncTocActive{ toc_auto_scroll });
}

void EmitScrollEffects(AppState& state, SideEffectList& effects, float old_scroll)
{
    if (state.view.viewport.GetScrollY() != old_scroll) {
        EmitScrollChangedSideEffects(state, effects, /*toc_auto_scroll=*/true);
    }
}

// f が viewport を直接動かした場合に、前後の scroll_y を比べて通常スクロールと同じ副作用を積む。
template <std::invocable F>
void WithScrollEffects(AppState& state, SideEffectList& effects, F&& f)
{
    const float old_scroll = state.view.viewport.GetScrollY();
    std::forward<F>(f)();
    EmitScrollEffects(state, effects, old_scroll);
}

void ApplyScrollTargetAndEmit(AppState& state, SideEffectList& effects, int node, float offset, bool toc_auto_scroll)
{
    state.view.viewport.SetScrollTarget(node, offset);
    state.view.viewport.ApplyScrollTarget(state.document.layout_cache);
    // 末尾セクションへのジャンプで scroll_y > max_scroll になると、後続の DirectScrollBy で
    // 位置が一気に飛ぶ。事前クランプ + target 無効化で範囲外への復帰を抑える。
    state.view.viewport.ClampAndDetach();
    EmitScrollChangedSideEffects(state, effects, toc_auto_scroll);
}

// new_value は差分ではなく絶対値。
bool SetBlockScrollX(AppState& state, int node_index, float new_value, const BlockHScrollGeometry& geom)
{
    const float clamped = geom.ClampScrollX(new_value);
    const float prev = state.view.GetBlockScrollX(node_index, geom);
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

void ReduceKeyScroll(AppState& state, SideEffectList& effects, const KeyScrollAction& a)
{
    auto& viewport = state.view.viewport;
    const float page_size = state.pane_layout_cache.Get().md_rect.height;
    WithScrollEffects(state, effects, [&] {
        switch (a.type) {
        case ScrollType::LineUp:
            viewport.DirectScrollBy(-SCROLL_LINE_AMOUNT);
            break;
        case ScrollType::LineDown:
            viewport.DirectScrollBy(SCROLL_LINE_AMOUNT);
            break;
        case ScrollType::PageUp:
            viewport.DirectScrollBy(-page_size * SCROLL_PAGE_FACTOR);
            break;
        case ScrollType::PageDown:
            viewport.DirectScrollBy(page_size * SCROLL_PAGE_FACTOR);
            break;
        case ScrollType::Home:
            // ノード 0 の target だと scroll_y = Top(0) になり上余白が隠れる (ロード直後の 0 と不一致)。
            // 短い文書では max_scroll も超えるため、ピクセルで 0 を指定する。
            viewport.ScrollTo(0.0f);
            break;
        case ScrollType::End:
            // 末尾は max_scroll に依存するためピクセル指定。scroll target は無効化される。
            viewport.ScrollTo(viewport.GetMaxScroll());
            break;
        }
    });
}

void ReduceDirectScrollBy(AppState& state, SideEffectList& effects, const DirectScrollByAction& a)
{
    WithScrollEffects(state, effects, [&] { state.view.viewport.DirectScrollBy(a.delta); });
}

void ReduceScrollPane(AppState& state, SideEffectList& effects, const ScrollPaneAction& a)
{
    const auto target = ToPaneTarget(a.pane);
    if (!target) {
        return;
    }
    const auto ctx = GetSidePaneContext(state, *target);
    if (state.view.panes.ScrollSidePaneBy(*target, a.delta, ctx.info.max_scroll)) {
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ReduceHWheel(AppState& state, SideEffectList& effects, const HWheelAction& a)
{
    // ホバー or ドラッグ中のブロックが横スクロール可能ならそこに適用し、スワイプオーバーレイには流さない。
    const int target = (state.view.h_drag_node >= 0) ? state.view.h_drag_node : state.view.hovered_h_block;
    if (target >= 0) {
        // pane_layout_cache 未確立時は ResolveBlockHScrollGeometry が can_scroll()=false を返す。
        const auto geom = ResolveBlockHScrollGeometry(state, target);
        if (!geom.can_scroll()) {
            return;
        }
        const float dx = static_cast<float>(a.delta) / WHEEL_DELTA * HSCROLL_DIP_PER_NOTCH;
        if (SetBlockScrollX(state, target, state.view.GetBlockScrollX(target, geom) + dx, geom)) {
            PushEffect(effects, effect::InvalidateWindow{});
        }
        return;
    }

    auto& swipe = state.interaction.swipe_detector;
    const bool had_overlay = swipe.IsOverlayVisible();
    const auto old_direction = swipe.GetOverlayDirection();
    swipe.OnHWheel(a.delta, a.tick);
    PushEffect(effects, effect::SetTimer{ app_timer::Id::SWIPE_OVERLAY, static_cast<UINT>(SwipeDetector::COMMIT_TIMEOUT_MS) });
    if (had_overlay != swipe.IsOverlayVisible() || old_direction != swipe.GetOverlayDirection()) {
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ReduceBlockHScrollDragStarted(AppState& state, SideEffectList& effects, const BlockHScrollDragStartedAction& a)
{
    const auto geom = ResolveBlockHScrollGeometry(state, a.node_index);
    if (!geom.can_scroll()) {
        return;
    }
    // ドラッグ state の初期化は SetCapture より前に行う
    auto& sv = state.view;
    sv.h_drag_node = a.node_index;
    sv.h_drag_start_x = a.dip_x;
    sv.h_drag_start_scroll = sv.GetBlockScrollX(a.node_index, geom);
    PushEffect(effects, effect::SetCapture{});
    PushEffect(effects, effect::InvalidateWindow{});
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
    if (SetBlockScrollX(state, sv.h_drag_node, sv.h_drag_start_scroll + dip_delta * scroll_max / drag_range, geom)) {
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ReduceBlockHScrollDragEnded(AppState& state, SideEffectList& effects)
{
    if (state.view.h_drag_node < 0) {
        return;
    }
    state.view.h_drag_node = -1;
    PushEffect(effects, effect::ReleaseCapture{});
    PushEffect(effects, effect::InvalidateWindow{});
}

void ApplyNavResult(AppState& state, SideEffectList& effects, NavEntry&& entry)
{
    if (!entry.file_path.empty() && !path_util::iequal(entry.file_path, state.document.doc.GetFilePath())) {
        state.view.scroll_restore.SetNodeRestore(entry.node, static_cast<int>(std::lround(entry.offset)));
        PushEffect(effects, effect::LoadFile{ std::move(entry.file_path) });
    }
    else {
        // 戻る/進むを続けて表示中の文書へ戻ったとき、途中で開き始めたファイルが後から表示されないようにする。
        PushEffect(effects, effect::CancelOtherFileLoad{});
        // 取り消すファイル用の復元位置なので、次に開くファイルへ持ち越さない。
        state.view.scroll_restore.ClearNodeRestore();
        // ロードを伴わないので、ここで確定しないと後の無関係なロード失敗で取り消される。
        state.view.nav_history.CommitPending();
        ApplyScrollTargetAndEmit(state, effects, entry.node, entry.offset, /*toc_auto_scroll=*/true);
    }
}

void NavigateHistory(AppState& state, SideEffectList& effects, bool forward)
{
    auto& history = state.view.nav_history;
    const NavEntry current = CurrentNavEntry(state);
    NavEntry out;
    const bool moved = forward ? history.GoForward(current, out) : history.GoBack(current, out);
    if (moved) {
        ApplyNavResult(state, effects, std::move(out));
    }
}

// 現在位置を履歴に積んでから別ファイルを開く。
void OpenFileWithHistory(AppState& state, SideEffectList& effects, const std::pmr::wstring& path)
{
    PushCurrentNavEntry(state, path);
    PushEffect(effects, effect::LoadFile{ path });
}

void ScrollToHeading(AppState& state, SideEffectList& effects, int node_index, bool toc_auto_scroll)
{
    if (node_index < 0) {
        return;
    }
    const auto target = MakeHeadingTopTarget(node_index, state.theme->heading_spacing_above);
    ApplyScrollTargetAndEmit(state, effects, target.node, target.offset, toc_auto_scroll);
}

void ReduceFilePaneDirectoryClicked(AppState& state, SideEffectList& effects, const FilePaneDirectoryClickedAction& a)
{
    state.file_explorer.SetDirectory(a.full_path);
    state.view.panes.SidePaneScroll(PaneTarget::File) = {};
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceFilePaneRevealCurrentFile(AppState& state, SideEffectList& effects)
{
    // ヘルプ表示中は移動先のフォルダがない。
    if (!state.document.doc.HasBackingFile()) {
        return;
    }
    state.file_explorer.SetDirectory(state.document.doc.GetDirectory());

    const auto& entries = state.file_explorer.GetEntries();
    const auto it = std::ranges::find_if(entries, &FileEntry::is_current);
    const auto ctx = GetSidePaneContext(state, PaneTarget::File);
    ctx.scroll.scroll_y = it != entries.end()
                              ? CenterPaneScrollOnItem(static_cast<size_t>(std::distance(entries.begin(), it)), state.theme->pane_item_height, ctx.info)
                              : 0.0f;
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceTocItemClicked(AppState& state, SideEffectList& effects, const TocItemClickedAction& a)
{
    PushCurrentNavEntry(state);
    // 目次クリック由来では目次ペインの自動スクロールを抑制する (issue#259)。
    ScrollToHeading(state, effects, a.node_index, /*toc_auto_scroll=*/false);
}

void ReduceNavigateAnchor(AppState& state, SideEffectList& effects, const NavigateAnchorAction& a)
{
    // リンク切れのアンカーで履歴を積むと、進む履歴が消えて戻っても同じ位置になる。
    const int node_index = state.document.doc.FindAnchorIndex(a.anchor_id);
    if (node_index < 0) {
        return;
    }
    PushCurrentNavEntry(state);
    ScrollToHeading(state, effects, node_index, /*toc_auto_scroll=*/true);
}

void ReduceRestoreScrollAfterLoad(AppState& state)
{
    state.view.nav_history.CommitPending();
    auto& viewport = state.view.viewport;
    auto& restore = state.view.scroll_restore;
    if (restore.HasNodeRestore()) {
        viewport.SetScrollTarget(restore.pending_restore_node, static_cast<float>(restore.pending_restore_offset));
        viewport.ApplyScrollTarget(state.document.layout_cache);
        restore.ClearNodeRestore();
    }
    else {
        viewport.SetScrollY(0.0f);
    }
}

// 復元情報は失敗したロード先のノード番号なので、残すと次に開く別ファイル
// (起動時の失敗ならフォールバックのヘルプ) が無関係な位置へスクロールする。
void ReduceLoadFailed(AppState& state)
{
    state.view.nav_history.RollbackPending();
    state.view.scroll_restore.ClearNodeRestore();
}

void ReduceShowHelp(AppState& state, SideEffectList& effects)
{
    if (!IsHelpPath(state.document.doc.GetFilePath())) {
        PushCurrentNavEntry(state, HELP_PATH);
    }
    PushEffect(effects, effect::LoadFile{ std::pmr::wstring(HELP_PATH) });
}

constexpr PaneController::DragTarget SidePaneDragTarget(PaneTarget pane) noexcept
{
    using enum PaneController::DragTarget;
    return (pane == PaneTarget::File) ? FileScrollbar : TocScrollbar;
}

// つまみ上クリック → 位置維持 (オフセットのみ記録)、つまみ外 → 中心へジャンプ。
struct ScrollbarDragGrip {
    float drag_offset;
    bool inside_thumb;
};

ScrollbarDragGrip ComputeScrollbarDragGrip(float thumb_y, float thumb_height, float click_y) noexcept
{
    if (click_y >= thumb_y && click_y <= thumb_y + thumb_height) {
        return { click_y - thumb_y, true };
    }
    return { thumb_height * 0.5f, false };
}

PaneScrollInfo MdScrollInfo(const AppState& state) noexcept
{
    return ComputeScrollInfo(state.pane_layout_cache.Get().md_rect, 0.0f, MdScrollableContentHeight(state));
}

// ドラッグ state の初期化は SetCapture より前に行う。
void BeginScrollbarDrag(AppState& state, SideEffectList& effects, PaneController::DragTarget target, float drag_offset)
{
    state.view.panes.StartDrag(target);
    state.view.panes.SetDragScrollOffset(drag_offset);
    PushEffect(effects, effect::SetCapture{});
}

// ツールチップ以外のホバー状態を反映し、見た目が変わったら再描画させる。
// 横スクロールのドラッグ中は、ポインタがどこへ出てもバーを出したままにする。
void ApplyHoverHighlights(AppState& state, SideEffectList& effects, const PointerHover& h)
{
    bool changed = state.window.titlebar.SetHovered(h.titlebar);
    changed |= state.search.search_bar_ctrl.SetHover(h.search_bar);
    changed |= std::exchange(state.interaction.nav_hover, h.nav) != h.nav;
    changed |= std::exchange(state.interaction.hovered, h.buttons) != h.buttons;
    if (state.view.h_drag_node < 0) {
        changed |= std::exchange(state.view.hovered_h_block, h.h_block) != h.h_block;
    }
    for (const auto t : { PaneTarget::File, PaneTarget::Toc }) {
        const auto k = std::to_underlying(t);
        changed |= state.view.panes.SetHoveredSideIndex(t, h.side_index[k]);
        changed |= state.view.panes.SetSideHoveredButton(t, h.side_button[k]);
    }
    if (changed) {
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ReducePointerHover(AppState& state, SideEffectList& effects, const PointerHover& hover)
{
    ApplyHoverHighlights(state, effects, hover);
    if (!hover.tooltip.SameTarget(state.interaction.tooltip.GetCurrent())) {
        PushEffect(effects, effect::ShowTooltip{ hover.tooltip });
    }
}

// ペインを経由せずに離れた経路 (ウィンドウ外・キャプション上) で呼ばないとホバーハイライトが残留する。
void ReduceMouseLeave(AppState& state, SideEffectList& effects)
{
    ApplyHoverHighlights(state, effects, PointerHover{});
    InvalidateHover(state, effects);
}

void ReduceCaptureChanged(AppState& state, SideEffectList& effects)
{
    state.search.search_bar_ctrl.EndDrag();
    bool invalidate = false;
    if (state.interaction.gesture.GetPhase() != GesturePhase::Idle) {
        state.interaction.gesture.Reset();
        invalidate = true;
    }
    // キャプチャ喪失時 (Alt+Tab・他アプリの SetCapture 等) は WM_LBUTTONUP が
    // 届かないため、進行中の全ドラッグ状態をここで解除する
    if (state.view.viewport.IsDragging()) {
        state.view.viewport.SetDragging(false);
        invalidate = true;
    }
    if (state.view.panes.GetDragTarget() != PaneController::DragTarget::None) {
        state.view.panes.EndDrag();
        invalidate = true;
    }
    if (state.view.h_drag_node >= 0) {
        state.view.h_drag_node = -1;
        invalidate = true;
    }
    if (invalidate) {
        PushEffect(effects, effect::InvalidateWindow{});
    }
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
    const auto target = state.view.panes.GetDragTarget();
    if (!PaneController::IsSplitterDragTarget(target)) {
        return;
    }
    const float splitter_w = state.theme->splitter_width;
    const float before_file = state.view.panes.GetSidePaneWidth(PaneTarget::File);
    const float before_toc = state.view.panes.GetSidePaneWidth(PaneTarget::Toc);
    state.view.panes.DragSplitterTo(target, a.dip_x, state.pane_layout_cache.WindowWidth(), splitter_w);
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
    const auto& ctrl = state.search.search_bar_ctrl;
    if (!ctrl.IsDragging()) {
        return;
    }
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

void ReduceMdScrollbarDragStarted(AppState& state, SideEffectList& effects, const MdScrollbarDragStartedAction& a)
{
    const auto info = MdScrollInfo(state);
    auto& viewport = state.view.viewport;
    const auto grip = ComputeScrollbarDragGrip(ComputeThumbY(info, viewport.GetScrollY()), info.thumb_height, a.dip_y);
    BeginScrollbarDrag(state, effects, PaneController::DragTarget::MdScrollbar, grip.drag_offset);
    // thumb 内クリックなら 1st jump は不要 (thumb-grip オフセット記録だけ)。
    if (!grip.inside_thumb) {
        WithScrollEffects(state, effects, [&] { viewport.ScrollTo(ScrollFromThumbY(info, a.dip_y - grip.drag_offset)); });
    }
}

void ReduceMdScrollbarDragMoved(AppState& state, SideEffectList& effects, const MdScrollbarDragMovedAction& a)
{
    if (state.view.panes.GetDragTarget() != PaneController::DragTarget::MdScrollbar) {
        return;
    }
    const float new_thumb_y = a.dip_y - state.view.panes.GetDragScrollOffset();
    WithScrollEffects(state, effects, [&] { state.view.viewport.ScrollTo(ScrollFromThumbY(MdScrollInfo(state), new_thumb_y)); });
}

void ReduceMdScrollbarDragEnded(AppState& state, SideEffectList& effects)
{
    if (state.view.panes.GetDragTarget() != PaneController::DragTarget::MdScrollbar) {
        return;
    }
    state.view.panes.EndDrag();
    PushEffect(effects, effect::ReleaseCapture{});
    PushEffect(effects, effect::PerformResizeEnd{});
}

void ReducePaneScrollbarDragStarted(AppState& state, SideEffectList& effects, const PaneScrollbarDragStartedAction& a)
{
    const auto ctx = GetSidePaneContext(state, a.pane);
    if (ctx.info.total_content <= ctx.info.content_height) {
        return;
    }
    const auto grip = ComputeScrollbarDragGrip(ComputeThumbY(ctx.info, ctx.scroll.scroll_y), ctx.info.thumb_height, a.dip_y);
    BeginScrollbarDrag(state, effects, SidePaneDragTarget(a.pane), grip.drag_offset);
    if (!grip.inside_thumb) {
        ctx.scroll.scroll_y = ScrollFromThumbY(ctx.info, a.dip_y - grip.drag_offset);
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ReducePaneScrollbarDragMoved(AppState& state, SideEffectList& effects, const PaneScrollbarDragMovedAction& a)
{
    // drag target は pane だけから決まるので、ctx 構築前に短絡し
    // 非ドラッグ時の per-event GetEntries().size() + ComputeScrollInfo を回避。
    if (state.view.panes.GetDragTarget() != SidePaneDragTarget(a.pane)) {
        return;
    }
    const auto ctx = GetSidePaneContext(state, a.pane);
    const float new_thumb_y = a.dip_y - state.view.panes.GetDragScrollOffset();
    ctx.scroll.scroll_y = ScrollFromThumbY(ctx.info, new_thumb_y);
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReducePaneScrollbarDragEnded(AppState& state, SideEffectList& effects)
{
    using enum PaneController::DragTarget;
    const auto drag = state.view.panes.GetDragTarget();
    if (drag != FileScrollbar && drag != TocScrollbar) {
        return;
    }
    state.view.panes.EndDrag();
    PushEffect(effects, effect::ReleaseCapture{});
}

void ReduceTextSelectionStarted(AppState& state, SideEffectList& effects, const TextSelectionStartedAction& a)
{
    state.view.viewport.SetClickStart(a.click_x, a.click_y);
    if (a.node_index < 0) {
        return;
    }
    state.view.viewport.SetAnchor(a.node_index, a.text_pos);
    state.view.viewport.SetDragging(true);
    auto& selection = state.view.viewport.GetSelection();
    const bool had_selection = selection.active;
    selection.Clear();
    PushEffect(effects, effect::SetCapture{});
    if (had_selection) {
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ReduceTextSelectionMoved(AppState& state, SideEffectList& effects, const TextSelectionMovedAction& a)
{
    if (!state.view.viewport.IsDragging() || a.node_index < 0) {
        return;
    }
    // WM_MOUSEMOVE は 16ms 周期で連発するため、選択が同値なら早期 return。
    const auto next = TextSelection::MakeOrdered(
        state.view.viewport.GetAnchorNode(),
        state.view.viewport.GetAnchorPos(),
        a.node_index,
        a.text_pos);
    if (next == state.view.viewport.GetSelection()) {
        return;
    }
    state.view.viewport.SetSelection(next);
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceTextSelectionEnded(AppState& state, SideEffectList& effects, const TextSelectionEndedAction& a)
{
    if (!state.view.viewport.IsDragging()) {
        return;
    }
    bool changed = false;
    if (a.end_node_index >= 0) {
        const auto next = TextSelection::MakeOrdered(
            state.view.viewport.GetAnchorNode(),
            state.view.viewport.GetAnchorPos(),
            a.end_node_index,
            a.end_text_pos);
        changed = next != state.view.viewport.GetSelection();
        state.view.viewport.SetSelection(next);
    }
    state.view.viewport.SetDragging(false);
    PushEffect(effects, effect::ReleaseCapture{});
    // 選択の無いクリックなど、見た目が変わらないなら再描画しない。
    if (changed) {
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ReduceRightClickGestureStarted(AppState& state, SideEffectList& effects, const RightClickGestureStartedAction& a)
{
    state.interaction.gesture.OnRButtonDown(a.dip_x, a.dip_y);
    PushEffect(effects, effect::SetCapture{});
}

void ReduceRightClickGestureMoved(AppState& state, SideEffectList& effects, const RightClickGestureMovedAction& a)
{
    if (state.interaction.gesture.OnMouseMove(a.dip_x, a.dip_y)) {
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
        NavigateHistory(state, effects, /*forward=*/false);
        break;
    case GestureResult::Forward:
        NavigateHistory(state, effects, /*forward=*/true);
        break;
    case GestureResult::None:
        break;
    }
    PushEffect(effects, effect::InvalidateWindow{});
}

// SearchBarController は ScrollToCurrentMatch で viewport を直接動かすため WithScrollEffects で包む。
void ReduceSearchDebounce(AppState& state, SideEffectList& effects)
{
    WithScrollEffects(state, effects, [&] {
        state.search.search_bar_ctrl.OnDebounceTimer(state.document.doc.GetNodes());
    });
}

void ReduceSearchStep(AppState& state, SideEffectList& effects, bool forward)
{
    auto& ss = state.search;
    if (!ss.search_state.IsVisible()) {
        ss.search_bar_ctrl.OnOpen(state.document.doc.GetNodes());
        return;
    }
    WithScrollEffects(state, effects, [&] {
        if (forward) {
            ss.search_bar_ctrl.OnNext();
        }
        else {
            ss.search_bar_ctrl.OnPrev();
        }
    });
}

void ReduceSearchTextChanged(AppState& state, SideEffectList& effects, const SearchTextChangedAction& a)
{
    const auto& doc = state.document.doc;
    WithScrollEffects(state, effects, [&] {
        state.search.search_bar_ctrl.OnTextChanged(a.text, doc.GetNodes(), doc.GetRawText().size());
    });
}

void ReduceTogglePane(AppState& state, SideEffectList& effects, const TogglePaneAction& a)
{
    state.view.panes.ToggleSidePane(a.target);
    state.pane_layout_cache.Invalidate();
    PushEffect(effects, effect::PerformResizeEnd{});
}

void ReduceSelectAll(AppState& state, SideEffectList& effects)
{
    state.view.viewport.SelectAll(state.document.doc.GetNodes());
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceSelectWord(AppState& state, SideEffectList& effects, const SelectWordAction& a)
{
    const auto& nodes = state.document.doc.GetNodes();
    if (a.node_index < 0 || a.node_index >= static_cast<int>(nodes.size())) {
        return;
    }
    const std::string_view text = nodes[a.node_index].LinearizedText();
    if (text.empty()) {
        return;
    }
    const auto wb = FindWordBoundaries(text, a.text_pos);
    if (!wb.found) {
        return;
    }
    state.view.viewport.SetAnchor(a.node_index, wb.start);
    state.view.viewport.SetSelection(TextSelection::MakeOrdered(a.node_index, wb.start, a.node_index, wb.end));
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceClearSelection(AppState& state, SideEffectList& effects)
{
    if (state.search.search_state.IsVisible()) {
        state.search.search_bar_ctrl.OnClose();
        PushEffect(effects, effect::InvalidateWindow{});
        return;
    }
    auto& viewport = state.view.viewport;
    const bool dragging = viewport.IsDragging();
    const bool visible = viewport.GetSelection().active || dragging;
    // ClearSelection はドラッグ状態も落とすため、テキスト選択ドラッグ中だと後続の
    // LButtonUp が TextSelectionEnded に入らずキャプチャが残る。
    if (dragging) {
        PushEffect(effects, effect::ReleaseCapture{});
    }
    viewport.ClearSelection();
    if (visible) {
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ReduceCopyClipboard(const AppState& state, SideEffectList& effects)
{
    const auto& sel = state.view.viewport.GetSelection();
    if (!sel.active) {
        return;
    }
    PushEffect(effects, effect::ClipboardWrite{ ExtractSelectedText(state.document.doc.GetNodes(), sel) });
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

bool StepZoom(ViewportManager& viewport, ZoomDirection direction)
{
    switch (direction) {
    case ZoomDirection::In:
        return viewport.ZoomIn();
    case ZoomDirection::Out:
        return viewport.ZoomOut();
    case ZoomDirection::Reset:
        return viewport.ZoomReset();
    }
    std::unreachable();
}

void ReduceZoom(AppState& state, SideEffectList& effects, const ZoomAction& a)
{
    if (!StepZoom(state.view.viewport, a.direction)) {
        return;
    }
    const auto anchor = SnapshotVisibleTarget(state);
    state.pane_layout_cache.Invalidate();
    const float new_zoom = state.view.viewport.GetCurrentZoom();
    const float zoom_ratio = new_zoom / state.theme->zoom;
    state.view.panes.ApplyZoom(zoom_ratio);
    state.document.layout_cache.InvalidateAllLayouts(state.document.doc.GetNodes());
    if (anchor.IsValid()) {
        // offset もズーム比でスケールしないと、ノード内の同じ位置が可視先頭に残らない。
        state.view.viewport.SetScrollTarget(anchor.node, anchor.offset * zoom_ratio);
    }
    PushEffect(effects, effect::ApplyThemeChange{ effect::ApplyThemeChange::Type::Zoom });
}

void ReduceToggleDarkMode(AppState& state, SideEffectList& effects)
{
    const auto anchor = SnapshotVisibleTarget(state);
    state.pane_layout_cache.Invalidate();
    // 色のみの変更なのでテキストレイアウトは維持する。描画エフェクトの固定ブラシは SetColor で
    // 色だけ差し替わるので再適用不要で、テーマ色を焼き込んだ Mermaid bitmap のみ破棄する。
    state.document.layout_cache.InvalidateDiagramBitmaps(state.document.doc.GetNodes());
    if (anchor.IsValid()) {
        // Mermaid 再レンダリングで微小な高さ変化が起きるので target で追従する。
        state.view.viewport.SetScrollTarget(anchor.node, anchor.offset);
    }
    PushEffect(effects, effect::ApplyThemeChange{ effect::ApplyThemeChange::Type::DarkMode });
}

void ReduceActivate(AppState& state, SideEffectList& effects, const ActivateAction& a)
{
    if (state.window.window_active != a.active) {
        state.window.window_active = a.active;
        PushEffect(effects, effect::InvalidateWindow{});
        state.search.search_bar_ctrl.OnWindowActivate(a.active);
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
    // 最小化からの復元などで同じ大きさの WM_SIZE が来ても、描画面もレイアウトも変わらない。
    if (a.width == state.window.client_width && a.height == state.window.client_height) {
        return;
    }
    state.window.client_width = a.width;
    state.window.client_height = a.height;
    state.window.size_changed_in_sizing = true;
    state.pane_layout_cache.Invalidate();
    PushEffect(effects, effect::RendererResize{ a.width, a.height });
    const float window_w_dip = PixelToDip(static_cast<float>(a.width), state.window.cached_dpi_scale);
    state.window.titlebar.UpdateLayout(window_w_dip);
    if (state.window.is_sizing) {
        PushEffect(effects, effect::PerformSizingUpdate{});
    }
    else {
        PushEffect(effects, effect::PerformResizeEnd{});
    }
}

void ReduceExitSizeMove(AppState& state, SideEffectList& effects)
{
    state.window.is_sizing = false;
    if (state.window.size_changed_in_sizing) {
        PushEffect(effects, effect::PerformResizeEnd{});
    }
}

void ReduceDpiChanged(AppState& state, SideEffectList& effects, const DpiChangedAction& a)
{
    state.window.cached_dpi_scale = DpiScaleFrom(static_cast<float>(a.dpi));
    // ピクセルサイズ不変でも DIP サイズが変わるため、移動ループ終了時のレイアウトを要求する。
    // 続く WM_SIZE が同じピクセルサイズでも処理させる。
    state.window.size_changed_in_sizing = true;
    state.window.client_width = 0;
    state.window.client_height = 0;
    state.pane_layout_cache.Invalidate();
    // DPI 変更では IDWriteTextLayout (DIP 単位) は不変。
    state.document.layout_cache.NotifyDpiChanged();
    PushEffect(effects, effect::RendererSetDpi{ static_cast<float>(a.dpi) });
    const auto& rc = a.suggested;
    PushEffect(effects, effect::SetWindowPosition{ rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top });
}

void ReduceTimer(AppState& state, SideEffectList& effects, const TimerAction& a)
{
    switch (a.timer_id) {
    case app_timer::Id::TOAST:
        state.interaction.toast.Hide();
        PushEffect(effects, effect::KillTimer{ app_timer::Id::TOAST });
        PushEffect(effects, effect::InvalidateWindow{});
        return;
    case app_timer::Id::TOOLTIP:
        PushEffect(effects, effect::KillTimer{ app_timer::Id::TOOLTIP });
        state.interaction.tooltip.Show();
        return;
    case app_timer::Id::SEARCH_DEBOUNCE:
        ReduceSearchDebounce(state, effects);
        return;
    case app_timer::Id::SWIPE_OVERLAY: {
        const auto result = state.interaction.swipe_detector.Commit();
        PushEffect(effects, effect::KillTimer{ app_timer::Id::SWIPE_OVERLAY });
        if (result != NavDirection::None) {
            NavigateHistory(state, effects, result == NavDirection::Forward);
            PushEffect(effects, effect::InvalidateWindow{});
        }
        return;
    }
    default:
        // 状態を持たないサービスのタイマーは App::HandleTimer が直接処理する。
        return;
    }
}

} // namespace

SideEffectList Reduce(AppState& state, const AppAction& action)
{
    SideEffectList effects;

    // clang-format off
    std::visit(overloaded{
        [](const NoOpAction&) {},

        // ---- スクロール ----
        [&](const KeyScrollAction& a) { ReduceKeyScroll(state, effects, a); },
        [&](const DirectScrollByAction& a) { ReduceDirectScrollBy(state, effects, a); },
        [&](const ScrollPaneAction& a) { ReduceScrollPane(state, effects, a); },

        // ---- ペイン・選択・クリップボード ----
        [&](const TogglePaneAction& a) { ReduceTogglePane(state, effects, a); },
        [&](const SelectAllAction&) { ReduceSelectAll(state, effects); },
        [&](const SelectWordAction& a) { ReduceSelectWord(state, effects, a); },
        [&](const ClearSelectionAction&) { ReduceClearSelection(state, effects); },
        [&](const CopyClipboardAction&) { ReduceCopyClipboard(state, effects); },
        [&](const CopyFormattedClipboardAction&) { ReduceCopyFormattedClipboard(state, effects); },

        // ---- ズーム・テーマ ----
        [&](const ZoomAction& a) { ReduceZoom(state, effects, a); },
        [&](const ToggleDarkModeAction&) { ReduceToggleDarkMode(state, effects); },

        // ---- ウィンドウ・システムイベント ----
        [&](const ActivateAction& a) { ReduceActivate(state, effects, a); },
        [&](const EnterSizeMoveAction&) {
            state.window.is_sizing = true;
            state.window.size_changed_in_sizing = false;
        },
        [&](const ExitSizeMoveAction&) { ReduceExitSizeMove(state, effects); },
        [&](const ResizeAction& a) { ReduceResize(state, effects, a); },
        [&](const DpiChangedAction& a) { ReduceDpiChanged(state, effects, a); },
        [&](const HWheelAction& a) { ReduceHWheel(state, effects, a); },
        [&](const BlockHScrollDragStartedAction& a) { ReduceBlockHScrollDragStarted(state, effects, a); },
        [&](const BlockHScrollDragMovedAction& a) { ReduceBlockHScrollDragMoved(state, effects, a); },
        [&](const BlockHScrollDragEndedAction&) { ReduceBlockHScrollDragEnded(state, effects); },

        // ---- 検索 ----
        [&](const OpenSearchBarAction&) { state.search.search_bar_ctrl.OnOpen(state.document.doc.GetNodes()); },
        [&](const CloseSearchBarAction&) { state.search.search_bar_ctrl.OnClose(); },
        [&](const SearchNextAction&) { ReduceSearchStep(state, effects, true); },
        [&](const SearchPrevAction&) { ReduceSearchStep(state, effects, false); },
        [&](const SearchTextChangedAction& a) { ReduceSearchTextChanged(state, effects, a); },
        [&](const ToggleCaseSensitiveAction&) { state.search.search_bar_ctrl.OnToggleCaseSensitive(state.document.doc.GetNodes()); },
        [&](const ToggleHighlightAction&) { state.search.search_bar_ctrl.OnToggleHighlight(); },
        [&](const SearchSelectionAction& a) { state.search.search_bar_ctrl.SetSelection(a.sel_start, a.sel_end); },
        [&](const ImeCompositionAction& a) { state.search.search_bar_ctrl.SetImeComposition(a.text); },

        // ---- マウス関連 ----
        [&](const MouseLeaveAction&) { ReduceMouseLeave(state, effects); },
        [&](const CaptureChangedAction&) { ReduceCaptureChanged(state, effects); },
        [&](const PointerHoverAction& a) { ReducePointerHover(state, effects, a.hover); },
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
        [&](const FilePaneFileClickedAction& a) { OpenFileWithHistory(state, effects, a.full_path); },
        [&](const FilePaneRevealCurrentFileAction&) { ReduceFilePaneRevealCurrentFile(state, effects); },
        [&](const TocItemClickedAction& a) { ReduceTocItemClicked(state, effects, a); },
        [&](const NavigateAnchorAction& a) { ReduceNavigateAnchor(state, effects, a); },
        [&](const RestoreScrollAfterLoadAction&) { ReduceRestoreScrollAfterLoad(state); },
        [&](const LoadFailedAction&) { ReduceLoadFailed(state); },
        [&](const InvalidateHoverAction&) { InvalidateHover(state, effects); },

        // ---- ナビゲーション ----
        [&](const NavigateBackAction&) { NavigateHistory(state, effects, /*forward=*/false); },
        [&](const NavigateForwardAction&) { NavigateHistory(state, effects, /*forward=*/true); },

        // ---- ファイル操作 ----
        [&](const DropFilesAction& a) { OpenFileWithHistory(state, effects, a.path); },
        [&](const ShowHelpAction&) { ReduceShowHelp(state, effects); },
        [&](const OpenFileAction&) { PushEffect(effects, effect::OpenFileDialog{}); },
        [&](const ReloadFileAction&) { PushEffect(effects, effect::ReloadFile{}); },

        // ---- 非同期・タイマー ----
        [&](const TimerAction& a) { ReduceTimer(state, effects, a); },
    }, action);
    // clang-format on
    return effects;
}
