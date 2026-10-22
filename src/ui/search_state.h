#pragma once
#include "document_types.h"
#include "document_utils.h"
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
    // 検索を使わない間はメモリを空けておくため、結果の確保も返す。
    void Hide() noexcept
    {
        visible_ = false;
        StopSearch();
        ClearMatches();
        std::pmr::vector<SearchMatch>().swap(matches_);
        std::pmr::vector<SearchMatch>().swap(pending_);
        std::pmr::vector<int>().swap(candidate_nodes_);
    }
    void Reset() noexcept
    {
        Hide();
        query_.clear();
    }

    // マッチ列が変わるたびに進む世代カウンタ。
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
    // 見せる結果があるか。初めての検索を調べ終えるまでは無く、0 件を「見つからない」と見せない。
    constexpr bool HasResult() const noexcept
    {
        return has_result_;
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
    // BeginSearch で始め、ContinueSearch で deadline まで (少なくとも 1 ノード) 進める。終わったら true。
    // 確定仕様: 途中のマッチは見せず、終わるまで前回の結果 (件数・ハイライト) を見せ続け、終わったら差し替える。
    //   途中経過を見せると、保存のたびの再検索で 0 件・赤背景になりハイライトが消えるちらつきが出る (bd237563)。
    //   同期に戻すと大きい文書で UI が止まるので、区切ったまま見せ方で解決する。
    // 前回の検索を調べ切っていて、クエリを後ろへ伸ばしただけなら、前回マッチのあったノードだけを調べる
    // (新しいクエリが現れるノードには前回のクエリも現れる)。文書を差し替えたら Reset するか、
    // KeepMatchesOfUnchangedNodes の後に同じクエリで調べ直すこと。
    void BeginSearch();
    bool ContinueSearch(const std::pmr::vector<Node>& nodes, std::chrono::steady_clock::time_point deadline);
    // 検索を止め、前回の結果を残す。
    void StopSearch() noexcept
    {
        searching_ = false;
        pending_.clear();
    }
    // リロードで中身の変わらなかったノードのマッチだけを新しいノード番号で残す。
    // 調べ直しが終わるまで、変わらなかった所のハイライトを消さない。
    void KeepMatchesOfUnchangedNodes(const UnchangedEnds& ends) noexcept;
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

    // 見せている結果と世代カウンタを同時にリセットする。
    void ClearMatches() noexcept
    {
        matches_.clear();
        current_match_ = -1;
        completed_ = false;
        has_result_ = false;
        BumpGeneration();
    }
    // 0 は search_hl_gen の未初期化センチネルなので、32bit ラップアラウンドで 0 に
    // 戻るケースだけはスキップして必ず非ゼロを維持する。
    constexpr void BumpGeneration() noexcept
    {
        if (++generation_ == 0) {
            generation_ = 1;
        }
    }

    static constexpr size_t MAX_MATCHES = 10000;

    // 検索に使うクエリ (fold 時は ASCII 小文字化済み) と比べ方。
    struct QueryKey {
        std::pmr::string text;
        bool fold = false;
    };

    std::pmr::string query_;
    // 進行中の検索。query_ が途中で変わっても検索の中身は変えない。
    QueryKey active_;
    // 進行中 (または直前) の検索が調べるノード。narrowing_ なら candidate_nodes_ の番号、でなければ全ノード。
    bool narrowing_ = false;
    std::pmr::vector<int> candidate_nodes_;
    size_t next_node_ = 0;
    bool searching_ = false;
    // matches_ を出した検索。completed_ なら matches_ はその全マッチ (上限で打ち切っていない)。
    QueryKey matched_;
    bool completed_ = false;
    bool has_result_ = false;
    // 見せている結果。進行中の検索は pending_ に貯め、終わったら入れ替える。
    std::pmr::vector<SearchMatch> matches_;
    std::pmr::vector<SearchMatch> pending_;
    int current_match_ = -1;
    uint32_t generation_ = 1;
    bool visible_ = false;
    bool case_sensitive_ = false;
    bool highlight_enabled_ = true;
};
