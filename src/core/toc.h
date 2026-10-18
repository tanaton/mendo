#pragma once
#include <memory_resource>
#include <utility>
#include <vector>

struct TocEntry {
    int node_index = -1;
    int heading_level = 1;
};

class TableOfContents {
public:
    TableOfContents() = default;
    // entries は文書順の見出し (パーサが見出しの終わりで積む)。
    explicit TableOfContents(std::pmr::vector<TocEntry> entries) noexcept : entries_(std::move(entries)) {}

    constexpr const std::pmr::vector<TocEntry>& GetEntries() const noexcept
    {
        return entries_;
    }

private:
    std::pmr::vector<TocEntry> entries_;
};
