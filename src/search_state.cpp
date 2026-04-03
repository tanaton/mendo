#include "search_state.h"
#include <algorithm>
#include <cwctype>

void SearchState::SetQuery(std::wstring_view query)
{
    query_.assign(query);
}

void SearchState::ExecuteSearch(const std::pmr::vector<Node>& nodes)
{
    matches_.clear();
    current_match_ = -1;

    if (query_.empty()) {
        return;
    }

    // 大文字小文字無視の場合、クエリの小文字変換をループ外で1回だけ行う
    std::wstring lower_query;
    if (!case_sensitive_) {
        lower_query.reserve(query_.size());
        for (wchar_t ch : query_) {
            lower_query.push_back(static_cast<wchar_t>(std::towlower(ch)));
        }
    }

    for (int i = 0; i < static_cast<int>(nodes.size()); i++) {
        const auto& node = nodes[i];
        if (node.type == NodeType::Table && node.has_table()) {
            const auto& rows = node.table_rows();
            for (int r = 0; r < static_cast<int>(rows.size()); r++) {
                const auto& cells = rows[r].cells;
                for (int c = 0; c < static_cast<int>(cells.size()); c++) {
                    if (!cells[c].text.empty()) {
                        FindMatches(std::wstring_view(cells[c].text.data(), cells[c].text.size()),
                            lower_query, i, r, c);
                    }
                }
            }
        }
        else if (!node.text.empty()) {
            // ビットマップ描画ノードはテキストハイライト不可のため検索対象外
            if (node.type == NodeType::Image
                || (node.type == NodeType::CodeBlock && node.code_language == SyntaxLanguage::Mermaid)) {
                continue;
            }
            FindMatches(std::wstring_view(node.text.data(), node.text.size()),
                lower_query, i);
        }
    }
}

void SearchState::FindMatches(std::wstring_view text, const std::wstring& lower_query,
    int node_index, int table_row, int table_col)
{
    const uint32_t query_len = static_cast<uint32_t>(query_.size());

    if (case_sensitive_) {
        size_t pos = 0;
        while ((pos = text.find(query_, pos)) != std::wstring_view::npos) {
            matches_.push_back({ node_index, static_cast<uint32_t>(pos), query_len, table_row, table_col });
            pos += query_len;
        }
    }
    else {
        // 割り当てなしの大文字小文字無視検索: std::search + towlower 比較
        auto it = text.begin();
        while (it != text.end()) {
            it = std::search(it, text.end(), lower_query.begin(), lower_query.end(),
                [](wchar_t a, wchar_t b) { return static_cast<wchar_t>(std::towlower(a)) == b; });
            if (it == text.end()) {
                break;
            }
            const auto pos = static_cast<uint32_t>(std::distance(text.begin(), it));
            matches_.push_back({ node_index, pos, query_len, table_row, table_col });
            it += query_len;
        }
    }
}

bool SearchState::NextMatch()
{
    if (matches_.empty()) {
        return false;
    }
    const int next = current_match_ + 1;
    const bool wrapped = next >= static_cast<int>(matches_.size());
    current_match_ = wrapped ? 0 : next;
    return wrapped;
}

bool SearchState::PrevMatch()
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

void SearchState::SetCurrentMatchNear(float scroll_y, const LayoutCache& cache)
{
    if (matches_.empty()) {
        current_match_ = -1;
        return;
    }

    for (int i = 0; i < static_cast<int>(matches_.size()); i++) {
        const int ni = matches_[i].node_index;
        if (ni < static_cast<int>(cache.size()) && cache[ni].y_position >= scroll_y) {
            current_match_ = i;
            return;
        }
    }

    current_match_ = 0;
}
