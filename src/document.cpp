#include "document.h"
#include "document_utils.h"
#include "parser.h"
#include <filesystem>

Document Document::FromMarkdown(const std::pmr::string& utf8, std::wstring_view path)
{
    Document doc;
    doc.file_path_ = path;
    doc.nodes_ = ParseMarkdown(utf8);
    doc.toc_.BuildFromNodes(doc.nodes_);
    doc.BuildAnchorIndex();
    return doc;
}

std::pmr::wstring Document::GetDirectory() const
{
    auto dir = std::filesystem::path(file_path_.c_str()).parent_path();
    if (!dir.empty()) {
        return std::pmr::wstring{ std::wstring_view{dir.native()} };
    }
    return {};
}

void Document::ReplaceContent(std::pmr::vector<Node> new_nodes)
{
    nodes_ = std::move(new_nodes);
    toc_.BuildFromNodes(nodes_);
    BuildAnchorIndex();
}

void Document::ReplaceFromMarkdown(const std::pmr::string& utf8)
{
    ReplaceContent(ParseMarkdown(utf8));
}

int Document::FindAnchorIndex(std::wstring_view anchor) const
{
    if (anchor.empty()) {
        return -1;
    }
    std::pmr::wstring target = ToLowerAscii(anchor);
    auto it = anchor_index_.find(target);
    return (it != anchor_index_.end()) ? it->second : -1;
}

void Document::BuildAnchorIndex()
{
    anchor_index_.clear();
    for (int i = 0; i < static_cast<int>(nodes_.size()); i++) {
        const auto& node = nodes_[i];
        if (node.type == NodeType::Heading && !node.anchor_id.empty()) {
            anchor_index_.emplace(node.anchor_id, i);
        }
    }
}
