#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

// 検索マッチの位置情報。
// start / length は UTF-8 byte 単位 (ascii_util::Find の戻り値)。
// start_w / length_w は同じ範囲を UTF-16 code unit で表したもので
// IDWriteTextLayout の HitTestTextRange / HitTestTextPosition にそのまま渡せる。
// 不変条件: 両者は同じノード/セル内の同一範囲を指し、ExecuteSearch で同時に確定される。
struct SearchMatch {
    int node_index;
    uint32_t start;
    uint32_t length;
    int table_row = -1; // テーブルセル用（-1なら通常ノード）
    int table_col = -1;
    uint32_t start_w = 0;
    uint32_t length_w = 0;
};

// 検索状態の管理。start_w / length_w 算出のため doc_dwrite_bridge (UTF-8↔UTF-16) に依存する。
class SearchState {
public:
    constexpr bool IsVisible() const noexcept
    {
        return visible_;
    }
    constexpr void Show() noexcept
    {
        visible_ = true;
    }
    void Hide() noexcept
    {
        visible_ = false;
        ClearMatches();
        searching_ = false;
    }
    void Reset() noexcept
    {
        Hide();
        query_.clear();
    }

    // ExecuteSearch / Hide / Reset のたびにインクリメントされる世代カウンタ。
    // 描画側が検索ハイライト矩形のキャッシュ有効性判定に使う。0 は未初期化を意味するため
    // 1 からカウントし始める（search_hl_gen=0 と常に不一致になる）。
    constexpr uint32_t GetGeneration() const noexcept
    {
        return generation_;
    }

    constexpr const std::pmr::string& GetQuery() const noexcept
    {
        return query_;
    }
    constexpr const std::pmr::vector<SearchMatch>& GetMatches() const noexcept
    {
        return matches_;
    }
    constexpr int GetCurrentMatchIndex() const noexcept
    {
        return current_match_;
    }
    constexpr int GetMatchCount() const noexcept
    {
        return static_cast<int>(matches_.size());
    }
    constexpr bool IsCaseSensitive() const noexcept
    {
        return case_sensitive_;
    }

    constexpr void ToggleCaseSensitive() noexcept
    {
        case_sensitive_ = !case_sensitive_;
    }

    constexpr bool IsHighlightEnabled() const noexcept
    {
        return highlight_enabled_;
    }
    constexpr void ToggleHighlightEnabled() noexcept
    {
        highlight_enabled_ = !highlight_enabled_;
    }

    void SetQuery(std::string_view query);
    void ExecuteSearch(const std::pmr::vector<Node>& nodes);
    // 大きい文書でも入力や描画を止めないよう、検索を時間で区切って進める。
    // BeginSearch でマッチを空にし、ContinueSearch で deadline まで (少なくとも 1 ノード) 進める。
    // マッチは文書順に増える。終わったら true。
    // 前回の検索を調べ切っていて、クエリを後ろへ伸ばしただけなら、前回マッチのあったノードだけを調べる
    // (新しいクエリが現れるノードには前回のクエリも現れる)。文書を差し替えたら Reset するか、
    // 同じクエリで調べ直すこと。
    void BeginSearch();
    bool ContinueSearch(const std::pmr::vector<Node>& nodes, std::chrono::steady_clock::time_point deadline);
    // クエリを変えたときなど、途中までのマッチを残して止める。
    constexpr void StopSearch() noexcept
    {
        searching_ = false;
    }
    constexpr bool IsSearching() const noexcept
    {
        return searching_;
    }
    bool NextMatch() noexcept; // ラップしたらtrueを返す
    bool PrevMatch() noexcept; // ラップしたらtrueを返す
    void SetCurrentMatchNear(float scroll_y, const LayoutCache& cache) noexcept;

private:
    // query は case-insensitive (fold) 時は ASCII 小文字化済み。
    void FindTextMatches(std::string_view text, std::string_view query, bool fold, int node_index);
    // concat_text を 1 回だけ走査し、セル境界をまたぐヒットを除いてセル単位のマッチに振り分ける。
    // セルごとに Find を呼ぶと 1 セル十数バイトでは SIMD ループに入らず呼び出しコストが支配的になる。
    void FindTableMatches(const NodeTableData& tbl, std::string_view query, bool fold, int node_index);

    // マッチ一覧と世代カウンタを同時にリセットする。
    // 0 は search_hl_gen の未初期化センチネルなので、32bit ラップアラウンドで 0 に
    // 戻るケースだけはスキップして必ず非ゼロを維持する。
    void ClearMatches() noexcept
    {
        matches_.clear();
        current_match_ = -1;
        completed_ = false;
        BumpGeneration();
    }
    // 検索の途中でマッチが増えたときも、ハイライトのキャッシュを作り直させる。
    constexpr void BumpGeneration() noexcept
    {
        if (++generation_ == 0) {
            generation_ = 1;
        }
    }

    static constexpr size_t MAX_MATCHES = 10000;

    std::pmr::string query_;
    // 進行中の検索のクエリ (fold 時は ASCII 小文字化済み)。query_ が途中で変わっても検索の中身は変えない。
    std::pmr::string active_query_;
    bool active_fold_ = false;
    // 進行中 (または直前) の検索が調べるノード。narrowing_ なら candidate_nodes_ の番号、でなければ全ノード。
    bool narrowing_ = false;
    std::pmr::vector<int> candidate_nodes_;
    size_t next_node_ = 0;
    bool searching_ = false;
    // 直前の検索が上限で打ち切らずに調べ終わり、matches_ が active_query_ の全マッチになっている。
    bool completed_ = false;
    std::pmr::vector<SearchMatch> matches_;
    int current_match_ = -1;
    uint32_t generation_ = 1;
    bool visible_ = false;
    bool case_sensitive_ = false;
    bool highlight_enabled_ = true;
};
