#include "search_state.h"
#include <algorithm>
#include <cwctype>
#include <iterator>

void SearchState::SetQuery(std::wstring_view query)
{
    query_.assign(query);
}

void SearchState::ExecuteSearch(const std::pmr::vector<Node>& nodes)
{
    matches_.clear();
    current_match_ = -1;
    matches_truncated_ = false;

    if (query_.empty()) {
        return;
    }

    // 大文字小文字無視の場合、クエリの小文字変換をループ外で1回だけ行う
    std::wstring lower_query;
    if (!case_sensitive_) {
        lower_query.reserve(query_.size());
        std::ranges::transform(query_, std::back_inserter(lower_query),
            [](wchar_t ch) static noexcept { return static_cast<wchar_t>(std::towlower(ch)); });
    }

    const auto node_count = static_cast<int>(nodes.size());
    for (int i = 0; i < node_count && matches_.size() < kMaxMatches; i++) {
        const auto& node = nodes[i];
        if (node.type == NodeType::Table && node.has_table()) {
            const auto& rows = node.table_rows();
            const auto row_count = static_cast<int>(rows.size());
            for (int r = 0; r < row_count && matches_.size() < kMaxMatches; r++) {
                const auto& cells = rows[r].cells;
                const auto col_count = static_cast<int>(cells.size());
                for (int c = 0; c < col_count && matches_.size() < kMaxMatches; c++) {
                    if (!cells[c].text.empty()) {
                        FindMatches(std::wstring_view(cells[c].text.data(), cells[c].text.size()),
                            lower_query, i, r, c);
                    }
                }
            }
        }
        else if (const auto& text = node.GetText(); !text.empty()) {
            // ビットマップ描画ノードはテキストハイライト不可のため検索対象外
            if (node.type == NodeType::Image
                || (node.type == NodeType::CodeBlock && node.code_language == SyntaxLanguage::Mermaid)) {
                continue;
            }
            FindMatches(std::wstring_view(text.data(), text.size()),
                lower_query, i);
        }
    }
    matches_truncated_ = (matches_.size() >= kMaxMatches);
}

void SearchState::FindMatches(std::wstring_view text, const std::wstring& lower_query,
    int node_index, int table_row, int table_col)
{
    const uint32_t query_len = static_cast<uint32_t>(query_.size());

    if (case_sensitive_) {
        size_t pos = 0;
        while (matches_.size() < kMaxMatches && (pos = text.find(query_, pos)) != std::wstring_view::npos) {
            matches_.push_back({ node_index, static_cast<uint32_t>(pos), query_len, table_row, table_col });
            pos += query_len;
        }
    }
    else {
        // 割り当てなしの大文字小文字無視検索: std::search + towlower 比較
        auto it = text.begin();
        while (matches_.size() < kMaxMatches && it != text.end()) {
            it = std::search(it, text.end(), lower_query.begin(), lower_query.end(),
                [](wchar_t a, wchar_t b) static noexcept { return static_cast<wchar_t>(std::towlower(a)) == b; });
            if (it == text.end()) {
                break;
            }
            const auto pos = static_cast<uint32_t>(std::distance(text.begin(), it));
            matches_.push_back({ node_index, pos, query_len, table_row, table_col });
            it += query_len;
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

    // matches_ は node_index 昇順 → cache[ni].y_position も単調増加のため二分探索を使用
    int lo = 0, hi = static_cast<int>(matches_.size());
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        const int ni = matches_[mid].node_index;
        if (ni < static_cast<int>(cache.size()) && cache[ni].y_position >= scroll_y) {
            hi = mid;
        }
        else {
            lo = mid + 1;
        }
    }

    current_match_ = (lo < static_cast<int>(matches_.size())) ? lo : 0;
}
