#include "search_bar_controller.h"
#include "viewport_manager.h"
#include "layout_cache.h"
#include "renderer.h"  // SearchBarRenderState
#include <algorithm>
#include <cmath>

void SearchBarController::Init(SearchState& state, ViewportManager& viewport,
                                LayoutCache& cache, Callbacks cb)
{
    state_ = &state;
    viewport_ = &viewport;
    cache_ = &cache;
    cb_ = std::move(cb);
}

// ============================================================
// イベントハンドラ
// ============================================================

void SearchBarController::OnOpen(const std::pmr::vector<Node>& nodes)
{
    if (state_->IsVisible()) {
        OnClose();
        return;
    }
    state_->Show();
    has_focus_ = true;
    caret_visible_ = true;
    caret_pos_ = -1;
    selection_start_ = -1;

    // 前回のクエリが残っている場合は検索を再実行
    if (!state_->GetQuery().empty()) {
        RunSearchAndLocate(nodes);
    }

    RestartCaretBlink();
    cb_.focus_select_all();
    cb_.invalidate();
}

void SearchBarController::OnClose()
{
    state_->Hide();
    hover_ = HoverZone::None;
    has_focus_ = false;
    caret_visible_ = false;
    ime_composition_.clear();
    cb_.kill_timer(TIMER_CARET);
    cb_.kill_timer(TIMER_DEBOUNCE);
    cb_.unfocus();
    cb_.invalidate();
}

void SearchBarController::OnNext()
{
    if (state_->NextMatch() && state_->GetMatchCount() > 1) {
        MessageBeep(MB_OK);
    }
    ScrollToCurrentMatch();
    cb_.invalidate();
}

void SearchBarController::OnPrev()
{
    if (state_->PrevMatch() && state_->GetMatchCount() > 1) {
        MessageBeep(MB_OK);
    }
    ScrollToCurrentMatch();
    cb_.invalidate();
}

void SearchBarController::OnTextChanged(std::wstring_view text,
                                         const std::pmr::vector<Node>& nodes)
{
    state_->SetQuery(text);
    cb_.kill_timer(TIMER_DEBOUNCE);

    if (text.empty()) {
        state_->ExecuteSearch(nodes);
        cb_.invalidate();
        return;
    }

    // 小規模ドキュメント（≤1000ノード）: 即座に検索実行
    if (nodes.size() <= 1000) {
        RunSearchAndLocate(nodes, true);
        cb_.invalidate();
        return;
    }

    // 大規模ドキュメント: デバウンスで連続入力中の再検索を抑制
    cb_.invalidate();
    cb_.set_timer(TIMER_DEBOUNCE, 150);
}

void SearchBarController::OnToggleCaseSensitive(
    const std::pmr::vector<Node>& nodes)
{
    state_->ToggleCaseSensitive();
    if (!state_->GetQuery().empty()) {
        RunSearchAndLocate(nodes);
    }
    cb_.invalidate();
}

void SearchBarController::OnToggleHighlight()
{
    state_->ToggleHighlightEnabled();
    cb_.invalidate();
}

void SearchBarController::SetSelection(int sel_start, int sel_end) noexcept
{
    if (caret_pos_ == sel_end && selection_start_ == sel_start) {
        return;
    }
    caret_pos_ = sel_end;
    selection_start_ = sel_start;
    if (has_focus_) {
        caret_visible_ = true;
        RestartCaretBlink();
        cb_.invalidate_search_bar();
    }
}

void SearchBarController::SetImeComposition(std::wstring_view comp)
{
    if (ime_composition_ == comp) {
        return;
    }
    ime_composition_ = comp;
    if (has_focus_) {
        cb_.invalidate_search_bar();
    }
}

// ============================================================
// タイマーハンドラ
// ============================================================

void SearchBarController::OnCaretBlinkTimer()
{
    caret_visible_ = !caret_visible_;
    if (has_focus_) {
        cb_.invalidate_search_bar();
    }
}

void SearchBarController::OnDebounceTimer(const std::pmr::vector<Node>& nodes)
{
    cb_.kill_timer(TIMER_DEBOUNCE);
    RunSearchAndLocate(nodes, true);
    cb_.invalidate();
}

// ============================================================
// 検索実行
// ============================================================

void SearchBarController::RunSearchAndLocate(
    const std::pmr::vector<Node>& nodes, bool scroll_to_match)
{
    state_->ExecuteSearch(nodes);
    if (state_->GetMatchCount() > 0) {
        state_->SetCurrentMatchNear(viewport_->GetScrollY(), *cache_);
        if (scroll_to_match) {
            ScrollToCurrentMatch();
        }
    }
}

void SearchBarController::ScrollToCurrentMatch()
{
    const int idx = state_->GetCurrentMatchIndex();
    if (idx < 0 || idx >= state_->GetMatchCount()) {
        return;
    }
    const auto& match = state_->GetMatches()[idx];
    if (match.node_index < 0 ||
        match.node_index >= static_cast<int>(cache_->size())) {
        return;
    }

    const auto& entry = (*cache_)[match.node_index];
    const float match_y = entry.y_position;
    const float md_pane_height = cb_.get_md_pane_height();
    const float visible_height = md_pane_height
        - (state_->IsVisible() ? SEARCH_BAR_HEIGHT : 0.0f);
    const float scroll_y = viewport_->GetScrollY();
    const float effective_bottom = scroll_y + visible_height;

    // マッチが可視範囲外の場合のみスクロール
    if (match_y < scroll_y || match_y + entry.height > effective_bottom) {
        const float target = std::max(0.0f, match_y - visible_height / 3.0f);
        viewport_->SetScrollY(target);
        cb_.on_scroll_changed(visible_height);
    }
}

// ============================================================
// リセット
// ============================================================

void SearchBarController::Reset()
{
    state_->Reset();
    hover_ = HoverZone::None;
    has_focus_ = false;
    caret_visible_ = false;
    caret_pos_ = -1;
    selection_start_ = -1;
    dragging_ = false;
    ime_composition_.clear();
    cb_.kill_timer(TIMER_CARET);
    cb_.kill_timer(TIMER_DEBOUNCE);
}

// ============================================================
// ドラッグ選択
// ============================================================

void SearchBarController::StartDrag(int anchor_pos) noexcept
{
    dragging_ = true;
    drag_anchor_ = anchor_pos;
}

// ============================================================
// ホバー管理
// ============================================================

SearchBarController::HoverZone SearchBarController::UpdateHover(
    float dip_x, float dip_y, const SearchBarLayout& sbl)
{
    const auto old_hover = hover_;
    hover_ = HoverZone::None;

    if (dip_y >= sbl.bar_top) {
        if (PointInRect(dip_x, dip_y, sbl.up_btn)) {
            hover_ = HoverZone::Up;
        }
        else if (PointInRect(dip_x, dip_y, sbl.down_btn)) {
            hover_ = HoverZone::Down;
        }
        else if (PointInRect(dip_x, dip_y, sbl.case_btn)) {
            hover_ = HoverZone::CaseSensitive;
        }
        else if (PointInRect(dip_x, dip_y, sbl.highlight_btn)) {
            hover_ = HoverZone::Highlight;
        }
        else if (PointInRect(dip_x, dip_y, sbl.close_btn)) {
            hover_ = HoverZone::Close;
        }
    }

    if (hover_ != old_hover) {
        cb_.invalidate();
    }
    return hover_;
}

// ============================================================
// レンダー状態構築
// ============================================================

SearchBarRenderState SearchBarController::BuildRenderState() const
{
    SearchBarRenderState sb;
    sb.visible = state_->IsVisible();
    sb.query = state_->GetQuery();
    sb.current_match = state_->GetCurrentMatchIndex();
    sb.total_matches = state_->GetMatchCount();
    sb.has_focus = has_focus_;
    sb.caret_visible = has_focus_ && caret_visible_;
    sb.caret_pos = caret_pos_;
    sb.selection_start = selection_start_;
    sb.ime_composition = ime_composition_;
    sb.case_sensitive = state_->IsCaseSensitive();
    sb.highlight_enabled = state_->IsHighlightEnabled();
    sb.up_btn_hovered = (hover_ == HoverZone::Up);
    sb.down_btn_hovered = (hover_ == HoverZone::Down);
    sb.close_btn_hovered = (hover_ == HoverZone::Close);
    sb.case_btn_hovered = (hover_ == HoverZone::CaseSensitive);
    sb.highlight_btn_hovered = (hover_ == HoverZone::Highlight);
    return sb;
}

// ============================================================
// 内部ヘルパー
// ============================================================

void SearchBarController::RestartCaretBlink()
{
    cb_.kill_timer(TIMER_CARET);
    const UINT blink_time = GetCaretBlinkTime();
    if (blink_time > 0 && blink_time != INFINITE) {
        cb_.set_timer(TIMER_CARET, blink_time);
    }
}
