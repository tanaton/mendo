#pragma once
#include "app_constants.h"
#include "doc_text.h"
#include "search_state.h"
#include "ui_constants.h"
#include <memory_resource>
#include <string>
#include <string_view>
#include <windows.h>

class ViewportManager;
class LayoutCache;
struct SearchBarRenderState;

// 検索バーのUI状態管理。
// SearchState（ドメインロジック）を参照で保持し、検索バー固有の
// UIステート（フォーカス、キャレット、ドラッグ選択、ホバー）を管理する。
// Win32依存の操作は Cb 経由でAppに委譲する。
template <class Cb>
class SearchBarControllerT {
public:
    SearchBarControllerT() = default;
    void Init(SearchState& state, ViewportManager& viewport, LayoutCache& cache, Cb cb);

    void OnOpen(const std::pmr::vector<Node>& nodes);
    void OnClose();
    void OnNext();
    void OnPrev();
    void OnTextChanged(std::wstring_view text, const std::pmr::vector<Node>& nodes);
    void OnToggleCaseSensitive(const std::pmr::vector<Node>& nodes);
    void OnToggleHighlight();
    void SetSelection(int sel_start, int sel_end) noexcept;
    void SetImeComposition(std::wstring_view comp);

    void OnCaretBlinkTimer();
    void OnDebounceTimer(const std::pmr::vector<Node>& nodes);

    void RunSearchAndLocate(const std::pmr::vector<Node>& nodes, bool scroll_to_match = false);
    void ScrollToCurrentMatch();

    // ファイル切替時のリセット。
    void Reset();

    bool IsDragging() const noexcept
    {
        return dragging_;
    }
    void StartDrag(int anchor_pos) noexcept;
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

    void UpdateHoverFromZone(SearchBarHitZone zone);
    SearchBarHitZone GetHover() const noexcept
    {
        return hover_;
    }
    void ResetHover() noexcept
    {
        hover_ = SearchBarHitZone::None;
    }

    SearchBarRenderState BuildRenderState() const;

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
    const std::pmr::wstring& GetQueryWide() const;

private:
    void RestartCaretBlink();

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
    // SearchBarRenderState::query (wstring_view) 用に string→wstring 変換結果を保持。
    // BuildRenderState() の戻り値内 view が SearchBarController の生存中 valid であることを保証する。
    // query_wide_cache_key_ は invalidation キーとしての「前回入力 UTF-8 そのもの」。
    mutable std::pmr::wstring query_wide_cache_;
    mutable std::pmr::string query_wide_cache_key_;
};
