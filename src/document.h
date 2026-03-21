#pragma once
#include "types.h"
#include "toc.h"
#include <string>
#include <string_view>
#include <vector>
#include <memory_resource>

class Document {
public:
    Document() = default;

    // ファクトリ
    static Document FromMarkdown(const std::string& utf8, std::wstring_view path);

    // アクセサ
    const std::pmr::vector<Node>& GetNodes() const noexcept { return nodes_; }
    std::pmr::vector<Node>& GetNodesMut() noexcept { return nodes_; }
    const std::pmr::wstring& GetFilePath() const noexcept { return file_path_; }
    const TableOfContents& GetToc() const noexcept { return toc_; }
    bool IsEmpty() const noexcept { return nodes_.empty(); }
    std::wstring GetDirectory() const;

    // ファイルパス設定（LoadMarkdownFile で使用）
    void SetFilePath(std::wstring_view path) { file_path_ = path; }

    // 内容の差し替え（再パース時）
    void ReplaceContent(std::pmr::vector<Node> new_nodes);

    // Markdown文字列から内容を再パース（パスは保持）
    void ReplaceFromMarkdown(const std::string& utf8);

private:
    std::pmr::vector<Node> nodes_;
    std::pmr::wstring file_path_;
    TableOfContents toc_;
};
