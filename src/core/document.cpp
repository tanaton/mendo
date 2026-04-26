#include "document.h"
#include "document_utils.h"
#include "parser.h"
#include "profiler.h"
#include <filesystem>

Document Document::FromMarkdown(std::pmr::string utf8, std::wstring_view path)
{
    Document doc;
    doc.file_path_ = path;
    doc.raw_utf8_ = std::move(utf8);
    ParseResult result;
    {
        MENDO_PROFILE("ParseMarkdown");
        result = ParseMarkdown(doc.raw_utf8_);
    }
    {
        MENDO_PROFILE("BuildIndices");
        doc.ReplaceContent(std::move(result));
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

void Document::ReplaceContent(ParseResult&& result)
{
    // ParseMarkdown は各ノードの text_ を Wide で確定させて返す契約。ここでは再変換しない。
    nodes_ = std::move(result.nodes);
    image_node_indices_ = std::move(result.image_indices);
    diagram_node_indices_ = std::move(result.diagram_indices);
    BuildHeadingIndices(result.heading_indices);
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
    // クエリ引数（外部リンク等）は大文字混在の可能性があるため正規化する。
    const std::pmr::wstring target = ToLowerAscii(anchor);
    const auto it = anchor_index_.find(target);
    return (it != anchor_index_.end()) ? it->second : -1;
}

void Document::BuildHeadingIndices(const std::pmr::vector<size_t>& heading_indices)
{
    toc_.Clear();
    anchor_index_.clear();
    anchor_index_.reserve(heading_indices.size());

    for (size_t i : heading_indices) {
        const auto& node = nodes_[i];
        toc_.AddEntry(node, static_cast<int>(i));
        const auto sv = node.anchor_id();
        if (!sv.empty()) {
            anchor_index_.emplace(sv, static_cast<int>(i));
        }
    }
}
