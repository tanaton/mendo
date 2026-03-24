#include "document.h"
#include "parser.h"
#include <filesystem>

Document Document::FromMarkdown(const std::pmr::string& utf8, std::wstring_view path) {
    Document doc;
    doc.file_path_ = path;
    doc.nodes_ = ParseMarkdown(utf8);
    doc.toc_.BuildFromNodes(doc.nodes_);
    return doc;
}

std::pmr::wstring Document::GetDirectory() const {
    auto dir = std::filesystem::path(file_path_.c_str()).parent_path();
    if (!dir.empty()) {
        return std::pmr::wstring{ std::wstring_view{dir.native()} };
    }
    return {};
}

void Document::ReplaceContent(std::pmr::vector<Node> new_nodes) {
    nodes_ = std::move(new_nodes);
    toc_.BuildFromNodes(nodes_);
}

void Document::ReplaceFromMarkdown(const std::pmr::string& utf8) {
    ReplaceContent(ParseMarkdown(utf8));
}
