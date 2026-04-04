#include "document.h"
#include "document_utils.h"
#include "parser.h"
#include "profiler.h"
#include <filesystem>
#include <ranges>

Document Document::FromMarkdown(std::pmr::string utf8, std::wstring_view path)
{
    Document doc;
    doc.file_path_ = path;
    doc.raw_utf8_ = std::move(utf8);
    {
        MENDO_PROFILE("ParseMarkdown");
        doc.nodes_ = ParseMarkdown(doc.raw_utf8_);
    }
    {
        MENDO_PROFILE("BuildFromNodes(TOC)");
        doc.toc_.BuildFromNodes(doc.nodes_);
    }
    {
        MENDO_PROFILE("BuildAnchorIndex");
        doc.BuildAnchorIndex();
    }
    {
        MENDO_PROFILE("BuildSpecialNodeIndices");
        doc.BuildSpecialNodeIndices();
    }
    return doc;
}

std::pmr::wstring Document::GetDirectory() const
{
    const auto dir = std::filesystem::path(file_path_).parent_path();
    if (!dir.empty()) {
        return std::pmr::wstring{ dir.native() };
    }
    return {};
}

void Document::ReplaceContent(std::pmr::vector<Node> new_nodes)
{
    nodes_ = std::move(new_nodes);
    toc_.BuildFromNodes(nodes_);
    BuildAnchorIndex();
    BuildSpecialNodeIndices();
}

void Document::ReplaceFromMarkdown(std::pmr::string utf8)
{
    raw_utf8_ = std::move(utf8);
    ReplaceContent(ParseMarkdown(raw_utf8_));
}

int Document::FindAnchorIndex(std::wstring_view anchor) const
{
    if (anchor.empty()) {
        return -1;
    }
    const std::pmr::wstring target = ToLowerAscii(anchor);
    const auto it = anchor_index_.find(target);
    return (it != anchor_index_.end()) ? it->second : -1;
}

void Document::BuildAnchorIndex()
{
    anchor_index_.clear();
    for (const auto& [i, node] : nodes_ | std::views::enumerate) {
        if (node.type == NodeType::Heading && !node.anchor_id.empty()) {
            anchor_index_.emplace(node.anchor_id, static_cast<int>(i));
        }
    }
}

void Document::BuildSpecialNodeIndices()
{
    image_node_indices_.clear();
    mermaid_node_indices_.clear();
    const auto node_count = nodes_.size();
    for (size_t i = 0; i < node_count; i++) {
        const auto& node = nodes_[i];
        if (node.type == NodeType::Image) {
            image_node_indices_.emplace_back(i);
        }
        else if (node.type == NodeType::CodeBlock && node.code_language == SyntaxLanguage::Mermaid) {
            mermaid_node_indices_.emplace_back(i);
        }
    }
}
