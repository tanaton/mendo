#pragma once
#include "search_state.h"
#include "ui_constants.h"
#include <functional>
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
// Win32依存の操作はコールバック経由でAppに委譲する。
class SearchBarController {
public:
    // Win32操作をAppから注入するコールバック群
    struct Callbacks {
        std::move_only_function<void()> invalidate;                  // ウィンドウ全体の再描画
        std::move_only_function<void()> invalidate_search_bar;       // 検索バー領域のみ再描画
        std::move_only_function<void(UINT_PTR, UINT)> set_timer;     // SetTimer(id, ms)
        std::move_only_function<void(UINT_PTR)> kill_timer;          // KillTimer(id)
        std::move_only_function<void()> focus_select_all;            // 検索テキスト全選択でフォーカス
        std::move_only_function<void(int)> focus_set_caret;          // キャレット位置指定でフォーカス
        std::move_only_function<void(int, int)> focus_set_selection; // anchor,caret指定でフォーカス
        std::move_only_function<void()> unfocus;                     // フォーカス解除
        std::move_only_function<float()> get_md_pane_height;         // Markdownペイン高さ取得
        std::move_only_function<void(float)> on_scroll_changed;      // スクロール変更後処理(md_pane_height)
    };

    // タイマーID（App::HandleTimerでのルーティング用）
    static constexpr UINT_PTR TIMER_CARET = 7;
    static constexpr UINT_PTR TIMER_DEBOUNCE = 9;

    SearchBarController() = default;
    void Init(SearchState& state, ViewportManager& viewport, LayoutCache& cache, Callbacks cb);

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

private:
    void RestartCaretBlink();

    SearchState* state_ = nullptr;
    ViewportManager* viewport_ = nullptr;
    LayoutCache* cache_ = nullptr;
    Callbacks cb_;

    SearchBarHitZone hover_ = SearchBarHitZone::None;
    bool caret_visible_ = false;
    bool has_focus_ = false;
    int caret_pos_ = -1;
    int selection_start_ = -1;
    bool dragging_ = false;
    int drag_anchor_ = 0;
    std::pmr::wstring ime_composition_;
};
