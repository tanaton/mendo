#pragma once
#include "app_constants.h"
#include "doc_text.h"
#include "layout_cache.h"
#include "renderer.h"
#include "search_state.h"
#include "string_convert.h"
#include "ui_constants.h"
#include "viewport_manager.h"
#include <algorithm>
#include <cmath>
#include <memory_resource>
#include <string>
#include <string_view>
#include <windows.h>

// 検索バーのUI状態管理。
// SearchState（ドメインロジック）を参照で保持し、検索バー固有の
// UIステート（フォーカス、キャレット、ドラッグ選択、ホバー）を管理する。
// Win32依存の操作は Cb 経由でAppに委譲する。
template <class Cb>
class SearchBarControllerT {
public:
    SearchBarControllerT() = default;

    constexpr void Init(SearchState& state, ViewportManager& viewport, LayoutCache& cache, Cb cb) noexcept
    {
        state_ = &state;
        viewport_ = &viewport;
        cache_ = &cache;
        cb_ = std::move(cb);
    }

    void OnOpen(const std::pmr::vector<Node>& nodes)
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

        if (!state_->GetQuery().empty()) {
            RunSearchAndLocate(nodes);
        }

        RestartCaretBlink();
        cb_.focus_select_all();
        cb_.invalidate();
    }

    void OnClose()
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

    void OnNext()
    {
        if (state_->NextMatch() && state_->GetMatchCount() > 1) {
            cb_.on_wrap_around();
        }
        ScrollToCurrentMatch();
        cb_.invalidate();
    }

    void OnPrev()
    {
        if (state_->PrevMatch() && state_->GetMatchCount() > 1) {
            cb_.on_wrap_around();
        }
        ScrollToCurrentMatch();
        cb_.invalidate();
    }

    void OnTextChanged(std::wstring_view text, const std::pmr::vector<Node>& nodes)
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

    void OnToggleCaseSensitive(const std::pmr::vector<Node>& nodes)
    {
        state_->ToggleCaseSensitive();
        if (!state_->GetQuery().empty()) {
            RunSearchAndLocate(nodes);
        }
        cb_.invalidate();
    }

    void OnToggleHighlight()
    {
        state_->ToggleHighlightEnabled();
        cb_.invalidate();
    }

    void SetSelection(int sel_start, int sel_end) noexcept
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

    void SetImeComposition(std::wstring_view comp)
    {
        if (ime_composition_ == comp) {
            return;
        }
        ime_composition_ = comp;
        if (has_focus_) {
            cb_.invalidate_search_bar();
        }
    }

    void OnCaretBlinkTimer() noexcept
    {
        caret_visible_ = !caret_visible_;
        if (has_focus_) {
            cb_.invalidate_search_bar();
        }
    }

    void OnDebounceTimer(const std::pmr::vector<Node>& nodes)
    {
        cb_.kill_timer(app_timer::Id::SEARCH_DEBOUNCE);
        RunSearchAndLocate(nodes, true);
        cb_.invalidate();
    }

    void RunSearchAndLocate(const std::pmr::vector<Node>& nodes, bool scroll_to_match = false)
    {
        state_->ExecuteSearch(nodes);
        if (state_->GetMatchCount() > 0) {
            state_->SetCurrentMatchNear(viewport_->GetScrollY(), *cache_);
            if (scroll_to_match) {
                ScrollToCurrentMatch();
            }
        }
    }

    void ScrollToCurrentMatch()
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

        if (match_y < scroll_y || match_y + match_h > effective_bottom) {
            const float target = std::max(0.0f, match_y - visible_height / 3.0f);
            // Why: ScrollTo は scroll_target_ を無効化してくれる。SetScrollY のままだと、
            //      直後のレイアウト変化 (Mermaid 読込等) で古い scroll_target から再計算されて
            //      検索ジャンプが上書きされる恐れがある。
            viewport_->ScrollTo(target);
            cb_.on_scroll_changed(md_pane_height);
        }
    }

    void Reset()
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

    bool IsDragging() const noexcept
    {
        return dragging_;
    }

    void StartDrag(int anchor_pos) noexcept
    {
        dragging_ = true;
        drag_anchor_ = anchor_pos;
    }

    void EndDrag() noexcept
    {
        dragging_ = false;
    }

    int GetDragAnchor() const noexcept
    {
        return drag_anchor_;
    }

    void OnCaptureChanged() noexcept
    {
        dragging_ = false;
    }

    void UpdateHoverFromZone(SearchBarHitZone zone) noexcept
    {
        // Input ゾーンはテキスト編集領域でホバー強調は不要なので None に丸める。
        const auto new_hover = (zone == SearchBarHitZone::Input) ? SearchBarHitZone::None : zone;
        if (new_hover != hover_) {
            hover_ = new_hover;
            cb_.invalidate_search_bar();
        }
    }

    SearchBarHitZone GetHover() const noexcept
    {
        return hover_;
    }

    void ResetHover() noexcept
    {
        hover_ = SearchBarHitZone::None;
    }

    SearchBarRenderState BuildRenderState() const
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

    bool HasFocus() const noexcept
    {
        return has_focus_;
    }

    int GetCaretPos() const noexcept
    {
        return caret_pos_;
    }

    int GetSelectionStart() const noexcept
    {
        return selection_start_;
    }

    const std::pmr::wstring& GetImeComposition() const noexcept
    {
        return ime_composition_;
    }

    // 検索クエリ (UTF-8) を wstring 化したものを返す。
    // BuildRenderState / ドラッグ中の HitTest / クリック時の HitTest が同じキャッシュを共有する。
    // 同一 UTF-8 のときは Utf8ToWide を skip する。
    const std::pmr::wstring& GetQueryWide() const
    {
        const auto& utf8 = state_->GetQuery();
        if (query_wide_cache_key_ != utf8) {
            query_wide_cache_.clear();
            string_convert::Utf8ToWide(utf8, query_wide_cache_);
            query_wide_cache_key_.assign(utf8);
        }
        return query_wide_cache_;
    }

private:
    void RestartCaretBlink() noexcept
    {
        cb_.kill_timer(app_timer::Id::SEARCH_CARET);
        const UINT blink_time = GetCaretBlinkTime();
        if (blink_time > 0 && blink_time != INFINITE) {
            cb_.set_timer(app_timer::Id::SEARCH_CARET, blink_time);
        }
    }

    SearchState* state_ = nullptr;
    ViewportManager* viewport_ = nullptr;
    LayoutCache* cache_ = nullptr;
    Cb cb_{};

    SearchBarHitZone hover_ = SearchBarHitZone::None;
    bool caret_visible_ = false;
    bool has_focus_ = false;
    int caret_pos_ = -1;
    int selection_start_ = -1;
    bool dragging_ = false;
    int drag_anchor_ = 0;
    std::pmr::wstring ime_composition_;
    mutable std::pmr::wstring query_wide_cache_;
    mutable std::pmr::string query_wide_cache_key_;
};
