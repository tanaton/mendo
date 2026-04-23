#pragma once
#include "document_types.h"
#include "toc.h"
#include "parser.h"
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <memory_resource>

class Document {
public:
    constexpr Document() noexcept = default;

    // ファクトリ
    static Document FromMarkdown(std::pmr::string utf8, std::wstring_view path);

    // アクセサ
    constexpr const std::pmr::vector<Node>& GetNodes() const noexcept { return nodes_; }
    constexpr std::pmr::vector<Node>& GetNodesMut() noexcept { return nodes_; }
    constexpr const std::pmr::wstring& GetFilePath() const noexcept { return file_path_; }
    constexpr const TableOfContents& GetToc() const noexcept { return toc_; }
    constexpr bool IsEmpty() const noexcept { return nodes_.empty(); }
    const std::pmr::string& GetRawUtf8() const noexcept { return raw_utf8_; }
    std::pmr::wstring GetDirectory() const;

    constexpr void SetFilePath(std::wstring_view path) { file_path_ = path; }
    void ReplaceContent(ParseResult&& result);
    void ReplaceFromMarkdown(std::pmr::string utf8);
    int FindAnchorIndex(std::wstring_view anchor) const;
    constexpr const std::pmr::vector<size_t>& GetImageNodeIndices() const noexcept { return image_node_indices_; }
    constexpr const std::pmr::vector<size_t>& GetDiagramNodeIndices() const noexcept { return diagram_node_indices_; }

private:
    void BuildHeadingIndices(const std::pmr::vector<size_t>& heading_indices);

    std::pmr::vector<Node> nodes_;
    std::pmr::wstring file_path_;
    std::pmr::string raw_utf8_;
    TableOfContents toc_;
    std::pmr::unordered_map<std::pmr::wstring, int> anchor_index_;
    std::pmr::vector<size_t> image_node_indices_;
    std::pmr::vector<size_t> diagram_node_indices_;
};
