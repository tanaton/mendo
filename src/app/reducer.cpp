#include "reducer.h"
#include "app_constants.h"
#include "block_h_scroll.h"
#include "document_utils.h"
#include "file_io.h"
#include "layout_computer.h"
#include "selection_html.h"
#include "string_convert.h"
#include "ui_constants.h"
#include "utility.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace {

NavEntry CurrentNavEntry(const AppState& state)
{
    const ScrollTarget t = SnapshotVisibleTarget(state);
    return NavEntry{ state.document.doc.GetFilePath(), t.node, t.offset };
}

// ロード中の別ファイルが取り消されたか失敗したときに、その仮の移動と移動先用の復元位置を捨てる。
// 復元位置を残すと、次に開く別ファイルが無関係なノード位置へスクロールする。
void DiscardPendingLoad(AppState& state)
{
    state.view.nav_history.RollbackPending();
    state.view.pending_restore = {};
}

// load_path: これから開くファイル。現在と別のファイルなら、ロード失敗 (LoadFailedAction) で取り消す仮の履歴にする。
void PushCurrentNavEntry(AppState& state, std::wstring_view load_path = {})
{
    const auto& current_path = state.document.doc.GetFilePath();
    // doc 未ロード (パス空) の状態は戻り先にならないため積まない
    if (current_path.empty()) {
        return;
    }
    NavEntry entry = CurrentNavEntry(state);
    if (load_path.empty()) {
        state.view.nav_history.Push(std::move(entry));
        return;
    }
    // 新しいロードはロード中の別ファイルを取り消す。
    DiscardPendingLoad(state);
    // 同じファイルの開き直しはリロードとして終わり、確定 (DocumentOpenedAction) が来ない。
    if (path_util::iequal(load_path, current_path)) {
        state.view.nav_history.Push(std::move(entry));
    }
    else {
        state.view.nav_history.PushPending(std::move(entry));
    }
}

} // namespace

const PaneLayout& EnsurePaneLayout(AppState& state) noexcept
{
    auto& cache = state.pane_layout_cache;
    // WM_NCHITTEST は Init 前にも届く。大きさが決まる (最初の WM_SIZE) までは空の配置のまま。
    if (!cache.IsValid() && state.theme && state.window.client_width > 0) {
        const float width = state.window.WidthDip();
        const float height = PixelToDip(static_cast<float>(state.window.client_height), state.window.cached_dpi_scale);
        cache.Set(state.view.panes.ComputeLayout(width, height, state.theme->splitter_width, state.window.titlebar.GetHeight()));
    }
    return cache.Get();
}

SidePaneContext GetSidePaneContext(AppState& state, PaneTarget pane)
{
    const float item_h = SidePaneItemHeight(state, pane);
    const float header_h = state.theme->pane_header_height;
    const auto& layout = state.pane_layout_cache.Get();
    return {
        ComputeScrollInfo(layout.Get(pane), header_h, SidePaneContentHeight(SidePaneItemCount(state, pane), item_h)),
        item_h,
        state.view.panes.SidePaneScroll(pane),
    };
}

void SyncActiveTocIndex(AppState& state)
{
    const bool follow = !std::exchange(state.view.keep_toc_pane_once, false);
    if (!state.theme || !state.view.panes.IsSidePaneVisible(PaneTarget::Toc)) {
        return;
    }
    const int new_active = layout::FindActiveTocIndex(
        state.document.doc.GetToc(), state.document.layout_cache, state.view.viewport.GetScrollY(), state.theme->heading_spacing_above);
    if (new_active == state.view.active_toc_index) {
        return;
    }
    state.view.active_toc_index = new_active;
    if (!follow || new_active < 0) {
        return;
    }
    // アクティブ見出しを目次ペインの中央に保つように追従スクロールする。
    const auto ctx = GetSidePaneContext(state, PaneTarget::Toc);
    if (ctx.info.content_height > 0.0f) {
        ctx.scroll.scroll_y = CenterPaneScrollOnItem(static_cast<size_t>(new_active), ctx.item_height, ctx.info);
    }
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
    return GetBlockHScrollGeometry(nodes, cache, static_cast<size_t>(node_index), *state.theme, state.pane_layout_cache.Get().md_rect.width);
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

float MdMaxScroll(const AppState& state) noexcept
{
    return MaxScroll(MdScrollableContentHeight(state), state.pane_layout_cache.Get().md_rect.height);
}

namespace {

// 待たせていたリロードを予約する。LButtonUp の処理中に文書を差し替えないよう、タイマーで後に回す。
void ClearDrag(AppState& state, SideEffectList& effects)
{
    state.view.drag = {};
    if (std::exchange(state.view.reload_after_drag, false)) {
        PushEffect(effects, effect::SetTimer{ app_timer::Id::FILE_RELOAD_DEBOUNCE, app_timer::FILE_RELOAD_RETRY_MS });
    }
}

// 進行中の左ドラッグを終え、キャプチャを解放させる (右ボタンのジェスチャーが使っている間は持ち続ける)。
void EndLeftDrag(AppState& state, SideEffectList& effects)
{
    ClearDrag(state, effects);
    if (state.interaction.gesture.GetPhase() == GesturePhase::Idle) {
        PushEffect(effects, effect::ReleaseCapture{});
    }
}

void BeginLeftDrag(AppState& state, SideEffectList& effects, const LeftDragState& drag)
{
    state.view.drag = drag;
    PushEffect(effects, effect::SetCapture{});
}

// 対象も忘れるので、次のホバーで同じ対象に戻っても表示し直す。対象が無ければ隠れているので何もしない。
void ClearTooltip(AppState& state, SideEffectList& effects)
{
    if (std::exchange(state.interaction.tooltip, {}).IsEmpty()) {
        return;
    }
    PushEffect(effects, effect::SetTooltip{});
}

// スクロール位置が変わった時の副作用列。Reduce が前後の scroll_y を比べて 1 回だけ積む。
// 目次のアクティブ見出しは描画時に SyncActiveTocIndex が決め直す。
void EmitScrollChangedSideEffects(AppState& state, SideEffectList& effects)
{
    ClearTooltip(state, effects);
    PushEffect(effects, effect::InvalidateWindow{});
    PushEffect(effects, effect::BitmapManage{});
}

void ApplyScrollTarget(AppState& state, int node, float offset)
{
    state.view.viewport.SetScrollTarget(node, offset);
    state.view.viewport.ApplyScrollTarget(state.document.layout_cache);
    // 末尾セクションへのジャンプで scroll_y > max_scroll になると、後続の DirectScrollBy で
    // 位置が一気に飛ぶ。事前クランプ + target 無効化で範囲外への復帰を抑える。
    state.view.viewport.ClampAndDetach(MdMaxScroll(state));
}

// new_value は差分ではなく絶対値。
bool SetBlockScrollX(AppState& state, float new_value, const BlockHScrollGeometry& geom)
{
    const float clamped = geom.ClampScrollX(new_value);
    const float prev = state.view.GetBlockScrollX(geom);
    if (std::abs(prev - clamped) < 1e-3f) {
        return false;
    }
    if (clamped <= 0.0f) {
        state.view.block_scroll_x.erase(geom.scroll_key);
    }
    else {
        state.view.block_scroll_x[geom.scroll_key] = clamped;
    }
    return true;
}

void ReduceKeyScroll(AppState& state, const KeyScrollAction& a)
{
    auto& viewport = state.view.viewport;
    const float page_size = state.pane_layout_cache.Get().md_rect.height;
    const float max_scroll = MdMaxScroll(state);
    switch (a.type) {
    case ScrollType::LineUp:
        viewport.DirectScrollBy(-SCROLL_LINE_AMOUNT, max_scroll);
        return;
    case ScrollType::LineDown:
        viewport.DirectScrollBy(SCROLL_LINE_AMOUNT, max_scroll);
        return;
    case ScrollType::PageUp:
        viewport.DirectScrollBy(-page_size * SCROLL_PAGE_FACTOR, max_scroll);
        return;
    case ScrollType::PageDown:
        viewport.DirectScrollBy(page_size * SCROLL_PAGE_FACTOR, max_scroll);
        return;
    case ScrollType::Home:
        // ノード 0 の target だと scroll_y = Top(0) になり上余白が隠れる (ロード直後の 0 と不一致)。
        // 短い文書では max_scroll も超えるため、ピクセルで 0 を指定する。
        viewport.ScrollTo(0.0f, max_scroll);
        return;
    case ScrollType::End:
        // 末尾は max_scroll に依存するためピクセル指定。scroll target は無効化される。
        viewport.ScrollTo(max_scroll, max_scroll);
        return;
    }
    std::unreachable();
}

// 縦スクロールが発生した時点で軸ロックを更新 (再武装) し、直後の水平ホイールが
// スワイプとして誤検出されないようにする。
void NotifyVWheel(AppState& state, SideEffectList& effects, uint64_t tick)
{
    auto& swipe = state.interaction.swipe_detector;
    const bool had_overlay = swipe.IsOverlayVisible();
    swipe.NotifyVScroll(tick);
    if (had_overlay) {
        PushEffect(effects, effect::KillTimer{ app_timer::Id::SWIPE_OVERLAY });
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ReduceWheelScroll(AppState& state, SideEffectList& effects, const MouseWheelAction& a)
{
    NotifyVWheel(state, effects, a.tick);
    const float amount = -static_cast<float>(a.delta) * MOUSE_WHEEL_SCROLL_MULTIPLIER;
    const auto pane = ToPaneTarget(a.zone);
    if (!pane) {
        state.view.viewport.DirectScrollBy(amount, MdMaxScroll(state));
        return;
    }
    const auto ctx = GetSidePaneContext(state, *pane);
    if (state.view.panes.ScrollSidePaneBy(*pane, amount, ctx.info.max_scroll)) {
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ReduceHWheel(AppState& state, SideEffectList& effects, const HWheelAction& a)
{
    // ホバー or ドラッグ中のブロックが横スクロール可能ならそこに適用し、スワイプオーバーレイには流さない。
    const int drag_node = state.view.HDragNode();
    const int target = (drag_node >= 0) ? drag_node : state.view.hover.h_block;
    if (target >= 0) {
        // pane_layout_cache 未確立時は ResolveBlockHScrollGeometry が can_scroll()=false を返す。
        const auto geom = ResolveBlockHScrollGeometry(state, target);
        if (!geom.can_scroll()) {
            return;
        }
        const float dx = static_cast<float>(a.delta) / WHEEL_DELTA * HSCROLL_DIP_PER_NOTCH;
        if (SetBlockScrollX(state, state.view.GetBlockScrollX(geom) + dx, geom)) {
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
    BeginLeftDrag(state, effects, { .kind = LeftDrag::BlockHScroll, .block_node = a.node_index, .start_x = a.dip_x, .start_scroll = state.view.GetBlockScrollX(geom) });
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceBlockHScrollDragMoved(AppState& state, SideEffectList& effects, const BlockHScrollDragMovedAction& a)
{
    auto& sv = state.view;
    const int node = sv.HDragNode();
    if (node < 0) {
        return;
    }
    const auto geom = ResolveBlockHScrollGeometry(state, node);
    if (!geom.can_scroll()) {
        return;
    }
    // dip_delta はドラッグの sum = サムが動く DIP。サムが (track - thumb_w) 動いたとき
    // コンテンツが scroll_max 動くべきなので、scroll_delta = dip_delta * scroll_max / drag_range。
    // thumb_w が PANE_SCROLLBAR_THUMB_MIN に張り付いた長大コンテンツでも 1:1 で対応する。
    const float dip_delta = a.dip_x - sv.drag.start_x;
    const float thumb_w = BlockHScrollbarThumbWidth(geom.visible_width, geom.block_width);
    const float drag_range = std::max(1.0f, geom.visible_width - thumb_w);
    const float scroll_max = geom.scroll_max();
    if (SetBlockScrollX(state, sv.drag.start_scroll + dip_delta * scroll_max / drag_range, geom)) {
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ApplyNavResult(AppState& state, SideEffectList& effects, NavEntry&& entry)
{
    if (!entry.file_path.empty() && !path_util::iequal(entry.file_path, state.document.doc.GetFilePath())) {
        state.view.pending_restore = { entry.node, entry.offset };
        PushEffect(effects, effect::LoadFile{ std::move(entry.file_path) });
    }
    else {
        // 戻る/進むを続けて表示中の文書へ戻ったとき、途中で開き始めたファイルが後から表示されないようにする。
        PushEffect(effects, effect::CancelOtherFileLoad{});
        // 取り消すファイル用の復元位置なので、次に開くファイルへ持ち越さない。
        state.view.pending_restore = {};
        // ロードを伴わないので、ここで確定しないと後の無関係なロード失敗で取り消される。
        state.view.nav_history.CommitPending();
        ApplyScrollTarget(state, entry.node, entry.offset);
    }
}

void NavigateHistory(AppState& state, SideEffectList& effects, bool forward)
{
    auto& history = state.view.nav_history;
    NavEntry out;
    const bool moved = forward ? history.GoForward(CurrentNavEntry(state), out) : history.GoBack(CurrentNavEntry(state), out);
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

void ScrollToHeading(AppState& state, int node_index)
{
    if (node_index < 0) {
        return;
    }
    const auto target = MakeHeadingTopTarget(node_index, state.theme->heading_spacing_above);
    ApplyScrollTarget(state, target.node, target.offset);
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
    state.file_explorer.EnsureListed();

    const auto& entries = state.file_explorer.GetEntries();
    if (state.file_explorer.IsListing()) {
        state.file_explorer.DeferReveal();
        return;
    }
    const auto it = std::ranges::find_if(entries, &FileEntry::is_current);
    const auto ctx = GetSidePaneContext(state, PaneTarget::File);
    ctx.scroll.scroll_y = it != entries.end()
                              ? CenterPaneScrollOnItem(static_cast<size_t>(std::distance(entries.begin(), it)), ctx.item_height, ctx.info)
                              : 0.0f;
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceTocItemClicked(AppState& state, const TocItemClickedAction& a)
{
    PushCurrentNavEntry(state);
    ScrollToHeading(state, a.node_index);
    state.view.keep_toc_pane_once = true;
}

void ReduceNavigateAnchor(AppState& state, const NavigateAnchorAction& a)
{
    // リンク切れのアンカーで履歴を積むと、進む履歴が消えて戻っても同じ位置になる。
    const int node_index = state.document.doc.FindAnchorIndex(a.anchor_id);
    if (node_index < 0) {
        return;
    }
    PushCurrentNavEntry(state);
    ScrollToHeading(state, node_index);
}

void RestoreScrollAfterLoad(AppState& state)
{
    state.view.nav_history.CommitPending();
    auto& viewport = state.view.viewport;
    if (const ScrollTarget restore = std::exchange(state.view.pending_restore, {}); restore.IsValid()) {
        viewport.SetScrollTarget(restore.node, restore.offset);
        viewport.ApplyScrollTarget(state.document.layout_cache);
    }
    else {
        viewport.SetScrollY(0.0f);
    }
}

void ReduceShowHelp(AppState& state, SideEffectList& effects)
{
    // ヘルプ表示中はヘルプに留まり、ロード中の別ファイルだけを取り消す。
    if (IsHelpPath(state.document.doc.GetFilePath())) {
        DiscardPendingLoad(state);
        PushEffect(effects, effect::CancelOtherFileLoad{});
        return;
    }
    PushCurrentNavEntry(state, HELP_PATH);
    PushEffect(effects, effect::LoadFile{ std::pmr::wstring(HELP_PATH) });
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

// ツールチップ以外のホバー状態を反映し、見た目が変わったら再描画させる。
void ApplyHoverHighlights(AppState& state, SideEffectList& effects, HoverHighlights h)
{
    // 入力欄はテキスト編集領域でホバー強調は不要。
    if (h.search_bar == SearchBarHitZone::Input) {
        h.search_bar = SearchBarHitZone::None;
    }
    // 横スクロールのドラッグ中は、ポインタがどこへ出てもバーを出したままにする。
    if (state.view.HDragNode() >= 0) {
        h.h_block = state.view.hover.h_block;
    }
    if (std::exchange(state.view.hover, h) != h) {
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ReducePointerHover(AppState& state, SideEffectList& effects, const PointerHover& hover)
{
    ApplyHoverHighlights(state, effects, hover);
    if (hover.tooltip.target != state.interaction.tooltip) {
        state.interaction.tooltip = hover.tooltip.target;
        PushEffect(effects, effect::SetTooltip{ hover.tooltip.text });
    }
}

// ペインを経由せずに離れた経路 (ウィンドウ外・キャプション上) で呼ばないとホバーハイライトが残留する。
void ReduceMouseLeave(AppState& state, SideEffectList& effects)
{
    ApplyHoverHighlights(state, effects, {});
    ClearTooltip(state, effects);
}

void ReduceCaptureChanged(AppState& state, SideEffectList& effects)
{
    bool invalidate = false;
    if (state.interaction.gesture.GetPhase() != GesturePhase::Idle) {
        state.interaction.gesture.Reset();
        invalidate = true;
    }
    // キャプチャ喪失時 (Alt+Tab・他アプリの SetCapture 等) は WM_LBUTTONUP が
    // 届かないため、進行中のドラッグをここで終える。キャプチャは既に無いので解放しない。
    if (state.view.drag.kind != LeftDrag::None) {
        ClearDrag(state, effects);
        invalidate = true;
    }
    if (invalidate) {
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ReduceSplitterDragStarted(AppState& state, SideEffectList& effects, const SplitterDragStartedAction& a)
{
    BeginLeftDrag(state, effects, { .kind = LeftDrag::Splitter, .pane = a.edge });
}

void ReduceSplitterDragMoved(AppState& state, SideEffectList& effects, const SplitterDragMovedAction& a)
{
    if (state.view.drag.kind != LeftDrag::Splitter) {
        return;
    }
    const float splitter_w = state.theme->splitter_width;
    const float before_file = state.view.panes.GetSidePaneWidth(PaneTarget::File);
    const float before_toc = state.view.panes.GetSidePaneWidth(PaneTarget::Toc);
    state.view.panes.DragSplitterTo(state.view.drag.pane, a.dip_x, state.window.WidthDip(), splitter_w);
    if (state.view.panes.GetSidePaneWidth(PaneTarget::File) == before_file && state.view.panes.GetSidePaneWidth(PaneTarget::Toc) == before_toc) {
        return;
    }
    state.pane_layout_cache.Invalidate();
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceLeftDragEnded(AppState& state, SideEffectList& effects)
{
    const LeftDrag drag = state.view.drag.kind;
    if (drag == LeftDrag::None || drag == LeftDrag::TextSelect) {
        return;
    }
    EndLeftDrag(state, effects);
    if (drag == LeftDrag::Splitter || drag == LeftDrag::MdScrollbar) {
        PushEffect(effects, effect::PerformResizeEnd{});
    }
    else if (drag == LeftDrag::BlockHScroll) {
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ReduceSearchInputDragStarted(AppState& state, SideEffectList& effects, const SearchInputDragStartedAction& a)
{
    BeginLeftDrag(state, effects, { .kind = LeftDrag::SearchInput, .search_anchor = a.caret_pos });
    PushEffect(effects, effect::SearchFocus{ a.caret_pos, a.caret_pos });
}

void ReduceSearchInputDragMoved(AppState& state, SideEffectList& effects, const SearchInputDragMovedAction& a)
{
    if (state.view.drag.kind != LeftDrag::SearchInput) {
        return;
    }
    const auto& sg = state.search;
    const int anchor = state.view.drag.search_anchor;
    if (a.caret_pos == sg.caret_pos && anchor == sg.selection_start) {
        return;
    }
    PushEffect(effects, effect::SearchFocus{ anchor, a.caret_pos });
}

void ReduceMdScrollbarDragStarted(AppState& state, SideEffectList& effects, const MdScrollbarDragStartedAction& a)
{
    const auto info = MdScrollInfo(state);
    auto& viewport = state.view.viewport;
    const auto grip = ComputeScrollbarDragGrip(ComputeThumbY(info, viewport.GetScrollY()), info.thumb_height, a.dip_y);
    BeginLeftDrag(state, effects, { .kind = LeftDrag::MdScrollbar, .grip_offset = grip.drag_offset });
    // thumb 内クリックなら 1st jump は不要 (thumb-grip オフセット記録だけ)。
    if (!grip.inside_thumb) {
        viewport.ScrollTo(ScrollFromThumbY(info, a.dip_y - grip.drag_offset), MdMaxScroll(state));
    }
}

void ReduceMdScrollbarDragMoved(AppState& state, const MdScrollbarDragMovedAction& a)
{
    if (state.view.drag.kind != LeftDrag::MdScrollbar) {
        return;
    }
    const float new_thumb_y = a.dip_y - state.view.drag.grip_offset;
    state.view.viewport.ScrollTo(ScrollFromThumbY(MdScrollInfo(state), new_thumb_y), MdMaxScroll(state));
}

void ReducePaneScrollbarDragStarted(AppState& state, SideEffectList& effects, const PaneScrollbarDragStartedAction& a)
{
    const auto ctx = GetSidePaneContext(state, a.pane);
    if (ctx.info.total_content <= ctx.info.content_height) {
        return;
    }
    const auto grip = ComputeScrollbarDragGrip(ComputeThumbY(ctx.info, ctx.scroll.scroll_y), ctx.info.thumb_height, a.dip_y);
    BeginLeftDrag(state, effects, { .kind = LeftDrag::PaneScrollbar, .pane = a.pane, .grip_offset = grip.drag_offset });
    if (!grip.inside_thumb) {
        ctx.scroll.scroll_y = ScrollFromThumbY(ctx.info, a.dip_y - grip.drag_offset);
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ReducePaneScrollbarDragMoved(AppState& state, SideEffectList& effects, const PaneScrollbarDragMovedAction& a)
{
    if (state.view.drag.kind != LeftDrag::PaneScrollbar) {
        return;
    }
    const auto ctx = GetSidePaneContext(state, state.view.drag.pane);
    const float new_thumb_y = a.dip_y - state.view.drag.grip_offset;
    ctx.scroll.scroll_y = ScrollFromThumbY(ctx.info, new_thumb_y);
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceTextSelectionStarted(AppState& state, SideEffectList& effects, const TextSelectionStartedAction& a)
{
    if (a.node_index < 0) {
        return;
    }
    auto& selection = state.view.viewport.GetSelection();
    const bool had_selection = selection.active();
    selection.Clear();
    BeginLeftDrag(state, effects, { .kind = LeftDrag::TextSelect, .select_node = a.node_index, .select_pos = a.text_pos, .click_x = a.click_x, .click_y = a.click_y });
    if (had_selection) {
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ReduceTextSelectionMoved(AppState& state, SideEffectList& effects, const TextSelectionMovedAction& a)
{
    if (state.view.drag.kind != LeftDrag::TextSelect || a.node_index < 0) {
        return;
    }
    // WM_MOUSEMOVE は 16ms 周期で連発するため、選択が同値なら早期 return。
    const auto next = state.view.drag.SelectionTo(a.node_index, a.text_pos);
    if (next == state.view.viewport.GetSelection()) {
        return;
    }
    state.view.viewport.SetSelection(next);
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceTextSelectionEnded(AppState& state, SideEffectList& effects, const TextSelectionEndedAction& a)
{
    if (state.view.drag.kind != LeftDrag::TextSelect) {
        return;
    }
    bool changed = false;
    if (a.end_node_index >= 0) {
        const auto next = state.view.drag.SelectionTo(a.end_node_index, a.end_text_pos);
        changed = next != state.view.viewport.GetSelection();
        state.view.viewport.SetSelection(next);
    }
    EndLeftDrag(state, effects);
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

// ---- 検索バー ----

// 1 回でこれを超える分は、メッセージループが入力と描画を配った後の空き時間 (SearchContinueAction) へ回す。
constexpr auto kSearchSliceBudget = std::chrono::milliseconds(8);

constexpr void ResetSearchCaret(SearchGroup& sg) noexcept
{
    sg.caret_pos = -1;
    sg.selection_start = -1;
}

void ScrollToCurrentMatch(AppState& state, NodeLayoutAccess& layout)
{
    const auto& ss = state.search.search_state;
    const int idx = ss.GetCurrentMatchIndex();
    if (idx < 0 || idx >= ss.GetMatchCount()) {
        return;
    }
    const auto& match = ss.GetMatches()[idx];
    const auto& cache = state.document.layout_cache;
    if (match.node_index < 0 || match.node_index >= static_cast<int>(cache.size())) {
        return;
    }

    // 未計測や evict 済みのノードは text_layout が無く、行の Y を引けずにノード先頭へ寄ってしまう。
    layout.EnsureNodeLayout(match.node_index);
    const PaneRect& md = state.pane_layout_cache.Get().md_rect;
    const auto& entry = cache[match.node_index];
    // Why: ブロック先頭/行先頭に丸めると、長い段落内の複数マッチ間で同じ Y に集約され
    //      「次へ」を押してもスクロールしない。start_w で行単位の Y を出す。
    const auto [match_y, match_h] = entry.GetMatchYRange(match.table_row, match.table_col, match.start_w, cache.Top(static_cast<size_t>(match.node_index)));
    const float visible_height = md.height - (ss.IsVisible() ? SEARCH_BAR_HEIGHT : 0.0f);
    auto& viewport = state.view.viewport;
    const float scroll_y = viewport.GetScrollY();
    const float effective_bottom = scroll_y + visible_height;

    if (match_y < scroll_y || match_y + match_h > effective_bottom) {
        viewport.ScrollTo(match_y - visible_height / 3.0f, MdMaxScroll(state));
    }
}

void ContinueSearch(AppState& state, SideEffectList& effects, NodeLayoutAccess& layout, bool force_invalidate = false)
{
    auto& ss = state.search.search_state;
    const bool done = ss.ContinueSearch(state.document.doc.GetNodes(), std::chrono::steady_clock::now() + kSearchSliceBudget);
    // 結果は終わったときにまとめて差し替わるので、続きの区切りでは描き直さない。
    if (force_invalidate || done) {
        PushEffect(effects, effect::InvalidateWindow{});
    }
    if (!done) {
        return;
    }
    // 差し替えで選択は外れている。途中で前回の結果を辿っていても、見ている位置の近くで選び直す。
    ss.SetCurrentMatchNear(state.view.viewport.GetScrollY(), state.document.layout_cache);
    if (state.search.scroll_after_search) {
        ScrollToCurrentMatch(state, layout);
    }
}

// 今のクエリで検索し直す。終わった時点で見ている位置の近くのマッチを選び、scroll_to_match ならそこへスクロールする。
void RunSearchAndLocate(AppState& state, SideEffectList& effects, NodeLayoutAccess& layout, bool scroll_to_match = false)
{
    state.search.search_state.BeginSearch();
    state.search.scroll_after_search = scroll_to_match;
    // クエリや前のマッチの消去を描画に出すため、始めの区切りは必ず描き直す。
    ContinueSearch(state, effects, layout, true);
}

void CloseSearchBar(AppState& state, SideEffectList& effects)
{
    state.search.search_state.Hide();
    state.search.ime_composition.clear();
    PushEffect(effects, effect::SearchUnfocus{});
    PushEffect(effects, effect::InvalidateWindow{});
}

// 開いていれば閉じる。
void ToggleSearchBar(AppState& state, SideEffectList& effects, NodeLayoutAccess& layout)
{
    auto& sg = state.search;
    if (sg.search_state.IsVisible()) {
        CloseSearchBar(state, effects);
        return;
    }
    sg.search_state.Show();
    ResetSearchCaret(sg);
    if (!sg.search_state.GetQuery().empty()) {
        RunSearchAndLocate(state, effects, layout);
    }
    PushEffect(effects, effect::SearchFocus{});
    PushEffect(effects, effect::InvalidateWindow{});
}

void StepSearchMatch(AppState& state, SideEffectList& effects, NodeLayoutAccess& layout, bool forward)
{
    auto& ss = state.search.search_state;
    const bool wrapped = forward ? ss.NextMatch() : ss.PrevMatch();
    if (wrapped && ss.GetMatchCount() > 1) {
        PushEffect(effects, effect::Beep{});
    }
    ScrollToCurrentMatch(state, layout);
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceSearchStep(AppState& state, SideEffectList& effects, NodeLayoutAccess& layout, bool forward)
{
    if (!state.search.search_state.IsVisible()) {
        ToggleSearchBar(state, effects, layout);
        return;
    }
    StepSearchMatch(state, effects, layout, forward);
}

void ReduceSearchTextChanged(AppState& state, SideEffectList& effects, NodeLayoutAccess& layout, std::wstring_view text)
{
    auto& sg = state.search;
    // 文書の切り替えで入力欄を空にしたときなど、中身が変わらない通知は検索も再描画もしない。
    if (text == sg.query_wide) {
        return;
    }
    // 検索バー入力は IME 経由で wstring。表示とヒットテストにはこのまま使い、
    // SearchState は Document テキスト (UTF-8) と比較するため変換して渡す。
    sg.query_wide.assign(text);
    std::pmr::string text_utf8;
    string_convert::WideToUtf8(text, text_utf8);
    sg.search_state.SetQuery(text_utf8);
    if (text.empty()) {
        sg.search_state.BeginSearch();
        PushEffect(effects, effect::InvalidateWindow{});
        return;
    }
    // 検索は時間で区切って進むので、大きな文書でも入力を待たせずにすぐ始める。
    RunSearchAndLocate(state, effects, layout, true);
}

void ReduceToggleCaseSensitive(AppState& state, SideEffectList& effects, NodeLayoutAccess& layout)
{
    auto& ss = state.search.search_state;
    ss.ToggleCaseSensitive();
    if (!ss.GetQuery().empty()) {
        RunSearchAndLocate(state, effects, layout);
    }
    PushEffect(effects, effect::InvalidateWindow{});
}

// 表示中は常に入力欄がフォーカスを持つので、表示中だけ描き直す。
void ReduceSearchSelection(AppState& state, SideEffectList& effects, const SearchSelectionAction& a)
{
    auto& sg = state.search;
    if (sg.caret_pos == a.sel_end && sg.selection_start == a.sel_start) {
        return;
    }
    sg.caret_pos = a.sel_end;
    sg.selection_start = a.sel_start;
    if (sg.search_state.IsVisible()) {
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ReduceImeComposition(AppState& state, SideEffectList& effects, std::wstring_view comp)
{
    auto& sg = state.search;
    if (sg.ime_composition == comp) {
        return;
    }
    sg.ime_composition = comp;
    if (sg.search_state.IsVisible()) {
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ReduceTogglePane(AppState& state, SideEffectList& effects, const TogglePaneAction& a)
{
    state.view.panes.ToggleSidePane(a.target);
    // 次に表示したとき、隠す前のホバー強調が残らないようにする。
    const auto k = std::to_underlying(a.target);
    state.view.hover.side_index[k] = -1;
    state.view.hover.side_button[k] = PaneHeaderButton::None;
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
    const auto wb = FindNodeWordBoundaries(nodes, static_cast<size_t>(a.node_index), a.text_pos);
    if (!wb.found) {
        return;
    }
    state.view.viewport.SetSelection(TextSelection::MakeOrdered(static_cast<int>(wb.start_node), wb.start, static_cast<int>(wb.end_node), wb.end));
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceClearSelection(AppState& state, SideEffectList& effects)
{
    if (state.search.search_state.IsVisible()) {
        CloseSearchBar(state, effects);
        return;
    }
    auto& viewport = state.view.viewport;
    const bool dragging = state.view.drag.kind == LeftDrag::TextSelect;
    const bool visible = viewport.GetSelection().active() || dragging;
    // テキスト選択のドラッグ中なら LButtonUp を待たずに終える。
    if (dragging) {
        EndLeftDrag(state, effects);
    }
    viewport.ClearSelection();
    if (visible) {
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

void ReduceCopyClipboard(const AppState& state, SideEffectList& effects)
{
    const auto& sel = state.view.viewport.GetSelection();
    if (!sel.active()) {
        return;
    }
    PushEffect(effects, effect::ClipboardWrite{ ExtractSelectedText(state.document.doc.GetNodes(), sel) });
}

void ReduceCopyFormattedClipboard(const AppState& state, SideEffectList& effects)
{
    const auto& sel = state.view.viewport.GetSelection();
    if (!sel.active()) {
        return;
    }
    const auto& nodes = state.document.doc.GetNodes();
    PushEffect(effects, effect::ClipboardWriteHtml{ ExtractSelectedTextAsHtml(nodes, sel, state.theme->dark), ExtractSelectedText(nodes, sel) });
}

// 戻り値: 段が動いたか。
bool StepZoomBy(ZoomLevel& zoom, const ZoomAction& a)
{
    int stepped = 0;
    while (stepped < a.steps && zoom.Step(a.direction)) {
        ++stepped;
    }
    return stepped > 0;
}

void ReduceSidePaneZoom(AppState& state, SideEffectList& effects, PaneTarget pane, const ZoomAction& a)
{
    auto& zoom = state.view.panes.SidePaneZoom(pane);
    const float old_zoom = zoom.Value();
    if (!StepZoomBy(zoom, a)) {
        return;
    }
    // 先頭に見えていた項目が先頭に残るよう、スクロール位置も倍率の比で動かす。
    auto& scroll = state.view.panes.SidePaneScroll(pane);
    scroll.scroll_y *= zoom.Value() / old_zoom;
    if (state.pane_layout_cache.IsValid()) {
        scroll.scroll_y = std::clamp(scroll.scroll_y, 0.0f, GetSidePaneContext(state, pane).info.max_scroll);
    }
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceZoom(AppState& state, SideEffectList& effects, const ZoomAction& a)
{
    if (a.pane) {
        ReduceSidePaneZoom(state, effects, *a.pane, a);
        return;
    }
    // 全レイアウトを捨てて組み直すのは段数によらず 1 回で済ませる。
    if (!StepZoomBy(state.view.viewport.Zoom(), a)) {
        return;
    }
    const auto anchor = SnapshotVisibleTarget(state);
    const float new_zoom = state.view.viewport.Zoom().Value();
    const float zoom_ratio = new_zoom / state.theme->zoom;
    // サイドペインは別に倍率を持つので、幅・スクロール位置もペイン配置もそのまま。
    state.document.layout_cache.InvalidateAllLayouts(state.document.doc.GetNodes());
    if (anchor.IsValid()) {
        // offset もズーム比でスケールしないと、ノード内の同じ位置が可視先頭に残らない。
        state.view.viewport.SetScrollTarget(anchor.node, anchor.offset * zoom_ratio);
    }
    PushEffect(effects, effect::ApplyZoom{});
}

// ポインタの下の領域だけをズームする。
void ReduceWheelZoom(AppState& state, SideEffectList& effects, const MouseWheelAction& a)
{
    auto& accum = state.interaction.zoom_wheel_accum;
    // 向きが反転したら逆向きの端数は持ち越さない。
    if ((accum < 0) != (a.delta < 0)) {
        accum = 0;
    }
    accum += a.delta;
    const int steps = std::abs(accum) / WHEEL_DELTA;
    if (steps == 0) {
        return;
    }
    accum = 0;
    ReduceZoom(state, effects, ZoomAction{ a.delta > 0 ? ZoomDirection::In : ZoomDirection::Out, steps, ToPaneTarget(a.zone) });
}

void ReduceToggleDarkMode(AppState& state, SideEffectList& effects)
{
    const auto anchor = SnapshotVisibleTarget(state);
    // 色のみの変更なのでテキストレイアウトは維持する。描画エフェクトの固定ブラシは SetColor で
    // 色だけ差し替わるので再適用不要で、テーマ色を焼き込んだ Mermaid bitmap のみ破棄する。
    state.document.layout_cache.InvalidateDiagramBitmaps(state.document.doc.GetDiagramNodeIndices());
    if (anchor.IsValid()) {
        // Mermaid 再レンダリングで微小な高さ変化が起きるので target で追従する。
        state.view.viewport.SetScrollTarget(anchor.node, anchor.offset);
    }
    PushEffect(effects, effect::ToggleDarkMode{});
}

void ReduceActivate(AppState& state, SideEffectList& effects, const ActivateAction& a)
{
    if (state.window.window_active != a.active) {
        state.window.window_active = a.active;
        PushEffect(effects, effect::InvalidateWindow{});
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
    const bool dpi_changed = std::exchange(state.window.dpi_changed, false);
    if (a.width == state.window.client_width && a.height == state.window.client_height && !dpi_changed) {
        return;
    }
    state.window.client_width = a.width;
    state.window.client_height = a.height;
    state.window.size_changed_in_sizing = true;
    state.pane_layout_cache.Invalidate();
    PushEffect(effects, effect::RendererResize{ a.width, a.height, state.window.is_sizing });
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
        // ライブリサイズ中に大きめに取った描画面を、実際の大きさに合わせ直す。
        PushEffect(effects, effect::RendererResize{ state.window.client_width, state.window.client_height });
        PushEffect(effects, effect::PerformResizeEnd{});
    }
}

void ReduceDpiChanged(AppState& state, SideEffectList& effects, const DpiChangedAction& a)
{
    state.window.cached_dpi_scale = DpiScaleFrom(static_cast<float>(a.dpi));
    // ピクセルサイズ不変でも DIP サイズが変わるため、移動ループ終了時のレイアウトを要求する。
    // 続く WM_SIZE が同じピクセルサイズでも処理させる。
    state.window.size_changed_in_sizing = true;
    state.window.dpi_changed = true;
    state.pane_layout_cache.Invalidate();
    PushEffect(effects, effect::RendererSetDpi{ static_cast<float>(a.dpi) });
    PushEffect(effects, effect::SetWindowPosition{ a.suggested });
}

void ReduceSwipeOverlayTimeout(AppState& state, SideEffectList& effects)
{
    const auto result = state.interaction.swipe_detector.Commit();
    PushEffect(effects, effect::KillTimer{ app_timer::Id::SWIPE_OVERLAY });
    if (result != NavDirection::None) {
        NavigateHistory(state, effects, result == NavDirection::Forward);
        PushEffect(effects, effect::InvalidateWindow{});
    }
}

// ノード/見出し index に結び付く一時状態 (選択・横スクロール・ホバー・目次の現在位置・ツールチップの対象・
// 文書を対象にするドラッグ・ドラッグの後に回したリロード) を消す。文書を差し替えると別の要素を指すので、
// ロードとリロードの両方で消す。ホバーは描画後に引き直される (App::RefreshHoverAtCursor)。
void ResetPerNodeTransientState(AppState& state, SideEffectList& effects)
{
    auto& view = state.view;
    view.viewport.ClearSelection();
    view.block_scroll_x.clear();
    view.hover = {};
    view.active_toc_index = -1;
    // ツールチップの同一判定は node / TOC の index で行う。
    ClearTooltip(state, effects);
    // 待たせていたリロードは旧文書のもの。先に外し、ドラッグの終わりで読み直させない。
    view.reload_after_drag = false;
    if (IsDocumentBoundDrag(view.drag.kind)) {
        EndLeftDrag(state, effects);
    }
}

void ReduceDocumentOpened(AppState& state, SideEffectList& effects, const DocumentOpenedAction& a)
{
    ResetPerNodeTransientState(state, effects);
    // 同一文書の再パースと違い、サイドペインのスクロールも先頭へ戻す。
    state.view.panes.ResetScrollStates();
    // 旧文書のマッチ位置が新 nodes に対して誤用されるのを防ぐ。入力欄を空にしたときの EN_CHANGE は、
    // ここでクエリを先に空にしておくので何もせず終わる。
    auto& sg = state.search;
    sg.search_state.Reset();
    sg.query_wide.clear();
    sg.ime_composition.clear();
    ResetSearchCaret(sg);
    PushEffect(effects, effect::SearchUnfocus{ /*clear_text=*/true });

    const auto& doc = state.document.doc;
    if (a.follow_file_pane && !doc.GetDirectory().empty()) {
        state.file_explorer.SetDirectory(doc.GetDirectory());
    }
    state.file_explorer.SetCurrentFile(doc.GetFilePath());
    RestoreScrollAfterLoad(state);
}

void ReduceDocumentReloaded(AppState& state, SideEffectList& effects, const DocumentReloadedAction& a)
{
    // ReloadSettledAction の検索し直しが終わるまで、変わらなかったノードのハイライトと件数を残す。
    state.search.search_state.KeepMatchesOfUnchangedNodes(a.unchanged);
    ResetPerNodeTransientState(state, effects);
    // 直前の移動先 (ノード基準) は旧文書のもの。捨ててピクセル位置で固定する。
    state.view.viewport.SetScrollY(a.scroll_y);
}

void ReduceReloadSettled(AppState& state, SideEffectList& effects, NodeLayoutAccess& layout)
{
    const auto& ss = state.search.search_state;
    if (ss.IsVisible() && !ss.GetQuery().empty()) {
        RunSearchAndLocate(state, effects, layout);
    }
}

} // namespace

SearchBarRenderState BuildSearchBarRenderState(const AppState& state)
{
    const auto& sg = state.search;
    const auto& ss = sg.search_state;
    return {
        .query = sg.query_wide,
        .ime_composition = sg.ime_composition,
        .current_match = ss.GetCurrentMatchIndex(),
        .total_matches = ss.HasResult() ? ss.GetMatchCount() : -1,
        .caret_pos = sg.caret_pos,
        .selection_start = sg.selection_start,
        .visible = ss.IsVisible(),
        .caret_visible = state.window.window_active,
        .case_sensitive = ss.IsCaseSensitive(),
        .highlight_enabled = ss.IsHighlightEnabled(),
        .hovered = state.view.hover.search_bar,
    };
}

SideEffectList Reduce(AppState& state, const AppAction& action, NodeLayoutAccess& layout, std::pmr::memory_resource* resource)
{
    SideEffectList effects(resource);
    EnsurePaneLayout(state);
    // viewport を動かす経路 (キー・ホイール・スクロールバー・履歴・検索のマッチ移動など) を問わず、
    // 前後の scroll_y を比べてスクロールの副作用を 1 回だけ積む。
    // 文書を差し替えた後 (読み込みの復元・リロード) では積まない。直後の App::RelayoutViewport が計測してから描き直させる。
    bool emit_scroll_effects = true;
    const float old_scroll = state.view.viewport.GetScrollY();

    // clang-format off
    std::visit(overloaded{
        [](const NoOpAction&) static {},

        // ---- スクロール ----
        [&](const KeyScrollAction& a) { ReduceKeyScroll(state, a); },
        [&](const MouseWheelAction& a) {
            if (a.ctrl) {
                ReduceWheelZoom(state, effects, a);
            }
            else {
                ReduceWheelScroll(state, effects, a);
            }
        },

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

        // ---- 検索 ----
        [&](const OpenSearchBarAction&) { ToggleSearchBar(state, effects, layout); },
        [&](const CloseSearchBarAction&) { CloseSearchBar(state, effects); },
        [&](const SearchNextAction&) { ReduceSearchStep(state, effects, layout, true); },
        [&](const SearchPrevAction&) { ReduceSearchStep(state, effects, layout, false); },
        [&](const SearchTextChangedAction& a) { ReduceSearchTextChanged(state, effects, layout, a.text); },
        [&](const SearchContinueAction&) {
            if (state.search.search_state.IsSearching()) {
                ContinueSearch(state, effects, layout);
            }
        },
        [&](const ToggleCaseSensitiveAction&) { ReduceToggleCaseSensitive(state, effects, layout); },
        [&](const ToggleHighlightAction&) {
            state.search.search_state.ToggleHighlightEnabled();
            PushEffect(effects, effect::InvalidateWindow{});
        },
        [&](const SearchSelectionAction& a) { ReduceSearchSelection(state, effects, a); },
        [&](const ImeCompositionAction& a) { ReduceImeComposition(state, effects, a.text); },

        // ---- マウス関連 ----
        [&](const MouseLeaveAction&) { ReduceMouseLeave(state, effects); },
        [&](const CaptureChangedAction&) { ReduceCaptureChanged(state, effects); },
        [&](const PointerHoverAction& a) { ReducePointerHover(state, effects, a.hover); },
        [&](const SplitterDragStartedAction& a) { ReduceSplitterDragStarted(state, effects, a); },
        [&](const SplitterDragMovedAction& a) { ReduceSplitterDragMoved(state, effects, a); },
        [&](const LeftDragEndedAction&) { ReduceLeftDragEnded(state, effects); },
        [&](const SearchInputDragStartedAction& a) { ReduceSearchInputDragStarted(state, effects, a); },
        [&](const SearchInputDragMovedAction& a) { ReduceSearchInputDragMoved(state, effects, a); },
        [&](const MdScrollbarDragStartedAction& a) { ReduceMdScrollbarDragStarted(state, effects, a); },
        [&](const MdScrollbarDragMovedAction& a) { ReduceMdScrollbarDragMoved(state, a); },
        [&](const PaneScrollbarDragStartedAction& a) { ReducePaneScrollbarDragStarted(state, effects, a); },
        [&](const PaneScrollbarDragMovedAction& a) { ReducePaneScrollbarDragMoved(state, effects, a); },
        [&](const TextSelectionStartedAction& a) { ReduceTextSelectionStarted(state, effects, a); },
        [&](const TextSelectionMovedAction& a) { ReduceTextSelectionMoved(state, effects, a); },
        [&](const TextSelectionEndedAction& a) { ReduceTextSelectionEnded(state, effects, a); },
        [&](const RightClickGestureStartedAction& a) { ReduceRightClickGestureStarted(state, effects, a); },
        [&](const RightClickGestureMovedAction& a) { ReduceRightClickGestureMoved(state, effects, a); },
        [&](const RightClickGestureCompletedAction& a) { ReduceRightClickGestureCompleted(state, effects, a); },
        [&](const FilePaneDirectoryClickedAction& a) { ReduceFilePaneDirectoryClicked(state, effects, a); },
        [&](const FilePaneRevealCurrentFileAction&) { ReduceFilePaneRevealCurrentFile(state, effects); },
        [&](const TocItemClickedAction& a) { ReduceTocItemClicked(state, a); },
        [&](const NavigateAnchorAction& a) { ReduceNavigateAnchor(state, a); },
        [&](const DocumentOpenedAction& a) {
            ReduceDocumentOpened(state, effects, a);
            emit_scroll_effects = false;
        },
        [&](const DocumentReloadedAction& a) {
            ReduceDocumentReloaded(state, effects, a);
            emit_scroll_effects = false;
        },
        [&](const ReloadSettledAction&) { ReduceReloadSettled(state, effects, layout); },
        [&](const LoadFailedAction&) { DiscardPendingLoad(state); },

        // ---- ナビゲーション ----
        [&](const NavigateBackAction&) { NavigateHistory(state, effects, /*forward=*/false); },
        [&](const NavigateForwardAction&) { NavigateHistory(state, effects, /*forward=*/true); },

        // ---- ファイル操作 ----
        [&](const OpenPathAction& a) { OpenFileWithHistory(state, effects, a.path); },
        [&](const ShowHelpAction&) { ReduceShowHelp(state, effects); },
        [&](const OpenFileAction&) { PushEffect(effects, effect::OpenFileDialog{}); },
        [&](const ReloadFileAction&) { PushEffect(effects, effect::ReloadFile{}); },

        // ---- 非同期・タイマー ----
        [&](const SwipeOverlayTimeoutAction&) { ReduceSwipeOverlayTimeout(state, effects); },
    }, action);
    // clang-format on
    if (emit_scroll_effects && state.view.viewport.GetScrollY() != old_scroll) {
        EmitScrollChangedSideEffects(state, effects);
    }
    return effects;
}
