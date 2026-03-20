#pragma once
#include "types.h"
#include "toc.h"
#include <string>
#include <vector>

class Document {
public:
    Document() = default;

    // ファクトリ
    static Document FromMarkdown(const std::string& utf8, std::wstring path);

    // アクセサ
    const std::vector<Node>& GetNodes() const noexcept { return nodes_; }
    std::vector<Node>& GetNodesMut() noexcept { return nodes_; }
    const std::wstring& GetFilePath() const noexcept { return file_path_; }
    const TableOfContents& GetToc() const noexcept { return toc_; }
    bool IsEmpty() const noexcept { return nodes_.empty(); }
    std::wstring GetDirectory() const;

    // ファイルパス設定（LoadMarkdownFile で使用）
    void SetFilePath(const std::wstring& path) { file_path_ = path; }

    // 内容の差し替え（再パース時）
    void ReplaceContent(std::vector<Node> new_nodes);

    // Markdown文字列から内容を再パース（パスは保持）
    void ReplaceFromMarkdown(const std::string& utf8);

private:
    std::vector<Node> nodes_;
    std::wstring file_path_;
    TableOfContents toc_;
};
