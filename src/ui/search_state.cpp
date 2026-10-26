#include "search_state.h"
#include "ascii_util.h"
#include "doc_dwrite_bridge.h"
#include "document_utils.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <ranges>
#include <utility>

namespace {
size_t FindNext(std::string_view text, std::string_view query, bool fold, size_t pos) noexcept
{
    return fold ? ascii_util::FindAsciiCaseInsensitive(text, query, pos) : ascii_util::Find(text, query, pos);
}
} // namespace

void SearchState::SetQuery(std::string_view query)
{
    query_.assign(query);
}

void SearchState::BeginSearch()
{
    StopSearch();
    if (query_.empty()) {
        ClearMatches();
        return;
    }
    // ASCII 英字を含まないクエリは大文字小文字を無視しても通常一致と結果が同じなので、
    // 比較時の小文字化 (fold) を省く。文書の小文字コピーは持たず走査中に畳み込む。
    const bool fold = !case_sensitive_ && ascii_util::HasAsciiLetter(query_);
    std::pmr::string active = fold ? ToLowerAsciiCopy(query_) : query_;
    // 比べ方 (fold) が同じなら、前回のクエリを伸ばしたクエリは前回マッチのあったノードにしか現れない。
    narrowing_ = completed_ && fold == matched_.fold &&
                 active.size() > matched_.text.size() && active.starts_with(matched_.text);
    candidate_nodes_.clear();
    if (narrowing_) {
        for (const auto& m : matches_) {
            if (candidate_nodes_.empty() || candidate_nodes_.back() != m.node_index) {
                candidate_nodes_.push_back(m.node_index);
            }
        }
    }
    next_node_ = 0;
    skip_node_ = -1;
    active_ = { std::move(active), fold };
    searching_ = true;
}

bool SearchState::ContinueSearch(const std::pmr::vector<Node>& nodes, std::chrono::steady_clock::time_point deadline)
{
    if (!searching_) {
        return true;
    }
    // 時計を毎ノード読むと小さいノードの多い文書で検索より重くなるので、走査量でまとめて確かめる。
    constexpr size_t kBytesPerClockCheck = 256 * 1024;
    constexpr size_t kNodesPerClockCheck = 256;
    const std::string_view query = active_.text;
    const size_t end = narrowing_ ? candidate_nodes_.size() : nodes.size();
    size_t scanned_bytes = 0;
    size_t scanned_nodes = 0;
    while (next_node_ < end && pending_.size() < MAX_MATCHES) {
        const int i = narrowing_ ? candidate_nodes_[next_node_++] : static_cast<int>(next_node_++);
        const auto& node = nodes[i];
        if (node.type == NodeType::Table && node.has_table()) {
            FindTableMatches(*node.table_data(), query, active_.fold, i);
            scanned_bytes += node.table_data()->concat_text.size();
        }
        else if (const auto& text = node.GetText(); !text.empty() && !IsBitmapDrawNode(node)) {
            // 検索欄は 1 行なので、改行を挟む継ぎ目はまたがない。
            const bool joined = static_cast<size_t>(i) + 1 < nodes.size() && JoinsPrevInline(nodes[i + 1]);
            const size_t from = i == skip_node_ ? skip_bytes_ : 0;
            skip_bytes_ = FindTextMatches(text, joined ? nodes[i + 1].GetText() : std::string_view{}, from, query, active_.fold, i);
            skip_node_ = i + 1;
            scanned_bytes += text.size();
        }
        if (scanned_bytes >= kBytesPerClockCheck || ++scanned_nodes >= kNodesPerClockCheck) {
            scanned_bytes = 0;
            scanned_nodes = 0;
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
        }
    }
    const bool truncated = pending_.size() >= MAX_MATCHES;
    searching_ = next_node_ < end && !truncated;
    if (searching_) {
        return false;
    }
    matches_.swap(pending_);
    pending_.clear();
    current_match_ = -1;
    std::swap(matched_, active_);
    completed_ = !truncated;
    has_result_ = true;
    BumpGeneration();
    return true;
}

void SearchState::KeepMatchesOfUnchangedNodes(const UnchangedEnds& ends) noexcept
{
    StopSearch();
    // matches_ は文書順なので、変わった範囲のマッチは 1 つの連続区間になる。
    const auto node_of = [](const SearchMatch& m) static noexcept { return static_cast<size_t>(m.node_index); };
    auto first = std::ranges::lower_bound(matches_, ends.head, {}, node_of);
    // 変わった範囲の先頭のノードへはみ出したマッチも消す。
    if (SpillLengthInto(matches_, static_cast<size_t>(first - matches_.begin()), static_cast<int>(ends.head)) > 0) {
        --first;
    }
    const auto last = std::ranges::lower_bound(first, matches_.end(), ends.OldTailBegin(), {}, node_of);
    const int shift = static_cast<int>(ends.new_count) - static_cast<int>(ends.old_count);
    for (auto& m : std::ranges::subrange(last, matches_.end())) {
        m.node_index += shift;
    }
    const auto first_k = static_cast<int>(first - matches_.begin());
    const auto removed = static_cast<int>(last - first);
    if (current_match_ >= first_k + removed) {
        current_match_ -= removed;
    }
    else if (current_match_ >= first_k) {
        current_match_ = -1;
    }
    matches_.erase(first, last);
    // 全部消えたら、変わった範囲にあったマッチが今も 0 件とは限らないので「見つからない」と見せない。
    has_result_ = has_result_ && !(matches_.empty() && removed > 0);
    // 変わった範囲のマッチが抜けているので、伸ばしたクエリの絞り込みには使えない。
    completed_ = false;
    BumpGeneration();
}

size_t SearchState::FindTextMatches(std::string_view text, std::string_view next, size_t from, std::string_view query, bool fold, int node_index)
{
    const uint32_t query_len = static_cast<uint32_t>(query.size());
    // UTF-16 位置は最初のヒットから前方へ数えるだけにし、ノード全文の UTF-16 化を避ける。
    Utf16OffsetCursor cursor{ text };
    size_t pos = from;
    size_t found;
    while (pending_.size() < MAX_MATCHES && (found = FindNext(text, query, fold, pos)) != ascii_util::npos) {
        const auto w = cursor.WideRange(static_cast<uint32_t>(found), query_len);
        pending_.emplace_back(SearchMatch{
            .node_index = node_index,
            .start_w = w.startPosition,
            .length_w = w.length,
        });
        pos = found + query_len;
    }
    // 継ぎ目をまたぐのは text の末尾 query_len - 1 バイトから始まるマッチだけなので、その前後だけをつないで探す。
    // どれも継ぎ目を含んで重なるので、拾うのは最初の 1 つだけ。
    if (next.empty() || query_len < 2 || pending_.size() >= MAX_MATCHES) {
        return 0;
    }
    const size_t tail_begin = std::max(pos, text.size() - std::min<size_t>(text.size(), query_len - 1));
    if (tail_begin >= text.size()) {
        return 0;
    }
    const std::string_view tail = text.substr(tail_begin);
    std::pmr::string joined{ tail };
    joined.append(next.substr(0, query_len - 1));
    const size_t at = FindNext(joined, query, fold, 0);
    if (at == ascii_util::npos || at >= tail.size()) {
        return 0;
    }
    const auto start = static_cast<uint32_t>(tail_begin + at);
    const auto w = cursor.WideRange(start, static_cast<uint32_t>(text.size()) - start);
    const size_t next_bytes = query_len - (text.size() - start);
    pending_.emplace_back(SearchMatch{
        .node_index = node_index,
        .start_w = w.startPosition,
        .length_w = w.length,
        .next_length_w = Utf16OffsetCursor{ next }.WideAt(static_cast<uint32_t>(next_bytes)),
    });
    return next_bytes;
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
    std::optional<Utf16OffsetCursor> cursor;
    size_t pos = 0;
    while (pending_.size() < MAX_MATCHES && (pos = FindNext(concat, query, fold, pos)) != ascii_util::npos) {
        const size_t cell = tbl.CellIndexAt(static_cast<uint32_t>(pos));
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
        pending_.emplace_back(SearchMatch{
            .node_index = node_index,
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
            const auto* tl = cache[m.node_index].table_layout();
            const auto row = static_cast<size_t>(m.table_row);
            if (tl && row + 1 < tl->row_cum_y.size()) {
                return cache.Top(static_cast<size_t>(m.node_index)) + tl->row_cum_y[row + 1] <= scroll_y;
            }
        }
        return match_y(m) < scroll_y;
    };
    // 境界の行より後のマッチは次の行以降 (行下端 > scroll_y) にあるので、走査はその行内で止まる。
    const auto boundary = std::ranges::partition_point(matches_, above);
    const auto it = std::ranges::find_if_not(boundary, matches_.end(), [&](const SearchMatch& m) noexcept {
        return in_cache(m) && match_y(m) < scroll_y;
    });
    current_match_ = (it != matches_.end()) ? static_cast<int>(it - matches_.begin()) : 0;
}
