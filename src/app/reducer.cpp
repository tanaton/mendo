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
#include <iterator>
#include <utility>

namespace {

// キーボードスクロール量（DIP）
constexpr float SCROLL_LINE_AMOUNT = 40.0f;
constexpr float SCROLL_PAGE_FACTOR = 0.9f;
// 横ホイール 1 ノッチあたりの DIP スクロール量。
// Shift+Wheel / WM_MOUSEHWHEEL の delta (WHEEL_DELTA = 120 単位) を DIP に換算する。
constexpr float HSCROLL_DIP_PER_NOTCH = 60.0f;

// ロード中の別ファイルが取り消されたか失敗したときに、その仮の移動と移動先用のスクロール先を捨てる。
// スクロール先を残すと、次に開く別ファイルが無関係な位置へスクロールする。
void DiscardPendingLoad(AppState& state)
{
    state.view.nav_history.RollbackPending();
    state.view.pending_scroll = {};
}

// 開き終えたら DocumentOpenedAction が来るか。同じファイルの開き直しはリロードとして終わり、確定が来ない。
bool LoadsAnotherDocument(const AppState& state, std::wstring_view path)
{
    return !path.empty() && !path_util::iequal(path, state.document.doc.GetFilePath());
}

// load_path: これから開くファイル。現在と別のファイルなら、ロード失敗 (LoadFailedAction) で取り消す仮の履歴にする。
void PushCurrentNavEntry(AppState& state, std::wstring_view load_path = {})
{
    // 新しいロードはロード中の別ファイルを取り消す。文書が無い起動時の読み込み中も、起動時の復元位置を持ち越さない。
    if (!load_path.empty()) {
        DiscardPendingLoad(state);
    }
    // doc 未ロード (パス空) の状態は戻り先にならないため積まない
    if (state.document.doc.GetFilePath().empty()) {
        return;
    }
    NavEntry entry = CurrentNavEntry(state);
    if (!LoadsAnotherDocument(state, load_path)) {
        state.view.nav_history.Push(std::move(entry));
    }
    else {
        state.view.nav_history.PushPending(std::move(entry));
    }
}

struct SidePaneContext {
    PaneScrollInfo info;
    float item_height;
    float& scroll_y;
};

SidePaneContext GetSidePaneContext(AppState& state, PaneTarget pane)
{
    return {
        SidePaneScrollInfo(state, pane, ComputePaneLayout(state)),
        SidePaneItemHeight(state, pane),
        state.view.panes.SidePaneScroll(pane),
    };
}

} // namespace

PaneLayout ComputePaneLayout(const AppState& state) noexcept
{
    // WM_NCHITTEST は Init 前にも届く。大きさが決まる (最初の WM_SIZE) までは空の配置。
    if (state.window.client_width == 0) {
        return {};
    }
    return state.view.panes.ComputeLayout(state.window.WidthDip(), state.window.HeightDip(), state.theme->splitter_width, TitleBar::BASE_HEIGHT);
}

void SyncActiveTocIndex(AppState& state)
{
    const bool follow = !std::exchange(state.view.keep_toc_pane_once, false);
    if (!state.view.panes.IsSidePaneVisible(PaneTarget::Toc)) {
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
        ctx.scroll_y = CenterPaneScrollOnItem(static_cast<size_t>(new_active), ctx.item_height, ctx.info);
    }
}

BlockHScrollGeometry ResolveBlockHScrollGeometry(const AppState& state, int node_index, float md_width) noexcept
{
    const auto& nodes = state.document.doc.GetNodes();
    const auto& cache = state.document.layout_cache;
    if (node_index < 0 || node_index >= static_cast<int>(cache.size())) {
        return {};
    }
    // 大きさが決まる前は配置が空で可視幅が 0 になり、can_scroll() が false を返す。
    return GetBlockHScrollGeometry(nodes, cache, static_cast<size_t>(node_index), *state.theme, md_width);
}

namespace {

// Reduce の呼び出し 1 回の文脈。アクションごとの処理 On はどれもこれを受ける。
struct ReduceContext {
    AppState& state;
    SideEffectList& effects;
    NodeLayoutAccess& layout;
    // 文書を差し替えた後 (読み込みの復元・リロード) ではスクロールの副作用を積まない。
    // 直後の App::RelayoutViewport が計測してから描き直させる。
    bool emit_scroll_effects = true;
    // 描き直しは Reduce の最後に 1 件だけ積む。
    bool invalidated = false;
};

void Invalidate(ReduceContext& c)
{
    c.invalidated = true;
}

// 対象も忘れるので、次のホバーで同じ対象に戻っても表示し直す。対象が無ければ隠れているので何もしない。
void ClearTooltip(ReduceContext& c)
{
    if (std::exchange(c.state.interaction.tooltip, {}).IsEmpty()) {
        return;
    }
    PushEffect(c.effects, effect::SetTooltip{});
}

// スクロール位置が変わった時の副作用列。Reduce が前後の scroll_y を比べて 1 回だけ積む。
// 目次のアクティブ見出しは描画時に SyncActiveTocIndex が決め直す。
void EmitScrollChangedSideEffects(ReduceContext& c)
{
    ClearTooltip(c);
    Invalidate(c);
    // ライブリサイズ中に上限へ収めても図や画像を取りに行かない。幅が決まるのはサイズ変更の終わり。
    if (!c.state.window.IsSizing()) {
        PushEffect(c.effects, effect::BitmapManage{});
    }
}

void ScrollToNode(AppState& state, int node, float offset)
{
    auto& viewport = state.view.viewport;
    // 末尾セクションへのジャンプで scroll_y > max_scroll になると、後続の DirectScrollBy で
    // 位置が一気に飛ぶ。target を残さず範囲内で確定し、範囲外への復帰を抑える。
    viewport.ScrollTo(node >= 0 ? NodeOffsetToScrollY(state.document.layout_cache, node, offset) : viewport.GetScrollY(), MdMaxScroll(state));
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

void On(ReduceContext& c, const KeyScrollAction& a)
{
    auto& viewport = c.state.view.viewport;
    const auto info = MdScrollInfo(c.state, ComputePaneLayout(c.state));
    const float page_size = info.content_height;
    const float max_scroll = info.max_scroll;
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
void NotifyVWheel(ReduceContext& c, uint64_t tick)
{
    auto& swipe = c.state.interaction.swipe_detector;
    const bool had_overlay = swipe.IsOverlayVisible();
    swipe.NotifyVScroll(tick);
    if (had_overlay) {
        PushEffect(c.effects, effect::KillTimer{ app_timer::Id::SWIPE_OVERLAY });
        Invalidate(c);
    }
}

void ReduceWheelScroll(ReduceContext& c, const MouseWheelAction& a)
{
    NotifyVWheel(c, a.tick);
    const float amount = -static_cast<float>(a.delta) * MOUSE_WHEEL_SCROLL_MULTIPLIER;
    const auto pane = ToPaneTarget(a.zone);
    if (!pane) {
        c.state.view.viewport.DirectScrollBy(amount, MdMaxScroll(c.state));
        return;
    }
    const auto ctx = GetSidePaneContext(c.state, *pane);
    if (c.state.view.panes.ScrollSidePaneBy(*pane, amount, ctx.info.max_scroll)) {
        Invalidate(c);
    }
}

void On(ReduceContext& c, const HWheelAction& a)
{
    // ホバー or ドラッグ中のブロックが横スクロール可能ならそこに適用し、スワイプオーバーレイには流さない。
    const int drag_node = c.state.view.HDragNode();
    const int target = (drag_node >= 0) ? drag_node : c.state.view.hover.h_block;
    if (target >= 0) {
        const auto geom = ResolveBlockHScrollGeometry(c.state, target);
        if (!geom.can_scroll()) {
            return;
        }
        const float dx = static_cast<float>(a.delta) / WHEEL_DELTA * HSCROLL_DIP_PER_NOTCH;
        if (SetBlockScrollX(c.state, c.state.view.GetBlockScrollX(geom) + dx, geom)) {
            Invalidate(c);
        }
        return;
    }

    auto& swipe = c.state.interaction.swipe_detector;
    const auto old_direction = swipe.GetOverlayDirection();
    swipe.OnHWheel(a.delta, a.tick);
    PushEffect(c.effects, effect::SetTimer{ app_timer::Id::SWIPE_OVERLAY, static_cast<UINT>(SwipeDetector::COMMIT_TIMEOUT_MS) });
    if (old_direction != swipe.GetOverlayDirection()) {
        Invalidate(c);
    }
}

void On(ReduceContext& c, const BlockHScrollDragStartedAction& a)
{
    const auto geom = ResolveBlockHScrollGeometry(c.state, a.node_index);
    if (!geom.can_scroll()) {
        return;
    }
    c.state.view.drag = left_drag::BlockHScroll{ .node = a.node_index, .start_x = a.dip_x, .start_scroll = c.state.view.GetBlockScrollX(geom) };
    Invalidate(c);
}

void MoveBlockHScrollDrag(ReduceContext& c, const left_drag::BlockHScroll& drag, float dip_x)
{
    const auto geom = ResolveBlockHScrollGeometry(c.state, drag.node);
    if (!geom.can_scroll()) {
        return;
    }
    // dip_delta はドラッグの sum = サムが動く DIP。サムが (track - thumb_w) 動いたとき
    // コンテンツが scroll_max 動くべきなので、scroll_delta = dip_delta * scroll_max / drag_range。
    // thumb_w が PANE_SCROLLBAR_THUMB_MIN に張り付いた長大コンテンツでも 1:1 で対応する。
    const float dip_delta = dip_x - drag.start_x;
    const float thumb_w = BlockHScrollbarThumbWidth(geom.visible_width, geom.block_width);
    const float drag_range = std::max(1.0f, geom.visible_width - thumb_w);
    const float scroll_max = geom.scroll_max();
    if (SetBlockScrollX(c.state, drag.start_scroll + dip_delta * scroll_max / drag_range, geom)) {
        Invalidate(c);
    }
}

void ApplyNavResult(ReduceContext& c, NavEntry&& entry)
{
    if (LoadsAnotherDocument(c.state, entry.file_path)) {
        c.state.view.pending_scroll = ScrollTarget{ entry.node, entry.offset };
        PushEffect(c.effects, effect::LoadFile{ std::move(entry.file_path) });
    }
    else {
        // 戻る/進むを続けて表示中の文書へ戻ったとき、途中で開き始めたファイルが後から表示されないようにする。
        PushEffect(c.effects, effect::CancelOtherFileLoad{});
        // 取り消すファイル用のスクロール先なので、次に開くファイルへ持ち越さない。
        c.state.view.pending_scroll = {};
        // ロードを伴わないので、ここで確定しないと後の無関係なロード失敗で取り消される。
        c.state.view.nav_history.CommitPending();
        ScrollToNode(c.state, entry.node, entry.offset);
    }
}

void On(ReduceContext& c, const NavigateHistoryAction& a)
{
    auto& history = c.state.view.nav_history;
    NavEntry out;
    const bool moved = a.forward ? history.GoForward(CurrentNavEntry(c.state), out) : history.GoBack(CurrentNavEntry(c.state), out);
    if (moved) {
        ApplyNavResult(c, std::move(out));
    }
}

// 現在位置を履歴に積んでから別ファイルを開く。
void On(ReduceContext& c, const OpenPathAction& a)
{
    PushCurrentNavEntry(c.state, a.path);
    // リロードでは開き終えた文書の扱い (RestoreScrollAfterLoad) を通らず、見出しが次に開く文書まで残る。
    if (!a.anchor.empty() && LoadsAnotherDocument(c.state, a.path)) {
        c.state.view.pending_scroll = a.anchor;
    }
    PushEffect(c.effects, effect::LoadFile{ a.path });
}

// 見出しを上端に貼り付けず、上の余白ごと見せる。
ScrollTarget HeadingTarget(const AppState& state, int node_index)
{
    return { node_index, -state.theme->heading_spacing_above };
}

void ScrollToHeading(AppState& state, int node_index)
{
    const ScrollTarget target = HeadingTarget(state, node_index);
    ScrollToNode(state, target.node, target.offset);
}

void On(ReduceContext& c, const FilePaneDirectoryClickedAction& a)
{
    c.state.file_explorer.SetDirectory(a.full_path);
    c.state.view.panes.SidePaneScroll(PaneTarget::File) = 0.0f;
    Invalidate(c);
}

void On(ReduceContext& c, const FilePaneRevealCurrentFileAction&)
{
    // ヘルプ表示中は移動先のフォルダがない。
    if (!c.state.document.doc.HasBackingFile()) {
        return;
    }
    c.state.file_explorer.SetDirectory(c.state.document.doc.GetDirectory());
    c.state.file_explorer.EnsureListed();

    const auto& entries = c.state.file_explorer.GetEntries();
    if (c.state.file_explorer.IsListing()) {
        c.state.file_explorer.DeferReveal();
        return;
    }
    const auto it = std::ranges::find_if(entries, &FileEntry::is_current);
    const auto ctx = GetSidePaneContext(c.state, PaneTarget::File);
    ctx.scroll_y = it != entries.end()
                       ? CenterPaneScrollOnItem(static_cast<size_t>(std::distance(entries.begin(), it)), ctx.item_height, ctx.info)
                       : 0.0f;
    Invalidate(c);
}

void On(ReduceContext& c, const TocItemClickedAction& a)
{
    PushCurrentNavEntry(c.state);
    ScrollToHeading(c.state, a.node_index);
    c.state.view.keep_toc_pane_once = true;
}

void On(ReduceContext& c, const NavigateAnchorAction& a)
{
    // リンク切れのアンカーで履歴を積むと、進む履歴が消えて戻っても同じ位置になる。
    const int node_index = c.state.document.doc.FindAnchorIndex(a.anchor_id);
    if (node_index < 0) {
        return;
    }
    PushCurrentNavEntry(c.state);
    ScrollToHeading(c.state, node_index);
}

ScrollTarget TakePendingScroll(AppState& state)
{
    const auto pending = std::exchange(state.view.pending_scroll, {});
    if (const auto* anchor = std::get_if<std::string>(&pending)) {
        const int node = state.document.doc.FindAnchorIndex(*anchor);
        // 見出しが無ければ、リンク先の文書の先頭を開く。
        return node >= 0 ? HeadingTarget(state, node) : ScrollTarget{};
    }
    const auto* target = std::get_if<ScrollTarget>(&pending);
    return target ? *target : ScrollTarget{};
}

void RestoreScrollAfterLoad(AppState& state)
{
    auto& viewport = state.view.viewport;
    if (const ScrollTarget restore = TakePendingScroll(state); restore.IsValid()) {
        viewport.SetScrollTarget(restore.node, restore.offset);
        viewport.ApplyScrollTarget(state.document.layout_cache);
    }
    else {
        viewport.SetScrollY(0.0f);
    }
}

void On(ReduceContext& c, const ShowHelpAction&)
{
    // ヘルプ表示中はヘルプに留まり、ロード中の別ファイルだけを取り消す。
    if (IsHelpPath(c.state.document.doc.GetFilePath())) {
        DiscardPendingLoad(c.state);
        PushEffect(c.effects, effect::CancelOtherFileLoad{});
        return;
    }
    On(c, OpenPathAction{ std::wstring(HELP_PATH) });
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

// ツールチップ以外のホバー状態を反映し、見た目が変わったら再描画させる。
void ApplyHoverHighlights(ReduceContext& c, HoverHighlights h)
{
    // 入力欄はテキスト編集領域でホバー強調は不要。
    if (h.search_bar == SearchBarHitZone::Input) {
        h.search_bar = SearchBarHitZone::None;
    }
    // 横スクロールのドラッグ中は、ポインタがどこへ出てもバーを出したままにする。
    if (c.state.view.HDragNode() >= 0) {
        h.h_block = c.state.view.hover.h_block;
    }
    if (std::exchange(c.state.view.hover, h) != h) {
        Invalidate(c);
    }
}

void On(ReduceContext& c, const PointerHoverAction& a)
{
    ApplyHoverHighlights(c, a);
    if (a.tooltip.target != c.state.interaction.tooltip) {
        c.state.interaction.tooltip = a.tooltip.target;
        PushEffect(c.effects, effect::SetTooltip{ a.tooltip.text });
    }
}

// ペインを経由せずに離れた経路 (ウィンドウ外・キャプション上) で呼ばないとホバーハイライトが残留する。
void On(ReduceContext& c, const MouseLeaveAction&)
{
    ApplyHoverHighlights(c, {});
    ClearTooltip(c);
}

// スピナーの間は本文ペインを描かないので、出す前に始まった本文ペインのドラッグとホバーをここで打ち切る。
// 続けると、離したときに見えない旧文書のリンクを開き、旧文書の選択を伸ばしてしまう。
void On(ReduceContext& c, const LoadingShownAction& a)
{
    auto& view = c.state.view;
    if (std::exchange(view.loading_shown, a.shown) == a.shown) {
        return;
    }
    Invalidate(c);
    if (!a.shown) {
        return;
    }
    if (IsMdPaneDrag(view.drag)) {
        view.drag = {};
    }
    // 見えたままのタイトルバーとサイドペインのホバーは残す。
    ApplyHoverHighlights(c, { .titlebar = view.hover.titlebar, .side_index = view.hover.side_index, .side_button = view.hover.side_button });
    ClearTooltip(c);
}

// キャプチャ喪失時 (Alt+Tab・他アプリの SetCapture 等) は WM_LBUTTONUP が
// 届かないため、進行中のジェスチャーとドラッグをここで終える。
void On(ReduceContext& c, const CaptureChangedAction&)
{
    if (!CaptureRequired(c.state)) {
        return;
    }
    c.state.interaction.gesture.Reset();
    c.state.view.drag = {};
    Invalidate(c);
}

void On(ReduceContext& c, const SplitterDragStartedAction& a)
{
    c.state.view.drag = left_drag::Splitter{ a.edge };
}

void MoveSplitterDrag(ReduceContext& c, PaneTarget pane, float dip_x)
{
    const float splitter_w = c.state.theme->splitter_width;
    const float before_file = c.state.view.panes.GetSidePaneWidth(PaneTarget::File);
    const float before_toc = c.state.view.panes.GetSidePaneWidth(PaneTarget::Toc);
    c.state.view.panes.DragSplitterTo(pane, dip_x, c.state.window.WidthDip(), splitter_w);
    if (c.state.view.panes.GetSidePaneWidth(PaneTarget::File) == before_file && c.state.view.panes.GetSidePaneWidth(PaneTarget::Toc) == before_toc) {
        return;
    }
    Invalidate(c);
}

void MoveTextSelectionDrag(ReduceContext& c, const left_drag::TextSelect& drag, int node_index, uint32_t text_pos)
{
    if (node_index < 0) {
        return;
    }
    // WM_MOUSEMOVE は 16ms 周期で連発するため、選択が同値なら早期 return。
    const auto next = drag.SelectionTo(node_index, text_pos);
    if (next == c.state.view.viewport.GetSelection()) {
        return;
    }
    c.state.view.viewport.SetSelection(next);
    Invalidate(c);
}

void On(ReduceContext& c, const LeftDragEndedAction& a)
{
    const LeftDragState drag = std::exchange(c.state.view.drag, left_drag::None{});
    // 選択の無いクリックなど、見た目が変わらないなら再描画しない。
    if (const auto* select = std::get_if<left_drag::TextSelect>(&drag)) {
        MoveTextSelectionDrag(c, *select, a.node_index, a.text_pos);
    }
    else if (std::holds_alternative<left_drag::Splitter>(drag) || std::holds_alternative<left_drag::MdScrollbar>(drag)) {
        PushEffect(c.effects, effect::SettleViewport{});
    }
    else if (std::holds_alternative<left_drag::BlockHScroll>(drag)) {
        Invalidate(c);
    }
}

void On(ReduceContext& c, const SearchInputDragStartedAction& a)
{
    c.state.view.drag = left_drag::SearchInput{ a.caret_pos };
    PushEffect(c.effects, effect::SearchFocus{ a.caret_pos, a.caret_pos });
}

void MoveSearchInputDrag(ReduceContext& c, int anchor, int caret)
{
    const auto& sg = c.state.search;
    if (caret == sg.caret_pos && anchor == sg.selection_start) {
        return;
    }
    PushEffect(c.effects, effect::SearchFocus{ anchor, caret });
}

void On(ReduceContext& c, const MdScrollbarDragStartedAction& a)
{
    const auto info = MdScrollInfo(c.state, ComputePaneLayout(c.state));
    auto& viewport = c.state.view.viewport;
    const auto grip = ComputeScrollbarDragGrip(ComputeThumbY(info, viewport.GetScrollY()), info.thumb_height, a.dip_y);
    c.state.view.drag = left_drag::MdScrollbar{ grip.drag_offset };
    // thumb 内クリックなら 1st jump は不要 (thumb-grip オフセット記録だけ)。
    if (!grip.inside_thumb) {
        viewport.ScrollTo(ScrollFromThumbY(info, a.dip_y - grip.drag_offset), info.max_scroll);
    }
}

void MoveMdScrollbarDrag(AppState& state, float grip_offset, float dip_y)
{
    const float new_thumb_y = dip_y - grip_offset;
    const auto info = MdScrollInfo(state, ComputePaneLayout(state));
    state.view.viewport.ScrollTo(ScrollFromThumbY(info, new_thumb_y), info.max_scroll);
}

void On(ReduceContext& c, const PaneScrollbarDragStartedAction& a)
{
    const auto ctx = GetSidePaneContext(c.state, a.pane);
    if (!ShowsVScrollbar(ctx.info, false)) {
        return;
    }
    const auto grip = ComputeScrollbarDragGrip(ComputeThumbY(ctx.info, ctx.scroll_y), ctx.info.thumb_height, a.dip_y);
    c.state.view.drag = left_drag::PaneScrollbar{ a.pane, grip.drag_offset };
    if (!grip.inside_thumb) {
        ctx.scroll_y = ScrollFromThumbY(ctx.info, a.dip_y - grip.drag_offset);
        Invalidate(c);
    }
}

void MovePaneScrollbarDrag(ReduceContext& c, const left_drag::PaneScrollbar& drag, float dip_y)
{
    const auto ctx = GetSidePaneContext(c.state, drag.pane);
    const float new_thumb_y = dip_y - drag.grip_offset;
    ctx.scroll_y = ScrollFromThumbY(ctx.info, new_thumb_y);
    Invalidate(c);
}

void On(ReduceContext& c, const TextSelectionStartedAction& a)
{
    if (a.node_index < 0) {
        return;
    }
    auto& viewport = c.state.view.viewport;
    const bool had_selection = viewport.GetSelection().active();
    viewport.ClearSelection();
    c.state.view.drag = left_drag::TextSelect{ .node = a.node_index, .pos = a.text_pos, .click_x = a.click_x, .click_y = a.click_y };
    if (had_selection) {
        Invalidate(c);
    }
}

void On(ReduceContext& c, const LeftDragMovedAction& a)
{
    // 動かす処理が c.state を書き換えても参照が浮かないよう、写してから渡す。
    const LeftDragState drag = c.state.view.drag;
    std::visit(overloaded{
                   [](const left_drag::None&) static {},
                   [&](const left_drag::SearchInput& d) { MoveSearchInputDrag(c, d.anchor, a.search_caret); },
                   [&](const left_drag::Splitter& d) { MoveSplitterDrag(c, d.pane, a.dip_x); },
                   [&](const left_drag::PaneScrollbar& d) { MovePaneScrollbarDrag(c, d, a.dip_y); },
                   [&](const left_drag::MdScrollbar& d) { MoveMdScrollbarDrag(c.state, d.grip_offset, a.dip_y); },
                   [&](const left_drag::BlockHScroll& d) { MoveBlockHScrollDrag(c, d, a.dip_x); },
                   [&](const left_drag::TextSelect& d) { MoveTextSelectionDrag(c, d, a.node_index, a.text_pos); },
               },
               drag);
}

void On(ReduceContext& c, const RightClickGestureStartedAction& a)
{
    c.state.interaction.gesture.OnRButtonDown(a.dip_x, a.dip_y);
}

void On(ReduceContext& c, const RightClickGestureMovedAction& a)
{
    if (c.state.interaction.gesture.OnMouseMove(a.dip_x, a.dip_y)) {
        Invalidate(c);
    }
}

void On(ReduceContext& c, const RightClickGestureCompletedAction& a)
{
    auto& gesture = c.state.interaction.gesture;
    if (gesture.GetPhase() == GesturePhase::Idle) {
        return;
    }
    Invalidate(c);
    if (const NavDirection dir = gesture.OnRButtonUp(); dir != NavDirection::None) {
        On(c, NavigateHistoryAction{ .forward = dir == NavDirection::Forward });
        return;
    }
    // 方向が確定しなければ通常の右クリック扱いにし、メニューを失わない。
    PushEffect(c.effects, effect::ShowContextMenu{ a.screen_x, a.screen_y });
}

// ---- 検索バー ----

// 1 回でこれを超える分は、メッセージループが入力と描画を配った後の空き時間 (SearchContinueAction) へ回す。
constexpr auto kSearchSliceBudget = std::chrono::milliseconds(8);

constexpr void ResetSearchCaret(SearchGroup& sg) noexcept
{
    sg.caret_pos = -1;
    sg.selection_start = -1;
}

void ScrollToCurrentMatch(ReduceContext& c)
{
    const auto& ss = c.state.search.search_state;
    const int idx = ss.GetCurrentMatchIndex();
    if (idx < 0 || idx >= ss.GetMatchCount()) {
        return;
    }
    const auto& match = ss.GetMatches()[idx];
    const auto& cache = c.state.document.layout_cache;

    // 未計測や evict 済みのノードは text_layout が無く、行の Y を引けずにノード先頭へ寄ってしまう。
    c.layout.EnsureNodeLayout(match.node_index);
    const auto info = MdScrollInfo(c.state, ComputePaneLayout(c.state));
    const auto& entry = cache[match.node_index];
    // Why: ブロック先頭/行先頭に丸めると、長い段落内の複数マッチ間で同じ Y に集約され
    //      「次へ」を押してもスクロールしない。start_w で行単位の Y を出す。
    const auto [match_y, match_h] = entry.GetMatchYRange(match.table_row, match.table_col, match.start_w, cache.Top(static_cast<size_t>(match.node_index)));
    const float visible_height = info.content_height - (ss.IsVisible() ? SEARCH_BAR_HEIGHT : 0.0f);
    auto& viewport = c.state.view.viewport;
    const float scroll_y = viewport.GetScrollY();
    const float effective_bottom = scroll_y + visible_height;

    if (match_y < scroll_y || match_y + match_h > effective_bottom) {
        viewport.ScrollTo(match_y - visible_height / 3.0f, info.max_scroll);
    }
}

void ContinueSearch(ReduceContext& c, bool force_invalidate = false)
{
    auto& ss = c.state.search.search_state;
    const bool done = ss.ContinueSearch(c.state.document.doc.GetNodes(), std::chrono::steady_clock::now() + kSearchSliceBudget);
    // 結果は終わったときにまとめて差し替わるので、続きの区切りでは描き直さない。
    if (force_invalidate || done) {
        Invalidate(c);
    }
    if (!done) {
        return;
    }
    // 差し替えで選択は外れている。途中で前回の結果を辿っていても、見ている位置の近くで選び直す。
    ss.SetCurrentMatchNear(c.state.view.viewport.GetScrollY(), c.state.document.layout_cache);
    if (c.state.search.scroll_after_search) {
        ScrollToCurrentMatch(c);
    }
}

// 今のクエリで検索し直す。終わった時点で見ている位置の近くのマッチを選び、scroll_to_match ならそこへスクロールする。
// クエリが空ならマッチを消すだけで終わる。
void RunSearchAndLocate(ReduceContext& c, bool scroll_to_match = false)
{
    // 入力欄は UTF-16、Document テキストは UTF-8。
    c.state.search.search_state.BeginSearch(string_convert::WideToUtf8(c.state.search.query_wide));
    c.state.search.scroll_after_search = scroll_to_match;
    // クエリや前のマッチの消去を描画に出すため、始めの区切りは必ず描き直す。
    ContinueSearch(c, true);
}

void On(ReduceContext& c, const CloseSearchBarAction&)
{
    c.state.search.search_state.Hide();
    c.state.search.ime_composition.clear();
    PushEffect(c.effects, effect::SearchUnfocus{});
    Invalidate(c);
}

// 開いていれば閉じる。
void On(ReduceContext& c, const OpenSearchBarAction&)
{
    auto& sg = c.state.search;
    if (sg.search_state.IsVisible()) {
        On(c, CloseSearchBarAction{});
        return;
    }
    sg.search_state.Show();
    ResetSearchCaret(sg);
    RunSearchAndLocate(c);
    PushEffect(c.effects, effect::SearchFocus{});
}

void On(ReduceContext& c, const ToggleHighlightAction&)
{
    c.state.search.search_state.ToggleHighlightEnabled();
    Invalidate(c);
}

void On(ReduceContext& c, const SearchContinueAction&)
{
    if (c.state.search.search_state.IsSearching()) {
        ContinueSearch(c);
    }
}

void On(ReduceContext& c, const SearchSelectAllAction&)
{
    PushEffect(c.effects, effect::SearchFocus{});
}

// 検索 EDIT は非表示で WM_LBUTTONDBLCLK を直接受けないので、語の境界を自分で求めて選ぶ。
void On(ReduceContext& c, const SearchSelectWordAction& a)
{
    const auto wb = FindWordBoundaries(std::wstring_view{ c.state.search.query_wide }, static_cast<uint32_t>(a.pos));
    if (wb.found()) {
        PushEffect(c.effects, effect::SearchFocus{ static_cast<int>(wb.start), static_cast<int>(wb.end) });
    }
}

void On(ReduceContext& c, const SearchStepAction& a)
{
    auto& ss = c.state.search.search_state;
    if (!ss.IsVisible()) {
        On(c, OpenSearchBarAction{});
        return;
    }
    const bool wrapped = a.forward ? ss.NextMatch() : ss.PrevMatch();
    if (wrapped && ss.GetMatchCount() > 1) {
        PushEffect(c.effects, effect::Beep{});
    }
    ScrollToCurrentMatch(c);
    Invalidate(c);
}

void On(ReduceContext& c, const SearchTextChangedAction& a)
{
    auto& sg = c.state.search;
    // 文書の切り替えで入力欄を空にしたときなど、中身が変わらない通知は検索も再描画もしない。
    if (a.text == sg.query_wide) {
        return;
    }
    sg.query_wide = a.text;
    // 検索は時間で区切って進むので、大きな文書でも入力を待たせずにすぐ始める。
    RunSearchAndLocate(c, true);
}

void On(ReduceContext& c, const ToggleCaseSensitiveAction&)
{
    c.state.search.search_state.ToggleCaseSensitive();
    RunSearchAndLocate(c);
}

// 表示中は常に入力欄がフォーカスを持つので、表示中だけ描き直す。
void On(ReduceContext& c, const SearchSelectionAction& a)
{
    auto& sg = c.state.search;
    int anchor = a.anchor;
    int caret = a.caret;
    // 向きが分からなければ、前の基点が後ろ端に残っているときは後ろ向きとみなす (Shift+← や Shift+Home)。
    // 両端とも動いたときは前向き (EDIT と同じくキャレットを後ろ端に描く)。
    if (a.ordered && anchor != caret && caret == sg.selection_start) {
        std::swap(anchor, caret);
    }
    if (sg.caret_pos == caret && sg.selection_start == anchor) {
        return;
    }
    sg.caret_pos = caret;
    sg.selection_start = anchor;
    if (sg.search_state.IsVisible()) {
        Invalidate(c);
    }
}

void On(ReduceContext& c, const ImeCompositionAction& a)
{
    auto& sg = c.state.search;
    if (sg.ime_composition == a.text) {
        return;
    }
    sg.ime_composition = a.text;
    if (sg.search_state.IsVisible()) {
        Invalidate(c);
    }
}

void On(ReduceContext& c, const TogglePaneAction& a)
{
    c.state.view.panes.ToggleSidePane(a.target);
    // 次に表示したとき、隠す前のホバー強調が残らないようにする。
    const auto k = std::to_underlying(a.target);
    c.state.view.hover.side_index[k] = -1;
    c.state.view.hover.side_button[k] = PaneHeaderButton::None;
    PushEffect(c.effects, effect::SettleViewport{});
}

void On(ReduceContext& c, const OpenDirectoryAction& a)
{
    // 一覧を隠していると、ドロップしても何も起きないように見える。
    if (!c.state.view.panes.IsSidePaneVisible(PaneTarget::File)) {
        On(c, TogglePaneAction{ PaneTarget::File });
    }
    On(c, FilePaneDirectoryClickedAction{ a.path });
}

void On(ReduceContext& c, const SelectAllAction&)
{
    c.state.view.viewport.SelectAll(c.state.document.doc.GetNodes());
    Invalidate(c);
}

void On(ReduceContext& c, const SelectWordAction& a)
{
    const auto& nodes = c.state.document.doc.GetNodes();
    if (!NodeAt(nodes, a.node_index)) {
        return;
    }
    const auto wb = FindNodeWordBoundaries(nodes, static_cast<size_t>(a.node_index), a.text_pos);
    if (!wb.found()) {
        return;
    }
    c.state.view.viewport.SetSelection(TextSelection::MakeOrdered(static_cast<int>(wb.start_node), wb.start, static_cast<int>(wb.end_node), wb.end));
    Invalidate(c);
}

void On(ReduceContext& c, const ClearSelectionAction&)
{
    if (c.state.search.search_state.IsVisible()) {
        On(c, CloseSearchBarAction{});
        return;
    }
    auto& viewport = c.state.view.viewport;
    const bool dragging = std::holds_alternative<left_drag::TextSelect>(c.state.view.drag);
    const bool visible = viewport.GetSelection().active() || dragging;
    // テキスト選択のドラッグ中なら LButtonUp を待たずに終える。
    if (dragging) {
        c.state.view.drag = {};
    }
    viewport.ClearSelection();
    if (visible) {
        Invalidate(c);
    }
}

void On(ReduceContext& c, const CopyAction& a)
{
    const auto& sel = c.state.view.viewport.GetSelection();
    if (!sel.active()) {
        return;
    }
    const auto& nodes = c.state.document.doc.GetNodes();
    PushEffect(c.effects, effect::ClipboardWrite{
                              .text = ExtractSelectedText(nodes, sel),
                              .html = a.formatted ? ExtractSelectedTextAsHtml(nodes, sel, c.state.theme->dark) : std::string{},
                          });
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

void ReduceSidePaneZoom(ReduceContext& c, PaneTarget pane, const ZoomAction& a)
{
    auto& zoom = c.state.view.panes.SidePaneZoom(pane);
    const float old_zoom = zoom.Value();
    if (!StepZoomBy(zoom, a)) {
        return;
    }
    // 先頭に見えていた項目が先頭に残るよう、スクロール位置も倍率の比で動かす。
    auto& scroll_y = c.state.view.panes.SidePaneScroll(pane);
    scroll_y *= zoom.Value() / old_zoom;
    scroll_y = std::clamp(scroll_y, 0.0f, GetSidePaneContext(c.state, pane).info.max_scroll);
    Invalidate(c);
}

void On(ReduceContext& c, const ZoomAction& a)
{
    if (a.pane) {
        ReduceSidePaneZoom(c, *a.pane, a);
        return;
    }
    // 全レイアウトを捨てて組み直すのは段数によらず 1 回で済ませる (App::ApplyZoom)。
    auto& zoom = c.state.view.viewport.Zoom();
    const float old_zoom = zoom.Value();
    if (!StepZoomBy(zoom, a)) {
        return;
    }
    const auto anchor = SnapshotVisibleTarget(c.state);
    const float zoom_ratio = zoom.Value() / old_zoom;
    // サイドペインは別に倍率を持つので、幅・スクロール位置もペイン配置もそのまま。
    if (anchor.IsValid()) {
        // offset もズーム比でスケールしないと、ノード内の同じ位置が可視先頭に残らない。
        c.state.view.viewport.SetScrollTarget(anchor.node, anchor.offset * zoom_ratio);
    }
    PushEffect(c.effects, effect::ApplyZoom{});
}

// ポインタの下の領域だけをズームする。
void ReduceWheelZoom(ReduceContext& c, const MouseWheelAction& a)
{
    auto& accum = c.state.interaction.zoom_wheel_accum;
    // 向きが反転したら逆向きの端数は持ち越さない。
    if ((accum < 0) != (a.delta < 0)) {
        accum = 0;
    }
    accum += a.delta;
    const int steps = std::abs(accum) / WHEEL_DELTA;
    if (steps == 0) {
        return;
    }
    accum %= WHEEL_DELTA;
    On(c, ZoomAction{ a.delta > 0 ? ZoomDirection::In : ZoomDirection::Out, steps, ToPaneTarget(a.zone) });
}

void On(ReduceContext& c, const MouseWheelAction& a)
{
    if (a.ctrl) {
        ReduceWheelZoom(c, a);
    }
    else {
        ReduceWheelScroll(c, a);
    }
}

void On(ReduceContext& c, const ToggleDarkModeAction&)
{
    PushEffect(c.effects, effect::ToggleDarkMode{});
}

void On(ReduceContext& c, const ActivateAction& a)
{
    if (c.state.window.window_active != a.active) {
        c.state.window.window_active = a.active;
        Invalidate(c);
    }
    if (!a.active) {
        ClearTooltip(c);
    }
}

void On(ReduceContext& c, const ResizeAction& a)
{
    if (a.width == 0 || a.height == 0) {
        return;
    }
    // 最小化からの復元などで同じ大きさの WM_SIZE が来ても、描画面もレイアウトも変わらない。
    auto& w = c.state.window;
    if (a.width == w.client_width && a.height == w.client_height && w.cached_dpi_scale == w.client_dpi_scale) {
        return;
    }
    w.client_width = a.width;
    w.client_height = a.height;
    w.client_dpi_scale = w.cached_dpi_scale;
    PushEffect(c.effects, effect::RendererResize{ a.width, a.height, w.IsSizing() });
    w.titlebar.UpdateLayout(w.WidthDip());
    if (w.IsSizing()) {
        w.size_move = SizeMove::Resized;
        c.state.view.viewport.ClampScroll(MdMaxScroll(c.state));
        Invalidate(c);
    }
    else {
        PushEffect(c.effects, effect::SettleViewport{});
    }
}

void On(ReduceContext& c, const EnterSizeMoveAction&)
{
    c.state.window.size_move = SizeMove::Moving;
}

void On(ReduceContext& c, const ExitSizeMoveAction&)
{
    if (std::exchange(c.state.window.size_move, SizeMove::None) == SizeMove::Resized) {
        // ライブリサイズ中に大きめに取った描画面を、実際の大きさに合わせ直す。
        PushEffect(c.effects, effect::RendererResize{ c.state.window.client_width, c.state.window.client_height });
        PushEffect(c.effects, effect::SettleViewport{});
    }
}

void On(ReduceContext& c, const DpiChangedAction& a)
{
    c.state.window.cached_dpi_scale = DpiScaleFrom(static_cast<float>(a.dpi));
    // ピクセルサイズ不変でも DIP サイズが変わるため、移動ループ終了時のレイアウトを要求する。
    if (c.state.window.IsSizing()) {
        c.state.window.size_move = SizeMove::Resized;
    }
    PushEffect(c.effects, effect::ApplyDpi{ static_cast<float>(a.dpi), a.suggested });
}

void On(ReduceContext& c, const SwipeOverlayTimeoutAction&)
{
    const auto result = c.state.interaction.swipe_detector.Commit();
    PushEffect(c.effects, effect::KillTimer{ app_timer::Id::SWIPE_OVERLAY });
    if (result != NavDirection::None) {
        On(c, NavigateHistoryAction{ .forward = result == NavDirection::Forward });
        Invalidate(c);
    }
}

// ノード/見出し index に結び付く一時状態 (選択・横スクロール・ホバー・目次の現在位置・ツールチップの対象・
// 文書を対象にするドラッグ) を消す。文書を差し替えると別の要素を指すので、
// ロードとリロードの両方で消す。ホバーは描画後に引き直される (App::RefreshHoverAtCursor)。
void ResetPerNodeTransientState(ReduceContext& c)
{
    auto& view = c.state.view;
    view.viewport.ClearSelection();
    view.block_scroll_x.clear();
    view.hover = {};
    view.active_toc_index = -1;
    // スピナーを出している間は描画が同期しないので、クリックした文書の分が残っていることがある。
    view.keep_toc_pane_once = false;
    // ツールチップの同一判定は node / TOC の index で行う。
    ClearTooltip(c);
    if (IsDocumentBoundDrag(view.drag)) {
        view.drag = {};
    }
}

void On(ReduceContext& c, const DocumentOpenedAction& a)
{
    c.emit_scroll_effects = false;
    ResetPerNodeTransientState(c);
    // 同一文書の再パースと違い、サイドペインのスクロールも先頭へ戻す。
    c.state.view.panes.ResetScrollStates();
    // 旧文書のマッチ位置が新 nodes に対して誤用されるのを防ぐ。入力欄を空にしたときの EN_CHANGE は、
    // ここでクエリを先に空にしておくので何もせず終わる。
    auto& sg = c.state.search;
    sg.search_state.Hide();
    sg.query_wide.clear();
    sg.ime_composition.clear();
    ResetSearchCaret(sg);
    PushEffect(c.effects, effect::SearchUnfocus{ /*clear_text=*/true });

    const auto& doc = c.state.document.doc;
    if (a.follow_file_pane && !doc.GetDirectory().empty()) {
        c.state.file_explorer.SetDirectory(doc.GetDirectory());
    }
    c.state.file_explorer.SetCurrentFile(doc.GetFilePath());
    c.state.view.nav_history.CommitOpened(a.left);
    RestoreScrollAfterLoad(c.state);
}

void On(ReduceContext& c, const DocumentReloadedAction& a)
{
    c.emit_scroll_effects = false;
    // ReloadSettledAction の検索し直しが終わるまで、変わらなかったノードのハイライトと件数を残す。
    c.state.search.search_state.KeepMatchesOfUnchangedNodes(a.unchanged);
    ResetPerNodeTransientState(c);
    // 直前の移動先 (ノード基準) は旧文書のもの。捨ててピクセル位置で固定する。
    c.state.view.viewport.SetScrollY(a.scroll_y);
}

void On(ReduceContext& c, const ReloadSettledAction&)
{
    if (c.state.search.search_state.IsVisible()) {
        RunSearchAndLocate(c);
    }
}

void On(ReduceContext& c, const LoadFailedAction&)
{
    DiscardPendingLoad(c.state);
}

void On(ReduceContext& c, const OpenFileAction&)
{
    PushEffect(c.effects, effect::OpenFileDialog{});
}

void On(ReduceContext& c, const NewWindowAction& a)
{
    std::wstring path = a.path;
    if (path.empty()) {
        const auto& doc = c.state.document.doc;
        path = doc.HasBackingFile() ? doc.GetFilePath() : c.state.file_explorer.GetDirectory();
    }
    PushEffect(c.effects, effect::LaunchNewWindow{ std::move(path), a.anchor });
}

void On(ReduceContext& c, const ReloadFileAction&)
{
    PushEffect(c.effects, effect::ReloadFile{});
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
    ReduceContext c{ state, effects, layout };
    // viewport を動かす経路 (キー・ホイール・スクロールバー・履歴・検索のマッチ移動など) を問わず、
    // 前後の scroll_y を比べてスクロールの副作用を 1 回だけ積む。
    const float old_scroll = state.view.viewport.GetScrollY();
    const bool had_capture = CaptureRequired(state);

    std::visit([&](const auto& a) { On(c, a); }, action);
    if (c.emit_scroll_effects && state.view.viewport.GetScrollY() != old_scroll) {
        EmitScrollChangedSideEffects(c);
    }
    // 描き直しとキャプチャの変更は、モーダルの副作用 (メニュー・ファイルを開くダイアログ) より先に済ませるよう先頭に積む。
    // 描き直しを後ろに積むと、メニューを閉じるまで右ドラッグの軌跡が残った。
    if (c.invalidated) {
        effects.insert(effects.begin(), effect::InvalidateWindow{});
    }
    const bool needs_capture = CaptureRequired(state);
    if (needs_capture != had_capture) {
        effects.insert(effects.begin(), needs_capture ? SideEffect{ effect::SetCapture{} } : SideEffect{ effect::ReleaseCapture{} });
    }
    return effects;
}
