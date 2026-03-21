#include "document.h"
#include "parser.h"

Document Document::FromMarkdown(const std::string& utf8, std::wstring_view path) {
    Document doc;
    doc.file_path_ = path;
    doc.nodes_ = ParseMarkdown(utf8);
    doc.toc_.BuildFromNodes(doc.nodes_);
    return doc;
}

std::wstring Document::GetDirectory() const {
    auto pos = file_path_.find_last_of(L"\\/");
    if (pos != std::pmr::wstring::npos) {
        return std::wstring(file_path_.substr(0, pos));
    }
    return {};
}

void Document::ReplaceContent(std::pmr::vector<Node> new_nodes) {
    nodes_ = std::move(new_nodes);
    toc_.BuildFromNodes(nodes_);
}

void Document::ReplaceFromMarkdown(const std::string& utf8) {
    ReplaceContent(ParseMarkdown(utf8));
}
