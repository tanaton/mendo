#include "search_state.h"
#include "simd_ascii.h"
#include <algorithm>

void SearchState::SetQuery(std::wstring_view query)
{
    query_.assign(query);
}

void SearchState::ExecuteSearch(const std::pmr::vector<Node>& nodes)
{
    ClearMatches();

    if (query_.empty()) {
        return;
    }

    // 大文字小文字無視の場合、クエリの小文字変換をループ外で1回だけ行う
    std::pmr::wstring lower_query;
    if (!case_sensitive_) {
        lower_query.resize(query_.size());
        simd_ascii::ToLower(query_.data(), lower_query.data(), query_.size());
        // ドキュメント単位で lowercase 化結果をキャッシュし、入力1文字ごとの
        // 全文再変換コストを除去する。ドキュメント切替時は自動で再生成される。
        EnsureLowercaseCache(nodes);
    }

    const auto node_count = static_cast<int>(nodes.size());
    for (int i = 0; i < node_count && matches_.size() < MAX_MATCHES; i++) {
        const auto& node = nodes[i];
        if (node.type == NodeType::Table && node.has_table()) {
            const auto& rows = node.table_rows();
            const auto row_count = static_cast<int>(rows.size());
            for (int r = 0; r < row_count && matches_.size() < MAX_MATCHES; r++) {
                const auto& cells = rows[r].cells;
                const auto col_count = static_cast<int>(cells.size());
                for (int c = 0; c < col_count && matches_.size() < MAX_MATCHES; c++) {
                    if (!cells[c].text.empty()) {
                        FindMatches(cells[c].text, lower_query, i, r, c);
                    }
                }
            }
        }
        else if (const auto& text = node.GetText(); !text.empty()) {
            // ビットマップ描画ノードはテキストハイライト不可のため検索対象外
            if (node.type == NodeType::Image ||
                (node.type == NodeType::CodeBlock && IsDiagramLanguage(node.code_language))) {
                continue;
            }
            FindMatches(text, lower_query, i);
        }
    }
    matches_truncated_ = (matches_.size() >= MAX_MATCHES);
}

void SearchState::EnsureLowercaseCache(const std::pmr::vector<Node>& nodes)
{
    if (cached_nodes_ptr_ == nodes.data() && cached_node_count_ == nodes.size()) {
        return;
    }

    lower_cache_.clear();
    lower_cache_.resize(nodes.size());

    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        auto& entry = lower_cache_[i];
        if (node.type == NodeType::Table && node.has_table()) {
            const auto& rows = node.table_rows();
            entry.table_cells.resize(rows.size());
            for (size_t r = 0; r < rows.size(); ++r) {
                const auto& cells = rows[r].cells;
                entry.table_cells[r].resize(cells.size());
                for (size_t c = 0; c < cells.size(); ++c) {
                    const auto& src = cells[c].text;
                    auto& dst = entry.table_cells[r][c];
                    dst.resize(src.size());
                    simd_ascii::ToLower(src.data(), dst.data(), src.size());
                }
            }
        }
        else if (node.type == NodeType::Image ||
            (node.type == NodeType::CodeBlock && IsDiagramLanguage(node.code_language))) {
            // 検索対象外ノードはキャッシュ不要（空のまま）
        }
        else {
            const auto& src = node.GetText();
            auto& dst = entry.text;
            dst.resize(src.size());
            simd_ascii::ToLower(src.data(), dst.data(), src.size());
        }
    }

    cached_nodes_ptr_ = nodes.data();
    cached_node_count_ = nodes.size();
}

void SearchState::FindMatches(std::wstring_view text, const std::pmr::wstring& lower_query, int node_index, int table_row, int table_col)
{
    if (matches_.size() >= MAX_MATCHES) {
        return;
    }
    const uint32_t query_len = static_cast<uint32_t>(query_.size());

    if (case_sensitive_) {
        size_t pos = 0;
        while (matches_.size() < MAX_MATCHES && (pos = simd_ascii::Find(text, query_, pos)) != simd_ascii::npos) {
            matches_.emplace_back(node_index, static_cast<uint32_t>(pos), query_len, table_row, table_col);
            pos += query_len;
        }
    }
    else {
        // ドキュメント単位でキャッシュした lowercase 文字列を使う。
        // EnsureLowercaseCache が ExecuteSearch 先頭で呼ばれている前提。
        const std::wstring_view lower_text = (table_row < 0)
            ? std::wstring_view{ lower_cache_[node_index].text }
            : std::wstring_view{ lower_cache_[node_index].table_cells[table_row][table_col] };

        size_t pos = 0;
        while (matches_.size() < MAX_MATCHES && (pos = simd_ascii::Find(lower_text, lower_query, pos)) != simd_ascii::npos) {
            matches_.emplace_back(node_index, static_cast<uint32_t>(pos), query_len, table_row, table_col);
            pos += query_len;
        }
    }
}

bool SearchState::NextMatch() noexcept
{
    if (matches_.empty()) {
        return false;
    }
    const int next = current_match_ + 1;
    const bool wrapped = next >= static_cast<int>(matches_.size());
    current_match_ = wrapped ? 0 : next;
    return wrapped;
}

bool SearchState::PrevMatch() noexcept
{
    if (matches_.empty()) {
        return false;
    }
    const bool wrapped = current_match_ <= 0;
    if (wrapped) {
        current_match_ = static_cast<int>(matches_.size()) - 1;
    }
    else {
        current_match_--;
    }
    return wrapped;
}

void SearchState::SetCurrentMatchNear(float scroll_y, const LayoutCache& cache) noexcept
{
    if (matches_.empty()) {
        current_match_ = -1;
        return;
    }

    // matches_ は node_index 昇順、同一ノード内では start 昇順、同一テーブル内では
    // (row, col, start) 昇順で追加される。GetMatchYRange も同じ順序で単調非減少となるため
    // 二分探索が使える (issue #97, 検索ジャンプ精度向上)。
    const auto it = std::ranges::partition_point(matches_, [&](const SearchMatch& m) noexcept {
        if (m.node_index >= static_cast<int>(cache.size())) {
            return true;
        }
        const auto [y, h] = cache[m.node_index].GetMatchYRange(m.table_row, m.table_col, m.start);
        (void)h;
        return y < scroll_y;
    });
    current_match_ = (it != matches_.end()) ? static_cast<int>(it - matches_.begin()) : 0;
}
