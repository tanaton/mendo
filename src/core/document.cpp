#include "document.h"
#include "document_utils.h"
#include "parser.h"
#include "profiler.h"
#include <cassert>
#include <cstring>

Document Document::FromMarkdown(std::pmr::string text, size_t byte_size, std::wstring_view path,
                                std::stop_token stop_token)
{
    MENDO_PROFILE("Document::FromMarkdown");
    assert(std::memchr(text.data(), '\r', text.size()) == nullptr);
    Document doc;
    doc.file_path_ = path;
    doc.cached_directory_ = ParentDirectory(path);
    doc.raw_text_.Replace(std::move(text));
    doc.loaded_byte_size_ = byte_size;
    // raw_text_ をパースした結果なのでノードのテキストは raw_text_ か arena を指しており rebase 不要。
    ParseResult result = ParseMarkdown(doc.raw_text_, std::move(stop_token));
    auto [nodes, arena, tables] = result.TakeNodes();
    doc.nodes_ = std::move(nodes);
    doc.text_arena_ = std::move(arena);
    doc.tables_ = std::move(tables);
    doc.image_node_indices_ = std::move(result.image_indices);
    doc.diagram_node_indices_ = std::move(result.diagram_indices);
    doc.table_node_indices_ = std::move(result.table_indices);
    doc.footnote_def_indices_ = std::move(result.footnote_def_indices);
    doc.footnote_refs_ = std::move(result.footnote_refs);
    doc.toc_ = std::move(result.toc_entries);
    return doc;
}

Document Document::FromMarkdown(std::pmr::string utf8, std::wstring_view path)
{
    // 入力 (Help 埋め込みリソース / テスト文字列) は BOM 無しが保証されている。
    const size_t byte_size = utf8.size();
    NormalizeNewlines(utf8);
    return FromMarkdown(std::move(utf8), byte_size, path);
}

bool Document::HasBackingFile() const noexcept
{
    return !file_path_.empty() && !IsHelpPath(file_path_);
}

int Document::FindAnchorIndex(std::string_view anchor)
{
    if (anchor.empty()) {
        return -1;
    }
    // 割り当てるアンカーは小文字なので query 側だけ畳む。
    const auto key = ToLowerAnchorCopy(anchor);
    if (const int footnote = FindFootnoteAnchor(key, footnote_def_indices_, footnote_refs_); footnote >= 0) {
        return footnote;
    }
    if (!anchor_index_) {
        anchor_index_ = std::make_unique<HeadingAnchorIndex>();
    }
    auto& index = *anchor_index_;
    if (const int found = index.Find(key); found >= 0) {
        return found;
    }
    // 割り当て済みのアンカーは後の見出しで変わらないので、前回の続きから延ばせばよい。
    index.Reserve(toc_.size());
    for (size_t i = index.heading_count(); i < toc_.size(); ++i) {
        const int node_index = toc_[i].node_index;
        if (index.Add(nodes_[node_index].GetText(), node_index) == key) {
            return node_index;
        }
    }
    return -1;
}
