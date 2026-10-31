#pragma once
#include "document_types.h"
#include "document_utils.h"
#include "layout_cache.h"
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// 検索マッチの位置情報。start_w / length_w はノード (表ならセル) 内の UTF-16 code unit 位置で、
// IDWriteTextLayout の HitTestTextRange / HitTestTextPosition にそのまま渡せる。
// パーサが区切ったノードの継ぎ目 (改行を挟まない) をまたぐマッチは前のノードに置き、length_w はそのノードの末尾まで、
// 続きのノードの先頭からの長さを next_length_w に持つ。
struct SearchMatch {
    int node_index;
    int table_row = -1; // テーブルセル用（-1なら通常ノード）
    int table_col = -1;
    uint32_t start_w = 0;
    uint32_t length_w = 0;
    uint32_t next_length_w = 0;
};

// matches[first] がノード node の最初のマッチ (無ければ node より後ろの最初のマッチ) のとき、
// 前のノードのマッチが node の先頭へはみ出した長さ。
constexpr uint32_t SpillLengthInto(std::span<const SearchMatch> matches, size_t first, int node) noexcept
{
    return first > 0 && matches[first - 1].node_index + 1 == node ? matches[first - 1].next_length_w : 0;
}

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
        std::vector<SearchMatch>().swap(matches_);
        std::vector<SearchMatch>().swap(pending_);
        std::vector<int>().swap(candidate_nodes_);
    }

    // マッチ列が変わるたびに進む世代カウンタ。
    // 描画側が検索ハイライト矩形のキャッシュ有効性判定に使う。キャッシュの世代の初期値 0 と
    // 常に不一致になるよう 1 からカウントし始める。
    constexpr uint32_t GetGeneration() const noexcept
    {
        return generation_;
    }

    constexpr const std::vector<SearchMatch>& GetMatches() const noexcept
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

    // 大きい文書でも入力や描画を止めないよう、検索を時間で区切って進める。
    // BeginSearch で始め、ContinueSearch で deadline まで (少なくとも 1 ノード) 進める。終わったら true。
    // query は Document テキストと比べる UTF-8。空なら結果を消す。
    // 途中のマッチは見せず、終わるまで前回の結果 (件数・ハイライト) を見せ続け、終わったら差し替える。
    // 途中経過を見せると、保存のたびの再検索で 0 件・赤背景になりハイライトが消えるちらつきが出る (bd237563)。
    // 同期に戻すと大きい文書で UI が止まるので、区切ったまま見せ方で解決する。
    // 前回の検索を調べ切っていて、クエリを後ろへ伸ばしただけなら、前回マッチのあったノードだけを調べる
    // (新しいクエリが現れるノードには前回のクエリも現れる)。文書を差し替えたら Hide するか、
    // KeepMatchesOfUnchangedNodes の後に同じクエリで調べ直すこと。
    void BeginSearch(std::string_view query);
    bool ContinueSearch(const std::vector<Node>& nodes, std::chrono::steady_clock::time_point deadline);
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
    // query は case-insensitive (fold) 時は ASCII 小文字化済み。text の from バイト目から探す。
    // next は text に改行を挟まずつながる続きのノードのテキスト (無ければ空) で、継ぎ目をまたぐマッチも拾う。
    // 戻り値はまたいだマッチが next の先頭から使ったバイト数。next はそこから探す。
    size_t FindTextMatches(std::string_view text, std::string_view next, size_t from, std::string_view query, bool fold, int node_index);
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
    // 0 はキャッシュの未初期化センチネルなので、32bit ラップアラウンドで 0 に
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
        std::string text;
        bool fold = false;
    };

    QueryKey active_;
    bool searching_ = false;
    // 進行中 (または直前) の検索が調べるノード。narrowing_ なら candidate_nodes_ の番号、でなければ全ノード。
    bool narrowing_ = false;
    // 継ぎ目をまたいだマッチが使った、ノード skip_node_ の先頭のバイト数。
    int skip_node_ = -1;
    size_t skip_bytes_ = 0;
    std::vector<int> candidate_nodes_;
    size_t next_node_ = 0;
    // matches_ を出した検索。completed_ なら matches_ はその全マッチ (上限で打ち切っていない)。
    QueryKey matched_;
    bool completed_ = false;
    bool has_result_ = false;
    // 見せている結果。進行中の検索は pending_ に貯め、終わったら入れ替える。
    std::vector<SearchMatch> matches_;
    std::vector<SearchMatch> pending_;
    int current_match_ = -1;
    uint32_t generation_ = 1;
    bool visible_ = false;
    bool case_sensitive_ = false;
    bool highlight_enabled_ = true;
};
