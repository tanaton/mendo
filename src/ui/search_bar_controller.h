#pragma once
#include "layout_cache.h"
#include "render_params.h"
#include "search_state.h"
#include "side_effect.h"
#include "string_convert.h"
#include "ui_constants.h"
#include "viewport_manager.h"
#include <chrono>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

// 検索バーがマッチへスクロールするときに App へ問い合わせること。
class SearchBarHost {
public:
    virtual float MdPaneHeight() = 0;
    virtual float MdMaxScroll() = 0;
    // node_index のノードを計測済みにする (検索マッチの行位置を引けるようにする)。
    virtual void EnsureNodeLayout(int node_index) = 0;

protected:
    ~SearchBarHost() = default;
};

// 検索バーのUI状態管理。
// SearchState（ドメインロジック）を参照で保持し、検索バー固有の
// UIステート（フォーカス、キャレット、ドラッグ選択、ホバー）を管理する。
// 再描画・フォーカス・ビープは effects に積み、App::Execute に任せる。
class SearchBarController {
public:
    constexpr void Init(SearchState& state, ViewportManager& viewport, LayoutCache& cache, SearchBarHost& host) noexcept
    {
        state_ = &state;
        viewport_ = &viewport;
        cache_ = &cache;
        host_ = &host;
    }

    void OnOpen(SideEffectList& effects, const std::pmr::vector<Node>& nodes)
    {
        if (state_->IsVisible()) {
            OnClose(effects);
            return;
        }
        state_->Show();
        ResetCaretAndSelection();

        if (!state_->GetQuery().empty()) {
            RunSearchAndLocate(effects, nodes);
        }

        PushEffect(effects, effect::SearchFocus{});
        PushEffect(effects, effect::InvalidateWindow{});
    }

    void OnClose(SideEffectList& effects)
    {
        state_->Hide();
        ResetInteractionState();
        PushEffect(effects, effect::SearchUnfocus{});
        PushEffect(effects, effect::InvalidateWindow{});
    }

    void OnNext(SideEffectList& effects)
    {
        AfterMatchStep(effects, state_->NextMatch());
    }

    void OnPrev(SideEffectList& effects)
    {
        AfterMatchStep(effects, state_->PrevMatch());
    }

    void OnTextChanged(SideEffectList& effects, std::wstring_view text, const std::pmr::vector<Node>& nodes)
    {
        // 文書の切り替えで入力欄を空にしたときなど、中身が変わらない通知は検索も再描画もしない。
        if (text == query_wide_) {
            return;
        }
        // 検索バー入力は IME 経由で wstring。表示とヒットテストにはこのまま使い、
        // SearchState は Document テキスト (UTF-8) と比較するため変換して渡す。
        query_wide_.assign(text);
        std::pmr::string text_utf8;
        string_convert::WideToUtf8(text, text_utf8);
        state_->SetQuery(text_utf8);
        if (text.empty()) {
            state_->ExecuteSearch(nodes);
            PushEffect(effects, effect::InvalidateWindow{});
            return;
        }
        // 検索は時間で区切って進むので、大きな文書でも入力を待たせずにすぐ始める。
        RunSearchAndLocate(effects, nodes, true);
    }

    void OnToggleCaseSensitive(SideEffectList& effects, const std::pmr::vector<Node>& nodes)
    {
        state_->ToggleCaseSensitive();
        if (!state_->GetQuery().empty()) {
            RunSearchAndLocate(effects, nodes);
        }
        PushEffect(effects, effect::InvalidateWindow{});
    }

    void OnToggleHighlight(SideEffectList& effects)
    {
        state_->ToggleHighlightEnabled();
        PushEffect(effects, effect::InvalidateWindow{});
    }

    void SetSelection(SideEffectList& effects, int sel_start, int sel_end)
    {
        if (caret_pos_ == sel_end && selection_start_ == sel_start) {
            return;
        }
        caret_pos_ = sel_end;
        selection_start_ = sel_start;
        if (HasFocus()) {
            PushEffect(effects, effect::InvalidateWindow{});
        }
    }

    void SetImeComposition(SideEffectList& effects, std::wstring_view comp)
    {
        if (ime_composition_ == comp) {
            return;
        }
        ime_composition_ = comp;
        if (HasFocus()) {
            PushEffect(effects, effect::InvalidateWindow{});
        }
    }

    // 1 回でこれを超える分は、メッセージループが入力と描画を配った後の空き時間 (OnContinue) へ回す。
    static constexpr auto kSearchSliceBudget = std::chrono::milliseconds(8);

    // 終わった時点で見ている位置の近くのマッチを選び、scroll_to_match ならそこへスクロールする。
    void RunSearchAndLocate(SideEffectList& effects, const std::pmr::vector<Node>& nodes, bool scroll_to_match = false)
    {
        state_->BeginSearch();
        scroll_after_search_ = scroll_to_match;
        // クエリや前のマッチの消去を描画に出すため、始めの区切りは必ず描き直す。
        ContinueSearch(effects, nodes, true);
    }

    void OnContinue(SideEffectList& effects, const std::pmr::vector<Node>& nodes)
    {
        if (state_->IsSearching()) {
            ContinueSearch(effects, nodes);
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

        // 未計測や evict 済みのノードは text_layout が無く、行の Y を引けずにノード先頭へ寄ってしまう。
        host_->EnsureNodeLayout(match.node_index);
        const auto& entry = (*cache_)[match.node_index];
        // Why: ブロック先頭/行先頭に丸めると、長い段落内の複数マッチ間で同じ Y に集約され
        //      「次へ」を押してもスクロールしない。start_w で行単位の Y を出す。
        const auto [match_y, match_h] = entry.GetMatchYRange(match.table_row, match.table_col, match.start_w, cache_->Top(static_cast<size_t>(match.node_index)));
        const float visible_height = host_->MdPaneHeight() - (state_->IsVisible() ? SEARCH_BAR_HEIGHT : 0.0f);
        const float scroll_y = viewport_->GetScrollY();
        const float effective_bottom = scroll_y + visible_height;

        if (match_y < scroll_y || match_y + match_h > effective_bottom) {
            viewport_->ScrollTo(match_y - visible_height / 3.0f, host_->MdMaxScroll());
        }
    }

    void Reset()
    {
        state_->Reset();
        query_wide_.clear();
        ResetInteractionState();
        ResetCaretAndSelection();
    }


    SearchBarRenderState BuildRenderState() const
    {
        SearchBarRenderState sb;
        sb.visible = state_->IsVisible();
        sb.query = GetQueryWide();
        sb.current_match = state_->GetCurrentMatchIndex();
        sb.total_matches = state_->HasResult() ? state_->GetMatchCount() : -1;
        sb.caret_pos = caret_pos_;
        sb.selection_start = selection_start_;
        sb.ime_composition = ime_composition_;
        sb.case_sensitive = state_->IsCaseSensitive();
        sb.highlight_enabled = state_->IsHighlightEnabled();
        return sb;
    }

    // 表示中は常に入力欄がフォーカスを持つ。
    bool HasFocus() const noexcept
    {
        return state_->IsVisible();
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

    // 入力欄に表示している検索クエリ。SearchState::GetQuery() の UTF-16 版。
    const std::pmr::wstring& GetQueryWide() const noexcept
    {
        return query_wide_;
    }

private:
    void ContinueSearch(SideEffectList& effects, const std::pmr::vector<Node>& nodes, bool force_invalidate = false)
    {
        const bool done = state_->ContinueSearch(nodes, std::chrono::steady_clock::now() + kSearchSliceBudget);
        // 結果は終わったときにまとめて差し替わるので、続きの区切りでは描き直さない。
        if (force_invalidate || done) {
            PushEffect(effects, effect::InvalidateWindow{});
        }
        if (!done) {
            return;
        }
        // 差し替えで選択は外れている。途中で前回の結果を辿っていても、見ている位置の近くで選び直す。
        state_->SetCurrentMatchNear(viewport_->GetScrollY(), *cache_);
        if (scroll_after_search_) {
            ScrollToCurrentMatch();
        }
    }

    void AfterMatchStep(SideEffectList& effects, bool wrapped)
    {
        if (wrapped && state_->GetMatchCount() > 1) {
            PushEffect(effects, effect::Beep{});
        }
        ScrollToCurrentMatch();
        PushEffect(effects, effect::InvalidateWindow{});
    }

    constexpr void ResetCaretAndSelection() noexcept
    {
        caret_pos_ = -1;
        selection_start_ = -1;
    }

    void ResetInteractionState()
    {
        ime_composition_.clear();
        state_->StopSearch();
    }

    SearchState* state_ = nullptr;
    ViewportManager* viewport_ = nullptr;
    LayoutCache* cache_ = nullptr;
    SearchBarHost* host_ = nullptr;

    int caret_pos_ = -1;
    int selection_start_ = -1;
    std::pmr::wstring ime_composition_;
    std::pmr::wstring query_wide_;
    bool scroll_after_search_ = false;
};
