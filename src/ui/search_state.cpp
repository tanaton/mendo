#include "search_state.h"
#include "ascii_util.h"
#include "doc_dwrite_bridge.h"
#include "document_utils.h"
#include <algorithm>
#include <limits>

namespace {
// ビットマップ描画ノード (画像 / ダイアグラムコードブロック) はテキストハイライト不可のため検索対象外。
bool IsNonSearchableDrawNode(const Node& node) noexcept
{
    return node.type == NodeType::Image || IsDiagramCodeBlock(node);
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
        else if (const auto& text = node.GetText(); !text.empty()) {
            if (IsNonSearchableDrawNode(node)) {
                continue;
            }
            FindTextMatches(text, query, fold, i);
        }
    }
}

static size_t FindNext(std::string_view text, std::string_view query, bool fold, size_t pos) noexcept
{
    return fold ? ascii_util::FindAsciiCaseInsensitive(text, query, pos) : ascii_util::Find(text, query, pos);
}

void SearchState::FindTextMatches(std::string_view text, std::string_view query, bool fold, int node_index)
{
    const uint32_t query_len = static_cast<uint32_t>(query.size());
    // UTF-16 位置は最初のヒットから前方へ数えるだけにし、ノード全文の UTF-16 化を避ける。
    mendo::Utf16OffsetCursor cursor{ text };
    size_t pos = 0;
    while (matches_.size() < MAX_MATCHES && (pos = FindNext(text, query, fold, pos)) != ascii_util::npos) {
        const auto byte_start = static_cast<uint32_t>(pos);
        const uint32_t w_start = cursor.WideAt(byte_start);
        const uint32_t w_end = cursor.WideAt(byte_start + query_len);
        matches_.emplace_back(SearchMatch{
            .node_index = node_index,
            .start = byte_start,
            .length = query_len,
            .start_w = w_start,
            .length_w = w_end - w_start,
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
        const uint32_t w_start = cursor->WideAt(local);
        const uint32_t w_end = cursor->WideAt(local + query_len);
        matches_.emplace_back(SearchMatch{
            .node_index = node_index,
            .start = local,
            .length = query_len,
            .table_row = static_cast<int>(cell / col_count),
            .table_col = static_cast<int>(cell % col_count),
            .start_w = w_start,
            .length_w = w_end - w_start,
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

    // matches_ は node_index 昇順、同一ノード内では start 昇順、同一テーブル内では
    // (row, col, start) 昇順で追加される。GetMatchYRange も同じ順序で単調非減少となるため
    // 二分探索が使える。
    const auto it = std::ranges::partition_point(matches_, [&](const SearchMatch& m) noexcept {
        if (m.node_index >= static_cast<int>(cache.size())) {
            // cache 未同期の過渡状態では範囲外マッチを末尾扱い (false) にして
            // partition_point の単調性 (true→false) を保ち、current_match を誤らせない。
            return false;
        }
        const auto& e = cache[m.node_index];
        const auto [y, h] = e.GetMatchYRange(m.table_row, m.table_col, m.start_w, cache.Top(static_cast<size_t>(m.node_index)));
        (void)h;
        return y < scroll_y;
    });
    current_match_ = (it != matches_.end()) ? static_cast<int>(it - matches_.begin()) : 0;
}
