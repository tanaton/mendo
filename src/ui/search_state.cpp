#include "search_state.h"
#include "ascii_util.h"
#include <algorithm>

void SearchState::SetQuery(mendo::doc_string_view query)
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
    mendo::doc_string lower_query;
    if (!case_sensitive_) {
        lower_query.resize(query_.size());
        ascii_util::ToLower(query_.data(), lower_query.data(), query_.size());
        // ドキュメント単位で lowercase 化結果をキャッシュし、入力1文字ごとの
        // 全文再変換コストを除去する。ドキュメント切替時は自動で再生成される。
        EnsureLowercaseCache(nodes);
    }

    const auto node_count = static_cast<int>(nodes.size());
    for (int i = 0; i < node_count && matches_.size() < MAX_MATCHES; i++) {
        const auto& node = nodes[i];
        if (node.type == NodeType::Table && node.has_table()) {
            const auto* tbl = node.table_data();
            const auto row_count = static_cast<int>(tbl->row_count);
            const auto col_count = static_cast<int>(tbl->col_count);
            for (int r = 0; r < row_count && matches_.size() < MAX_MATCHES; r++) {
                for (int c = 0; c < col_count && matches_.size() < MAX_MATCHES; c++) {
                    const auto cell_text = tbl->GetCellText(r, c);
                    if (!cell_text.empty()) {
                        FindMatches(cell_text, lower_query, i, r, c);
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

    lower_cache_.buffer.clear();
    lower_cache_.offsets.clear();
    lower_cache_.tables.clear();

    // バッファ容量の概算: ノード総文字数。reserve で一度に確保しておくと
    // 連結中の再アロケーションで wstring_view が破綻するのを回避できる。
    size_t total_chars = 0;
    for (const auto& node : nodes) {
        if (node.type != NodeType::Table) {
            total_chars += node.GetText().size();
        }
    }
    lower_cache_.buffer.reserve(total_chars);
    lower_cache_.offsets.reserve(nodes.size() + 1);
    lower_cache_.offsets.push_back(0);

    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        if (node.type == NodeType::Table && node.has_table()) {
            // テーブルノードはノード本体のテキストを持たないので空スライス。
            // セル群は別バッファに連続配置する。
            lower_cache_.offsets.push_back(static_cast<uint32_t>(lower_cache_.buffer.size()));

            const auto* tbl = node.table_data();
            // concat_text を 1 回の bulk ToLower でコピー&小文字化し、cell_text_starts を
            // そのまま offsets として共有する。区切り '\t'/'\n' は ToLower 不変なのでそのまま残せる。
            LowercaseTable table;
            table.col_count = tbl->col_count;
            table.buffer.resize(tbl->concat_text.size());
            ascii_util::ToLower(tbl->concat_text.data(), table.buffer.data(), tbl->concat_text.size());
            table.offsets.assign(tbl->cell_text_starts.begin(), tbl->cell_text_starts.end());
            lower_cache_.tables.emplace(static_cast<int>(i), std::move(table));
        }
        else if (node.type == NodeType::Image || (node.type == NodeType::CodeBlock && IsDiagramLanguage(node.code_language))) {
            // 検索対象外ノードは空スライス
            lower_cache_.offsets.push_back(static_cast<uint32_t>(lower_cache_.buffer.size()));
        }
        else {
            const auto& src = node.GetText();
            const size_t prev = lower_cache_.buffer.size();
            lower_cache_.buffer.resize(prev + src.size());
            ascii_util::ToLower(src.data(), lower_cache_.buffer.data() + prev, src.size());
            lower_cache_.offsets.push_back(static_cast<uint32_t>(lower_cache_.buffer.size()));
        }
    }

    cached_nodes_ptr_ = nodes.data();
    cached_node_count_ = nodes.size();
}

void SearchState::FindMatches(mendo::doc_string_view text, const mendo::doc_string& lower_query, int node_index, int table_row, int table_col)
{
    if (matches_.size() >= MAX_MATCHES) {
        return;
    }
    const uint32_t query_len = static_cast<uint32_t>(query_.size());

    if (case_sensitive_) {
        size_t pos = 0;
        while (matches_.size() < MAX_MATCHES && (pos = ascii_util::Find(text, query_, pos)) != ascii_util::npos) {
            matches_.emplace_back(node_index, static_cast<uint32_t>(pos), query_len, table_row, table_col);
            pos += query_len;
        }
    }
    else {
        // ドキュメント単位でキャッシュした lowercase 文字列を使う。
        // EnsureLowercaseCache が ExecuteSearch 先頭で呼ばれている前提。
        const mendo::doc_string_view lower_text = (table_row < 0) ? lower_cache_.GetText(node_index) : lower_cache_.GetCell(node_index, table_row, table_col);

        size_t pos = 0;
        while (matches_.size() < MAX_MATCHES && (pos = ascii_util::Find(lower_text, lower_query, pos)) != ascii_util::npos) {
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
    // 二分探索が使える。
    const auto it = std::ranges::partition_point(matches_, [&](const SearchMatch& m) noexcept {
        if (m.node_index >= static_cast<int>(cache.size())) {
            return true;
        }
        const auto& e = cache[m.node_index];
        const auto [y, h] = e.GetMatchYRange(m.table_row, m.table_col, m.start, e.text_top);
        (void)h;
        return y < scroll_y;
    });
    current_match_ = (it != matches_.end()) ? static_cast<int>(it - matches_.begin()) : 0;
}
