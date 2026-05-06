#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include <string>
#include <vector>
#include <memory_resource>
#include <unordered_map>

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

// 検索状態の管理（Win32非依存）
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
    constexpr uint32_t GetGeneration() const noexcept
    {
        return generation_;
    }

    // ドキュメントが切り替わった/構造が変わったときに呼ぶ。
    // 次回 ExecuteSearch 時に lowercase キャッシュが再生成される。
    void InvalidateLowercaseCache() noexcept
    {
        lower_cache_.buffer.clear();
        lower_cache_.offsets.clear();
        lower_cache_.tables.clear();
        cached_nodes_ptr_ = nullptr;
        cached_node_count_ = 0;
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
    constexpr void SetCaseSensitive(bool v) noexcept
    {
        case_sensitive_ = v;
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
    bool NextMatch() noexcept; // ラップしたらtrueを返す
    bool PrevMatch() noexcept; // ラップしたらtrueを返す
    void SetCurrentMatchNear(float scroll_y, const LayoutCache& cache) noexcept;

private:
    // search_text は ascii_util::Find で走査する対象 (lowercase キャッシュ or 元 UTF-8)。
    // utf16_text は UTF-16 オフセット算出に使う元の UTF-8 (= ノード/セルテキスト)。
    // 両者は同一バイト長で位置対応が一致するが、ASCII lowercase はインプレース変換可能なので
    // start オフセットを共有できる。
    void FindMatches(std::string_view search_text, std::string_view utf16_text,
                     const std::pmr::string& lower_query, int node_index,
                     int table_row = -1, int table_col = -1);
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
    // メタオーバーヘッドを抑えるため、全ノードの lower text を 1 本の連続バッファに
    // 詰め、各ノードのスライスを offsets で参照する。
    // Why: 旧実装では LowercaseEntry を vector<vector<vector<pmr::wstring>>> で
    // 持っていたため、1 万ノードで pmr::wstring/vector メタだけで ~500KB 死蔵していた。
    // 連続バッファ化で metadata は offsets の 4B/ノード のみとなり、
    // CPU キャッシュ効率も向上する。
    struct LowercaseTable {
        // NodeTableData::concat_text を bulk lowercase 化したもの。区切り '\t'/'\n' も含む。
        std::pmr::string buffer;
        // NodeTableData::cell_text_starts のコピー (size = row_count * col_count + 1)。
        std::pmr::vector<uint32_t> offsets;
        uint16_t col_count = 0;
    };
    struct LowercaseCache {
        std::pmr::string buffer;                             // 全ノードの lower text を連結
        std::pmr::vector<uint32_t> offsets;                  // size = node_count + 1（末尾 sentinel）
        std::pmr::unordered_map<int, LowercaseTable> tables; // テーブルノードのみ確保

        std::string_view GetText(int node_index) const noexcept
        {
            const uint32_t b = offsets[node_index];
            const uint32_t e = offsets[node_index + 1];
            return std::string_view{ buffer.data() + b, e - b };
        }
        std::string_view GetCell(int node_index, int row, int col) const noexcept
        {
            const auto it = tables.find(node_index);
            if (it == tables.end()) {
                return {};
            }
            const auto& t = it->second;
            const size_t idx = static_cast<size_t>(row) * t.col_count + static_cast<size_t>(col);
            const uint32_t b = t.offsets[idx];
            const uint32_t e = t.offsets[idx + 1];
            const auto len = CellLengthFromOffsets(b, e, static_cast<uint32_t>(t.buffer.size()));
            return std::string_view{ t.buffer.data() + b, len };
        }
    };

    std::pmr::string query_;
    std::pmr::vector<SearchMatch> matches_;
    LowercaseCache lower_cache_;
    const Node* cached_nodes_ptr_ = nullptr;
    size_t cached_node_count_ = 0;
    int current_match_ = -1;
    uint32_t generation_ = 1;
    bool visible_ = false;
    bool case_sensitive_ = false;
    bool highlight_enabled_ = true;
    bool matches_truncated_ = false; // マッチ数が上限に達した場合true
};
