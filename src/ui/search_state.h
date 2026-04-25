#pragma once
#include "document_types.h"
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
        ClearMatches();
        InvalidateLowercaseCache();
    }
    void Reset() noexcept
    {
        ClearMatches();
        query_.clear();
        InvalidateLowercaseCache();
    }

    // ExecuteSearch / Hide / Reset のたびにインクリメントされる世代カウンタ。
    // 描画側が検索ハイライト矩形のキャッシュ有効性判定に使う。0 は未初期化を意味するため
    // 1 からカウントし始める（search_hl_gen=0 と常に不一致になる）。
    uint32_t GetGeneration() const noexcept { return generation_; }

    // ドキュメントが切り替わった/構造が変わったときに呼ぶ。
    // 次回 ExecuteSearch 時に lowercase キャッシュが再生成される。
    void InvalidateLowercaseCache() noexcept
    {
        lower_cache_.clear();
        cached_nodes_ptr_ = nullptr;
        cached_node_count_ = 0;
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
    void SetCurrentMatchNear(float scroll_y, const LayoutCache& cache) noexcept;

private:
    void FindMatches(std::wstring_view text, const std::pmr::wstring& lower_query, int node_index, int table_row = -1, int table_col = -1);
    void EnsureLowercaseCache(const std::pmr::vector<Node>& nodes);

    // マッチ一覧と関連する世代カウンタ・トランケーションフラグを同時にリセットする。
    // これらは常に一緒に更新しないとキャッシュ有効性判定が壊れるためヘルパに集約している。
    // 0 は search_hl_gen の未初期化センチネルなので、32bit ラップアラウンドで 0 に
    // 戻るケースだけはスキップして必ず非ゼロを維持する。
    void ClearMatches() noexcept
    {
        matches_.clear();
        current_match_ = -1;
        matches_truncated_ = false;
        if (++generation_ == 0) {
            generation_ = 1;
        }
    }

    static constexpr size_t MAX_MATCHES = 10000;

    // 大文字小文字無視検索の lowercase キャッシュ。
    // ノード毎に text を lowercase 化した結果を保持する。テーブルノードは
    // rows[r][c] の2次元配列に格納する（空のノードは空文字列）。
    struct LowercaseEntry {
        std::pmr::wstring text;
        std::pmr::vector<std::pmr::vector<std::pmr::wstring>> table_cells;
    };

    std::pmr::wstring query_;
    std::pmr::vector<SearchMatch> matches_;
    std::pmr::vector<LowercaseEntry> lower_cache_;
    const Node* cached_nodes_ptr_ = nullptr;
    size_t cached_node_count_ = 0;
    int current_match_ = -1;
    uint32_t generation_ = 1;
    bool visible_ = false;
    bool case_sensitive_ = false;
    bool highlight_enabled_ = true;
    bool matches_truncated_ = false; // マッチ数が上限に達した場合true
};
