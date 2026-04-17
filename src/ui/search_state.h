#pragma once
#include "types.h"
#include "layout_cache.h"
#include <string>
#include <vector>
#include <memory_resource>

// 検索マッチの位置情報
struct SearchMatch {
    int node_index;
    uint32_t start;       // node.text（またはcell.text）内の文字オフセット
    uint32_t length;
    int table_row = -1;   // テーブルセル用（-1なら通常ノード）
    int table_col = -1;
};

// 検索状態の管理（Win32非依存）
class SearchState {
public:
    bool IsVisible() const noexcept { return visible_; }
    void Show() noexcept { visible_ = true; }
    void Hide() noexcept
    {
        visible_ = false;
        current_match_ = -1;
        matches_.clear();
        matches_truncated_ = false;
        lower_text_buf_ = {};
    }
    void Reset() noexcept
    {
        current_match_ = -1;
        matches_.clear();
        query_.clear();
        matches_truncated_ = false;
        lower_text_buf_ = {};
    }

    const std::pmr::wstring& GetQuery() const noexcept { return query_; }
    const std::pmr::vector<SearchMatch>& GetMatches() const noexcept { return matches_; }
    int GetCurrentMatchIndex() const noexcept { return current_match_; }
    int GetMatchCount() const noexcept { return static_cast<int>(matches_.size()); }
    // 大文字小文字の区別
    bool IsCaseSensitive() const noexcept { return case_sensitive_; }
    void SetCaseSensitive(bool v) noexcept { case_sensitive_ = v; }
    void ToggleCaseSensitive() noexcept { case_sensitive_ = !case_sensitive_; }

    // ハイライト表示のON/OFF
    bool IsHighlightEnabled() const noexcept { return highlight_enabled_; }
    void ToggleHighlightEnabled() noexcept { highlight_enabled_ = !highlight_enabled_; }

    void SetQuery(std::wstring_view query);
    void ExecuteSearch(const std::pmr::vector<Node>& nodes);
    bool NextMatch() noexcept;   // ラップしたらtrueを返す
    bool PrevMatch() noexcept;   // ラップしたらtrueを返す

    // スクロール位置に最も近いマッチを現在マッチとして選択
    void SetCurrentMatchNear(float scroll_y, const LayoutCache& cache) noexcept;

private:
    void FindMatches(std::wstring_view text, const std::pmr::wstring& lower_query, int node_index, int table_row = -1, int table_col = -1);

    static constexpr size_t MAX_MATCHES = 10000;

    std::pmr::wstring query_;
    std::pmr::vector<SearchMatch> matches_;
    std::pmr::wstring lower_text_buf_; // 大文字小文字無視検索用の再利用可能バッファ
    int current_match_ = -1;
    bool visible_ = false;
    bool case_sensitive_ = false;
    bool highlight_enabled_ = true;
    bool matches_truncated_ = false; // マッチ数が上限に達した場合true
};
