#pragma once
#include "search_bar_controller.h"
#include "app_constants.h"
#include "viewport_manager.h"
#include "layout_cache.h"
#include "renderer.h" // SearchBarRenderState
#include "string_convert.h"
#include <algorithm>
#include <cmath>

template <class Cb>
void SearchBarControllerT<Cb>::Init(SearchState& state, ViewportManager& viewport, LayoutCache& cache, Cb cb)
{
    state_ = &state;
    viewport_ = &viewport;
    cache_ = &cache;
    cb_ = std::move(cb);
}

template <class Cb>
void SearchBarControllerT<Cb>::OnOpen(const std::pmr::vector<Node>& nodes)
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

template <class Cb>
void SearchBarControllerT<Cb>::OnClose()
{
    state_->Hide();
    hover_ = SearchBarHitZone::None;
    has_focus_ = false;
    caret_visible_ = false;
    ime_composition_.clear();
    cb_.kill_timer(app_timer::Id::SEARCH_CARET);
    cb_.kill_timer(app_timer::Id::SEARCH_DEBOUNCE);
    cb_.unfocus();
    cb_.invalidate();
}

template <class Cb>
void SearchBarControllerT<Cb>::OnNext()
{
    if (state_->NextMatch() && state_->GetMatchCount() > 1) {
        cb_.on_wrap_around();
    }
    ScrollToCurrentMatch();
    cb_.invalidate();
}

template <class Cb>
void SearchBarControllerT<Cb>::OnPrev()
{
    if (state_->PrevMatch() && state_->GetMatchCount() > 1) {
        cb_.on_wrap_around();
    }
    ScrollToCurrentMatch();
    cb_.invalidate();
}

template <class Cb>
void SearchBarControllerT<Cb>::OnTextChanged(std::wstring_view text, const std::pmr::vector<Node>& nodes)
{
    // 検索バー入力は IME 経由で wstring。SearchState は Document テキスト (UTF-8) と比較するため変換。
    std::pmr::string text_utf8;
    string_convert::WideToUtf8(text, text_utf8);
    state_->SetQuery(text_utf8);
    cb_.kill_timer(app_timer::Id::SEARCH_DEBOUNCE);

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
    cb_.set_timer(app_timer::Id::SEARCH_DEBOUNCE, 150);
}

template <class Cb>
void SearchBarControllerT<Cb>::OnToggleCaseSensitive(const std::pmr::vector<Node>& nodes)
{
    state_->ToggleCaseSensitive();
    if (!state_->GetQuery().empty()) {
        RunSearchAndLocate(nodes);
    }
    cb_.invalidate();
}

template <class Cb>
void SearchBarControllerT<Cb>::OnToggleHighlight()
{
    state_->ToggleHighlightEnabled();
    cb_.invalidate();
}

template <class Cb>
void SearchBarControllerT<Cb>::SetSelection(int sel_start, int sel_end) noexcept
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

template <class Cb>
void SearchBarControllerT<Cb>::SetImeComposition(std::wstring_view comp)
{
    if (ime_composition_ == comp) {
        return;
    }
    ime_composition_ = comp;
    if (has_focus_) {
        cb_.invalidate_search_bar();
    }
}

template <class Cb>
void SearchBarControllerT<Cb>::OnCaretBlinkTimer()
{
    caret_visible_ = !caret_visible_;
    if (has_focus_) {
        cb_.invalidate_search_bar();
    }
}

template <class Cb>
void SearchBarControllerT<Cb>::OnDebounceTimer(const std::pmr::vector<Node>& nodes)
{
    cb_.kill_timer(app_timer::Id::SEARCH_DEBOUNCE);
    RunSearchAndLocate(nodes, true);
    cb_.invalidate();
}

template <class Cb>
void SearchBarControllerT<Cb>::RunSearchAndLocate(const std::pmr::vector<Node>& nodes, bool scroll_to_match)
{
    state_->ExecuteSearch(nodes);
    if (state_->GetMatchCount() > 0) {
        state_->SetCurrentMatchNear(viewport_->GetScrollY(), *cache_);
        if (scroll_to_match) {
            ScrollToCurrentMatch();
        }
    }
}

template <class Cb>
void SearchBarControllerT<Cb>::ScrollToCurrentMatch()
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
    // Why: ブロック先頭/行先頭に丸めると、長い段落内の複数マッチ間で同じ Y に集約され
    //      「次へ」を押してもスクロールしない。start_w で行単位の Y を出す。
    const auto [match_y, match_h] = entry.GetMatchYRange(match.table_row, match.table_col, match.start_w, entry.text_top);
    const float md_pane_height = cb_.get_md_pane_height();
    const float visible_height = md_pane_height - (state_->IsVisible() ? SEARCH_BAR_HEIGHT : 0.0f);
    const float scroll_y = viewport_->GetScrollY();
    const float effective_bottom = scroll_y + visible_height;

    // マッチが可視範囲外の場合のみスクロール
    if (match_y < scroll_y || match_y + match_h > effective_bottom) {
        const float target = std::max(0.0f, match_y - visible_height / 3.0f);
        // Why: ScrollTo は scroll_target_ を無効化してくれる。SetScrollY のままだと、
        //      直後のレイアウト変化 (Mermaid 読込等) で古い scroll_target から再計算されて
        //      検索ジャンプが上書きされる恐れがある。
        viewport_->ScrollTo(target);
        cb_.on_scroll_changed(md_pane_height);
    }
}

template <class Cb>
void SearchBarControllerT<Cb>::Reset()
{
    state_->Reset();
    hover_ = SearchBarHitZone::None;
    has_focus_ = false;
    caret_visible_ = false;
    caret_pos_ = -1;
    selection_start_ = -1;
    dragging_ = false;
    ime_composition_.clear();
    cb_.kill_timer(app_timer::Id::SEARCH_CARET);
    cb_.kill_timer(app_timer::Id::SEARCH_DEBOUNCE);
}

template <class Cb>
void SearchBarControllerT<Cb>::StartDrag(int anchor_pos) noexcept
{
    dragging_ = true;
    drag_anchor_ = anchor_pos;
}

template <class Cb>
void SearchBarControllerT<Cb>::UpdateHoverFromZone(SearchBarHitZone zone)
{
    // Input ゾーンはテキスト編集領域でホバー強調は不要なので None に丸める。
    const auto new_hover = (zone == SearchBarHitZone::Input) ? SearchBarHitZone::None : zone;
    if (new_hover != hover_) {
        hover_ = new_hover;
        cb_.invalidate_search_bar();
    }
}

template <class Cb>
const std::pmr::wstring& SearchBarControllerT<Cb>::GetQueryWide() const
{
    const auto& utf8 = state_->GetQuery();
    if (query_wide_cache_key_ != utf8) {
        query_wide_cache_.clear();
        string_convert::Utf8ToWide(utf8, query_wide_cache_);
        query_wide_cache_key_.assign(utf8);
    }
    return query_wide_cache_;
}

template <class Cb>
SearchBarRenderState SearchBarControllerT<Cb>::BuildRenderState() const
{
    SearchBarRenderState sb;
    sb.visible = state_->IsVisible();
    sb.query = GetQueryWide();
    sb.current_match = state_->GetCurrentMatchIndex();
    sb.total_matches = state_->GetMatchCount();
    sb.has_focus = has_focus_;
    sb.caret_visible = has_focus_ && caret_visible_;
    sb.caret_pos = caret_pos_;
    sb.selection_start = selection_start_;
    sb.ime_composition = ime_composition_;
    sb.case_sensitive = state_->IsCaseSensitive();
    sb.highlight_enabled = state_->IsHighlightEnabled();
    sb.up_btn_hovered = (hover_ == SearchBarHitZone::Up);
    sb.down_btn_hovered = (hover_ == SearchBarHitZone::Down);
    sb.close_btn_hovered = (hover_ == SearchBarHitZone::Close);
    sb.case_btn_hovered = (hover_ == SearchBarHitZone::CaseSensitive);
    sb.highlight_btn_hovered = (hover_ == SearchBarHitZone::Highlight);
    return sb;
}

template <class Cb>
void SearchBarControllerT<Cb>::RestartCaretBlink()
{
    cb_.kill_timer(app_timer::Id::SEARCH_CARET);
    const UINT blink_time = GetCaretBlinkTime();
    if (blink_time > 0 && blink_time != INFINITE) {
        cb_.set_timer(app_timer::Id::SEARCH_CARET, blink_time);
    }
}
