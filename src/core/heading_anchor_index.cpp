#include "heading_anchor_index.h"
#include "document_utils.h"
#include <charconv>
#include <iterator>

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
