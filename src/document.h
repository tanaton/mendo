#pragma once
#include "types.h"
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

    // ファイルパス設定（LoadMarkdownFile で使用）
    constexpr void SetFilePath(std::wstring_view path) { file_path_ = path; }

    // 内容の差し替え（再パース時）
    void ReplaceContent(ParseResult&& result);

    // Markdown文字列から内容を再パース（パスは保持）
    void ReplaceFromMarkdown(std::pmr::string utf8);

    // アンカーIDに一致する見出しノードのインデックスを O(1) で検索する。
    // 見つからない場合は -1 を返す。
    int FindAnchorIndex(std::wstring_view anchor) const;

    // 特殊ノードインデックスの高速アクセス
    constexpr const std::pmr::vector<size_t>& GetImageNodeIndices() const noexcept { return image_node_indices_; }
    constexpr const std::pmr::vector<size_t>& GetMermaidNodeIndices() const noexcept { return mermaid_node_indices_; }

private:
    void BuildHeadingIndices(const std::pmr::vector<size_t>& heading_indices);

    std::pmr::vector<Node> nodes_;
    std::pmr::wstring file_path_;
    std::pmr::string raw_utf8_;
    TableOfContents toc_;
    std::pmr::unordered_map<std::pmr::wstring, int> anchor_index_;
    std::pmr::vector<size_t> image_node_indices_;
    std::pmr::vector<size_t> mermaid_node_indices_;
};
