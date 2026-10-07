#pragma once
// パーサ出力 (std::pmr::vector<Node>) の構造不変条件チェッカ。
// レイアウト / 描画 / 選択 / リロードの各層がここに挙げる性質を前提に添字アクセスするため、
// 任意入力のパース結果に対して一括検査できるようにしておく。
#include <gtest/gtest.h>
#include "document_types.h"
#include "reload.h"
#include <algorithm>
#include <cstddef>
#include <format>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace parse_invariants_detail {

class ViolationSink {
public:
    template <class... Args>
    void Add(std::format_string<Args...> fmt, Args&&... args)
    {
        // 1 つの壊れ方で数千件出ても読めないので先頭だけ残す。
        if (messages_.size() < kMaxMessages) {
            messages_.push_back(std::format(fmt, std::forward<Args>(args)...));
        }
        ++count_;
    }
    bool Empty() const noexcept
    {
        return count_ == 0;
    }
    std::string Join() const
    {
        std::string out = std::format("{} violation(s)", count_);
        for (const auto& m : messages_) {
            out += "\n  ";
            out += m;
        }
        return out;
    }

private:
    static constexpr size_t kMaxMessages = 12;
    std::vector<std::string> messages_;
    size_t count_ = 0;
};

// runs は start 昇順・非重複で text_size 以内 (AppendInlineHtml / レイアウトの二分探索の前提)。
inline void CheckRuns(ViolationSink& sink, size_t node_index, std::string_view where,
                      std::span<const TextRun> runs, size_t text_size, size_t link_url_count)
{
    size_t prev_end = 0;
    for (size_t k = 0; k < runs.size(); ++k) {
        const auto& r = runs[k];
        const size_t end = static_cast<size_t>(r.start) + r.length;
        if (r.start < prev_end) {
            sink.Add("node {} {} run {}: start {} < previous end {}", node_index, where, k, r.start, prev_end);
        }
        if (end > text_size) {
            sink.Add("node {} {} run {}: end {} > text size {}", node_index, where, k, end, text_size);
        }
        if (r.link_url_index >= 0 && static_cast<size_t>(r.link_url_index) >= link_url_count) {
            sink.Add("node {} {} run {}: link_url_index {} >= urls {}", node_index, where, k, r.link_url_index, link_url_count);
        }
        prev_end = std::max(prev_end, end);
    }
}

inline void CheckTable(ViolationSink& sink, size_t i, const Node& node)
{
    const auto* tbl = node.table_data();
    if (!tbl) {
        sink.Add("node {}: Table without table_data", i);
        return;
    }
    if (!node.runs.empty()) {
        sink.Add("node {}: Table has node-level runs ({})", i, node.runs.size());
    }
    const size_t cells = static_cast<size_t>(tbl->row_count) * tbl->col_count;
    if (tbl->cell_text_starts.size() != cells + 1 || tbl->cell_run_starts.size() != cells + 1) {
        sink.Add("node {}: offset tables {}/{} != R*C+1 = {}", i, tbl->cell_text_starts.size(), tbl->cell_run_starts.size(), cells + 1);
        return;
    }
    // 末尾オフセットが格納先のサイズとずれていると、以降のセル走査が範囲外を読む。
    if (tbl->cell_text_starts.back() != tbl->concat_text.size()) {
        sink.Add("node {}: cell_text_starts.back() {} != concat size {}", i, tbl->cell_text_starts.back(), tbl->concat_text.size());
        return;
    }
    if (tbl->cell_run_starts.back() != tbl->all_runs.size()) {
        sink.Add("node {}: cell_run_starts.back() {} != all_runs size {}", i, tbl->cell_run_starts.back(), tbl->all_runs.size());
        return;
    }
    if (!std::ranges::is_sorted(tbl->cell_text_starts) || !std::ranges::is_sorted(tbl->cell_run_starts)) {
        sink.Add("node {}: cell offset tables are not non-decreasing", i);
        return;
    }
    if (tbl->is_header_row.size() != tbl->row_count || tbl->aligns.size() != tbl->col_count) {
        sink.Add("node {}: is_header_row {} / aligns {} vs R={} C={}", i, tbl->is_header_row.size(), tbl->aligns.size(), tbl->row_count, tbl->col_count);
    }
    const size_t url_count = node.view_link_urls().size();
    for (size_t r = 0; r < tbl->row_count; ++r) {
        for (size_t c = 0; c < tbl->col_count; ++c) {
            const auto cell_text = tbl->GetCellText(r, c);
            const auto where = std::format("cell({},{})", r, c);
            CheckRuns(sink, i, where, tbl->GetCellRuns(r, c), cell_text.size(), url_count);
        }
    }
}

} // namespace parse_invariants_detail

// raw はパース入力 (= view_ が指すバッファ)。違反が無ければ success。
inline ::testing::AssertionResult ParseInvariantsHold(std::string_view raw, const std::pmr::vector<Node>& nodes)
{
    using namespace parse_invariants_detail;
    ViolationSink sink;
    const char* const base = raw.data();
    size_t prev_offset = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        if (node.HasSourceOffset()) {
            const size_t offset = node.SourceOffsetFrom(base);
            if (node.view_.data() < base || offset >= raw.size()) {
                sink.Add("node {}: source offset outside raw (size {})", i, raw.size());
                continue;
            }
            // view モードは raw_text_ をそのまま表示するので範囲全体が raw 内に収まる必要がある。
            if (node.IsViewMode() && node.view_.size() > raw.size() - offset) {
                sink.Add("node {}: view [{}, +{}) exceeds raw size {}", i, offset, node.view_.size(), raw.size());
            }
            if (offset < prev_offset) {
                sink.Add("node {}: source offset {} < previous {}", i, offset, prev_offset);
            }
            prev_offset = offset;
        }

        if (node.type == NodeType::Table) {
            CheckTable(sink, i, node);
            continue;
        }
        const auto text = node.GetText();
        CheckRuns(sink, i, "text", node.runs, text.size(), node.view_link_urls().size());
        const auto newlines = std::ranges::count(text, '\n');
        if (node.line_count != newlines) {
            sink.Add("node {} (type {}): line_count {} != newlines {}", i, static_cast<int>(node.type), node.line_count, newlines);
        }
        if (node.alert_label_length() > text.size()) {
            sink.Add("node {}: alert_label_length {} > text size {}", i, node.alert_label_length(), text.size());
        }
    }

    // FindNodeBySourceOffset (二分探索) が「offset <= d の最後のノード」の線形定義と一致するか。
    // 全 byte 位置について、線形の答えを掃引で求めて比較する。
    int expected = -1;
    size_t next = 0;
    for (size_t d = 0; d <= raw.size(); ++d) {
        while (next < nodes.size() &&
               (!nodes[next].HasSourceOffset() || nodes[next].SourceOffsetFrom(base) <= d)) {
            if (nodes[next].HasSourceOffset()) {
                expected = static_cast<int>(next);
            }
            ++next;
        }
        const int actual = FindNodeBySourceOffset(nodes, base, d);
        if (actual != expected) {
            sink.Add("FindNodeBySourceOffset({}) = {}, linear = {}", d, actual, expected);
        }
    }

    if (sink.Empty()) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << sink.Join();
}
