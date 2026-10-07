#include "search_state.h"
#include "ascii_util.h"
#include "doc_dwrite_bridge.h"
#include "document_utils.h"
#include <algorithm>
#include <limits>
#include <optional>

namespace {
// ビットマップ描画ノード (画像 / ダイアグラムコードブロック) はテキストハイライト不可のため検索対象外。
bool IsNonSearchableDrawNode(const Node& node) noexcept
{
    return node.type == NodeType::Image || IsDiagramCodeBlock(node);
}

size_t FindNext(std::string_view text, std::string_view query, bool fold, size_t pos) noexcept
{
    return fold ? ascii_util::FindAsciiCaseInsensitive(text, query, pos) : ascii_util::Find(text, query, pos);
}
} // namespace

void SearchState::SetQuery(std::string_view query)
{
    query_.assign(query);
}

void SearchState::ExecuteSearch(const std::pmr::vector<Node>& nodes)
{
    ClearMatches();

    if (query_.empty()) {
        return;
    }

    // ASCII 英字を含まないクエリは大文字小文字を無視しても通常一致と結果が同じなので、
    // 比較時の小文字化 (fold) を省く。文書の小文字コピーは持たず走査中に畳み込む。
    const bool fold = !case_sensitive_ && ascii_util::HasAsciiLetter(query_);
    std::pmr::string lower_query;
    if (fold) {
        lower_query = ToLowerAsciiCopy(query_);
    }
    const std::string_view query = fold ? std::string_view(lower_query) : std::string_view(query_);

    const auto node_count = static_cast<int>(nodes.size());
    for (int i = 0; i < node_count && matches_.size() < MAX_MATCHES; i++) {
        const auto& node = nodes[i];
        if (node.type == NodeType::Table && node.has_table()) {
            FindTableMatches(*node.table_data(), query, fold, i);
        }
        else if (const auto& text = node.GetText(); !text.empty() && !IsNonSearchableDrawNode(node)) {
            FindTextMatches(text, query, fold, i);
        }
    }
}

void SearchState::FindTextMatches(std::string_view text, std::string_view query, bool fold, int node_index)
{
    const uint32_t query_len = static_cast<uint32_t>(query.size());
    // UTF-16 位置は最初のヒットから前方へ数えるだけにし、ノード全文の UTF-16 化を避ける。
    mendo::Utf16OffsetCursor cursor{ text };
    size_t pos = 0;
    while (matches_.size() < MAX_MATCHES && (pos = FindNext(text, query, fold, pos)) != ascii_util::npos) {
        const auto byte_start = static_cast<uint32_t>(pos);
        const auto w = cursor.WideRange(byte_start, query_len);
        matches_.emplace_back(SearchMatch{
            .node_index = node_index,
            .start = byte_start,
            .length = query_len,
            .start_w = w.startPosition,
            .length_w = w.length,
        });
        pos += query_len;
    }
}

void SearchState::FindTableMatches(const NodeTableData& tbl, std::string_view query, bool fold, int node_index)
{
    const std::string_view concat = tbl.concat_text;
    const auto& starts = tbl.cell_text_starts;
    const size_t col_count = tbl.col_count;
    if (col_count == 0 || starts.size() < 2) {
        return;
    }
    // SearchMatch::table_row は int。INT_MAX 行を超えるセルは対象外にする (実用上存在しない)。
    const size_t row_limit = std::min<size_t>(tbl.row_count, std::numeric_limits<int>::max());
    const size_t cell_limit = std::min(row_limit * col_count, starts.size() - 1);
    const uint32_t query_len = static_cast<uint32_t>(query.size());
    const auto concat_size = static_cast<uint32_t>(concat.size());

    size_t cursor_cell = std::numeric_limits<size_t>::max();
    std::optional<mendo::Utf16OffsetCursor> cursor;
    size_t pos = 0;
    while (matches_.size() < MAX_MATCHES && (pos = FindNext(concat, query, fold, pos)) != ascii_util::npos) {
        const auto it = std::upper_bound(starts.begin(), starts.begin() + static_cast<std::ptrdiff_t>(cell_limit + 1), static_cast<uint32_t>(pos));
        const size_t cell = static_cast<size_t>(it - starts.begin()) - 1;
        if (cell >= cell_limit) {
            break;
        }
        const uint32_t cell_start = starts[cell];
        const uint32_t cell_len = CellLengthFromOffsets(cell_start, starts[cell + 1], concat_size);
        const auto local = static_cast<uint32_t>(pos) - cell_start;
        if (local + query_len > cell_len) {
            // セル区切り ('\t' / '\n') をまたぐヒット。1 byte 進めて探し直す。
            pos += 1;
            continue;
        }
        if (cell != cursor_cell) {
            cursor.emplace(concat.substr(cell_start, cell_len));
            cursor_cell = cell;
        }
        const auto w = cursor->WideRange(local, query_len);
        matches_.emplace_back(SearchMatch{
            .node_index = node_index,
            .start = local,
            .length = query_len,
            .table_row = static_cast<int>(cell / col_count),
            .table_col = static_cast<int>(cell % col_count),
            .start_w = w.startPosition,
            .length_w = w.length,
        });
        pos += query_len;
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

    // cache 未同期の過渡状態では範囲外マッチを末尾扱いにし、current_match を誤らせない。
    const auto in_cache = [&cache](const SearchMatch& m) noexcept {
        return m.node_index < static_cast<int>(cache.size());
    };
    const auto match_y = [&cache](const SearchMatch& m) noexcept {
        const auto node = static_cast<size_t>(m.node_index);
        return cache[node].GetMatchYRange(m.table_row, m.table_col, m.start_w, cache.Top(node)).first;
    };
    // matches_ は文書順 (テーブル内は (row, col, start) 順)。表の同一行内では左列の折り返し後半が
    // 右列の先頭行より下に来るため match_y は単調でない。行下端は単調なので、表のマッチは
    // 「行ごと scroll_y より上か」で二分探索し、境界の行の中だけ線形に見る。
    const auto above = [&](const SearchMatch& m) noexcept {
        if (!in_cache(m)) {
            return false;
        }
        if (m.table_row >= 0) {
            const auto& entry = cache[m.node_index];
            const auto row = static_cast<size_t>(m.table_row);
            if (entry.has_table_layout() && row + 1 < entry.table_layout->row_cum_y.size()) {
                return cache.Top(static_cast<size_t>(m.node_index)) + entry.table_layout->row_cum_y[row + 1] <= scroll_y;
            }
        }
        return match_y(m) < scroll_y;
    };
    // 境界の行より後のマッチは次の行以降 (行下端 > scroll_y) にあるので、走査はその行内で止まる。
    auto it = std::ranges::partition_point(matches_, above);
    while (it != matches_.end() && in_cache(*it) && match_y(*it) < scroll_y) {
        ++it;
    }
    current_match_ = (it != matches_.end()) ? static_cast<int>(it - matches_.begin()) : 0;
}
