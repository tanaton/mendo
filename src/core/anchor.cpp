#include "anchor.h"
#include "document_utils.h"
#include <algorithm>
#include <charconv>
#include <format>
#include <iterator>
#include <optional>
#include <system_error>

namespace {

constexpr std::string_view kFootnoteDefAnchorPrefix = "user-content-fn-";
constexpr std::string_view kFootnoteRefAnchorPrefix = "user-content-fnref-";

// s 全体が 1 以上の 10 進数なら値を返す。
std::optional<uint32_t> ParsePositive(std::string_view s) noexcept
{
    uint32_t value = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || ptr != s.data() + s.size() || value == 0) {
        return std::nullopt;
    }
    return value;
}

} // namespace

std::string_view HeadingAnchorIndex::Add(std::string_view heading_text, int node_index)
{
    ++heading_count_;
    GenerateAnchorIdInto(heading_text, base_);
    auto result = entries_.try_emplace(base_, Entry{ node_index });
    if (!result.second) {
        // unordered_map は再ハッシュでも要素の参照が変わらない。
        int& counter = result.first->second.counter;
        do {
            char digits[16];
            const auto end = std::to_chars(digits, std::end(digits), ++counter).ptr;
            candidate_.assign(base_);
            candidate_ += '-';
            candidate_.append(digits, end);
            result = entries_.try_emplace(candidate_, Entry{ node_index });
        } while (!result.second);
    }
    return result.first->first;
}

int HeadingAnchorIndex::Find(std::string_view anchor) const
{
    const auto it = entries_.find(anchor);
    return it != entries_.end() ? it->second.node_index : -1;
}

std::string FootnoteDefUrl(uint32_t id)
{
    return std::format("#{}{}", kFootnoteDefAnchorPrefix, id);
}

std::string FootnoteRefUrl(uint32_t id, uint32_t ref_id)
{
    auto url = std::format("#{}{}", kFootnoteRefAnchorPrefix, id);
    // GitHub と同じく 1 回目の参照は番号を付けない。
    if (ref_id > 1) {
        std::format_to(std::back_inserter(url), "-{}", ref_id);
    }
    return url;
}

int FindFootnoteAnchor(std::string_view anchor, std::span<const int32_t> def_nodes, std::span<const FootnoteRef> refs) noexcept
{
    // "fnref-" は "fn-" で始まらないので判定順は問わない。
    if (anchor.starts_with(kFootnoteDefAnchorPrefix)) {
        const auto id = ParsePositive(anchor.substr(kFootnoteDefAnchorPrefix.size()));
        return (id && *id <= def_nodes.size()) ? def_nodes[*id - 1] : -1;
    }
    if (!anchor.starts_with(kFootnoteRefAnchorPrefix)) {
        return -1;
    }
    const auto rest = anchor.substr(kFootnoteRefAnchorPrefix.size());
    const auto dash = rest.find('-');
    const auto id = ParsePositive(rest.substr(0, dash));
    const auto ref_id = dash == std::string_view::npos ? std::optional<uint32_t>{ 1 } : ParsePositive(rest.substr(dash + 1));
    if (!id || !ref_id) {
        return -1;
    }
    const auto it = std::ranges::find_if(refs, [&](const FootnoteRef& r) noexcept {
        return r.id == *id && r.ref_id == *ref_id;
    });
    return it != refs.end() ? it->node_index : -1;
}
