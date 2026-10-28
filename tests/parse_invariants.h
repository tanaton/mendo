#pragma once
// パーサ出力 (std::vector<Node>) の構造不変条件チェッカ。
// レイアウト / 描画 / 選択 / リロードの各層がここに挙げる性質を前提に添字アクセスするため、
// 任意入力のパース結果に対して一括検査できるようにしておく。
#include <gtest/gtest.h>
#include "document_types.h"
#include "document_utils.h"
#include "test_nodes.h"
#include <algorithm>
#include <cstddef>
#include <format>
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
// 利用側は runs の隙間を既定スタイルとして扱うので、装飾もリンクも無い run と空の run は作らない
// (4096 個の上限を無駄に消費し、レイアウト時に run ごとの UTF-16 位置の数え直しが走る)。
inline void CheckRuns(ViolationSink& sink, size_t node_index, std::string_view where,
                      std::span<const TextRun> runs, size_t text_size, size_t link_url_count)
{
    size_t prev_end = 0;
    for (size_t k = 0; k < runs.size(); ++k) {
        const auto& r = runs[k];
        const size_t end = static_cast<size_t>(r.start) + r.length;
        if (r.length == 0) {
            sink.Add("node {} {} run {}: zero length at {}", node_index, where, k, r.start);
        }
        if (r.raw_flags() == 0 && !r.has_link()) {
            sink.Add("node {} {} run {}: plain run [{}, {})", node_index, where, k, r.start, end);
        }
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
    if (!node.runs().empty()) {
        sink.Add("node {}: Table has node-level runs ({})", i, node.runs().size());
    }
    const size_t cells = static_cast<size_t>(tbl->row_count) * tbl->col_count;
    if (tbl->cell_text_starts.size() != cells + 1) {
        sink.Add("node {}: cell_text_starts {} != R*C+1 = {}", i, tbl->cell_text_starts.size(), cells + 1);
        return;
    }
    // run の無い表は cell_run_starts を持たない。
    if (tbl->all_runs.empty() != tbl->cell_run_starts.empty() ||
        (!tbl->cell_run_starts.empty() && tbl->cell_run_starts.size() != cells + 1)) {
        sink.Add("node {}: cell_run_starts {} (all_runs {}) vs R*C+1 = {}", i, tbl->cell_run_starts.size(), tbl->all_runs.size(), cells + 1);
        return;
    }
    // 末尾オフセットが格納先のサイズとずれていると、以降のセル走査が範囲外を読む。
    if (tbl->cell_text_starts.back() != tbl->concat_text.size()) {
        sink.Add("node {}: cell_text_starts.back() {} != concat size {}", i, tbl->cell_text_starts.back(), tbl->concat_text.size());
        return;
    }
    if (!tbl->cell_run_starts.empty() && tbl->cell_run_starts.back() != tbl->all_runs.size()) {
        sink.Add("node {}: cell_run_starts.back() {} != all_runs size {}", i, tbl->cell_run_starts.back(), tbl->all_runs.size());
        return;
    }
    if (!std::ranges::is_sorted(tbl->cell_text_starts) || !std::ranges::is_sorted(tbl->cell_run_starts)) {
        sink.Add("node {}: cell offset tables are not non-decreasing", i);
        return;
    }
    if (tbl->head_row_count > tbl->row_count || tbl->aligns.size() != tbl->col_count) {
        sink.Add("node {}: head_row_count {} / aligns {} vs R={} C={}", i, tbl->head_row_count, tbl->aligns.size(), tbl->row_count, tbl->col_count);
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

// raw はパース入力 (= ノードのテキストが直接指しうるバッファ)。違反が無ければ success。
inline ::testing::AssertionResult ParseInvariantsHold(std::string_view raw, const std::vector<Node>& nodes)
{
    using namespace parse_invariants_detail;
    ViolationSink sink;
    size_t prev_offset = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        // 原文をそのまま表示するノードは範囲全体が raw 内に収まる必要がある。
        if (TextPointsIntoRaw(node, raw)) {
            const auto start = static_cast<size_t>(node.GetText().data() - raw.data());
            if (node.GetText().size() > raw.size() - start) {
                sink.Add("node {}: text [{}, +{}) exceeds raw size {}", i, start, node.GetText().size(), raw.size());
            }
        }
        if (node.HasSourceOffset()) {
            const size_t offset = node.SourceOffset();
            if (offset >= raw.size()) {
                sink.Add("node {}: source offset {} outside raw (size {})", i, offset, raw.size());
                continue;
            }
            if (offset < prev_offset) {
                sink.Add("node {}: source offset {} < previous {}", i, offset, prev_offset);
            }
            prev_offset = offset;
        }

        // 区切ったノードの並びは前後のフラグが対になり、同じ種別・同じ入れ子に収まり、空のノードを含まない。
        if (node.continues_prev() != (i > 0 && nodes[i - 1].continues_next())) {
            sink.Add("node {}: continues_prev {} does not pair with previous continues_next", i, node.continues_prev());
        }
        if (node.continues_prev()) {
            const auto& prev = nodes[i - 1];
            if (prev.type != node.type || prev.indent_level != node.indent_level || prev.blockquote_group != node.blockquote_group ||
                prev.code_language != node.code_language) {
                sink.Add("node {}: continuation differs from previous node in type or nesting", i);
            }
        }
        if ((node.continues_prev() || node.continues_next()) && !node.HasText()) {
            sink.Add("node {}: empty chunk", i);
        }

        if (node.type == NodeType::Table) {
            CheckTable(sink, i, node);
            continue;
        }
        const auto text = node.GetText();
        CheckRuns(sink, i, "text", node.runs(), text.size(), node.view_link_urls().size());
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
               (!nodes[next].HasSourceOffset() || nodes[next].SourceOffset() <= d)) {
            if (nodes[next].HasSourceOffset()) {
                expected = static_cast<int>(next);
            }
            ++next;
        }
        const int actual = FindNodeBySourceOffset(nodes, d);
        if (actual != expected) {
            sink.Add("FindNodeBySourceOffset({}) = {}, linear = {}", d, actual, expected);
        }
    }

    if (sink.Empty()) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << sink.Join();
}
