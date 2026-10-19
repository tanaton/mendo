#pragma once
#include "app_constants.h"
#include "layout_cache.h"
#include "render_params.h"
#include "search_state.h"
#include "side_effect.h"
#include "string_convert.h"
#include "ui_constants.h"
#include "viewport_manager.h"
#include <chrono>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// 検索バーのUI状態管理。
// SearchState（ドメインロジック）を参照で保持し、検索バー固有の
// UIステート（フォーカス、キャレット、ドラッグ選択、ホバー）を管理する。
// 再描画・タイマー・フォーカス・ビープは effects に積み、App::Execute に任せる。
// Cb はレイアウト計測とペイン高さの問い合わせだけを担う。
template <class Cb>
class SearchBarControllerT {
public:
    constexpr void Init(SearchState& state, ViewportManager& viewport, LayoutCache& cache, Cb cb) noexcept
    {
        state_ = &state;
        viewport_ = &viewport;
        cache_ = &cache;
        cb_ = std::move(cb);
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
            RunSearchAndLocate(nodes);
        }

        PushEffect(effects, effect::SearchFocus{});
        PushEffect(effects, effect::InvalidateWindow{});
    }

    void OnClose(SideEffectList& effects)
    {
        state_->Hide();
        ResetInteractionState(effects);
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

    // 直前の検索がこれより速ければ、入力のたびに待たずに検索する。
    static constexpr auto kImmediateSearchBudget = std::chrono::milliseconds(8);
    // この文書でまだ検索していないとき、待たずに検索する文書の大きさ。
    static constexpr size_t kImmediateSearchMaxBytes = 1024 * 1024;

    void OnTextChanged(SideEffectList& effects, std::wstring_view text, const std::pmr::vector<Node>& nodes, size_t doc_bytes)
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
        PushEffect(effects, effect::KillTimer{ app_timer::Id::SEARCH_DEBOUNCE });

        if (text.empty()) {
            state_->ExecuteSearch(nodes);
            PushEffect(effects, effect::InvalidateWindow{});
            return;
        }

        // 検索の速さは文書の大きさだけでなくマッチ数でも変わるので、直前の検索にかかった時間で決める。
        // 入力で絞り込むほどマッチは減るので、直前の時間はおおむね上限になる。
        const bool immediate = last_search_time_ ? *last_search_time_ <= kImmediateSearchBudget : doc_bytes <= kImmediateSearchMaxBytes;
        if (immediate) {
            RunSearchAndLocate(nodes, true);
            PushEffect(effects, effect::InvalidateWindow{});
            return;
        }

        // 重い文書: デバウンスで連続入力中の再検索を抑制
        PushEffect(effects, effect::InvalidateWindow{});
        PushEffect(effects, effect::SetTimer{ app_timer::Id::SEARCH_DEBOUNCE, app_timer::SEARCH_DEBOUNCE_MS });
    }

    void OnToggleCaseSensitive(SideEffectList& effects, const std::pmr::vector<Node>& nodes)
    {
        state_->ToggleCaseSensitive();
        if (!state_->GetQuery().empty()) {
            RunSearchAndLocate(nodes);
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

    void OnDebounceTimer(SideEffectList& effects, const std::pmr::vector<Node>& nodes)
    {
        PushEffect(effects, effect::KillTimer{ app_timer::Id::SEARCH_DEBOUNCE });
        RunSearchAndLocate(nodes, true);
        PushEffect(effects, effect::InvalidateWindow{});
    }

    void RunSearchAndLocate(const std::pmr::vector<Node>& nodes, bool scroll_to_match = false)
    {
        const auto start = std::chrono::steady_clock::now();
        state_->ExecuteSearch(nodes);
        last_search_time_ = std::chrono::steady_clock::now() - start;
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

        // 未計測や evict 済みのノードは text_layout が無く、行の Y を引けずにノード先頭へ寄ってしまう。
        cb_.ensure_node_layout(match.node_index);
        const auto& entry = (*cache_)[match.node_index];
        // Why: ブロック先頭/行先頭に丸めると、長い段落内の複数マッチ間で同じ Y に集約され
        //      「次へ」を押してもスクロールしない。start_w で行単位の Y を出す。
        const auto [match_y, match_h] = entry.GetMatchYRange(match.table_row, match.table_col, match.start_w, cache_->Top(static_cast<size_t>(match.node_index)));
        const float visible_height = cb_.get_md_pane_height() - (state_->IsVisible() ? SEARCH_BAR_HEIGHT : 0.0f);
        const float scroll_y = viewport_->GetScrollY();
        const float effective_bottom = scroll_y + visible_height;

        if (match_y < scroll_y || match_y + match_h > effective_bottom) {
            viewport_->ScrollTo(match_y - visible_height / 3.0f);
        }
    }

    void Reset(SideEffectList& effects)
    {
        state_->Reset();
        query_wide_.clear();
        last_search_time_.reset();
        ResetInteractionState(effects);
        ResetCaretAndSelection();
    }


    SearchBarRenderState BuildRenderState() const
    {
        SearchBarRenderState sb;
        sb.visible = state_->IsVisible();
        sb.query = GetQueryWide();
        sb.current_match = state_->GetCurrentMatchIndex();
        sb.total_matches = state_->GetMatchCount();
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

    void ResetInteractionState(SideEffectList& effects)
    {
        ime_composition_.clear();
        PushEffect(effects, effect::KillTimer{ app_timer::Id::SEARCH_DEBOUNCE });
    }

    SearchState* state_ = nullptr;
    ViewportManager* viewport_ = nullptr;
    LayoutCache* cache_ = nullptr;
    Cb cb_{};

    int caret_pos_ = -1;
    int selection_start_ = -1;
    std::pmr::wstring ime_composition_;
    std::pmr::wstring query_wide_;
    // 直前の RunSearchAndLocate の検索時間。文書を替えると測り直す。
    std::optional<std::chrono::steady_clock::duration> last_search_time_;
};
