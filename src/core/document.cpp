#include "document.h"
#include "document_utils.h"
#include "file_io.h"
#include "parser.h"
#include "profiler.h"
#include <cassert>
#include <cstring>

Document Document::FromNormalizedMarkdown(std::string text, std::wstring_view path, std::stop_token stop_token)
{
    MENDO_PROFILE("Document::FromMarkdown");
    assert(std::memchr(text.data(), '\r', text.size()) == nullptr);
    Document doc;
    doc.file_path_ = path;
    doc.cached_directory_ = path_util::ParentDirectory(path);
    auto data = std::make_shared<DocumentData>(std::move(text));
    data->parsed = ParseMarkdown(data->raw, std::move(stop_token));
    doc.data_ = std::move(data);
    return doc;
}

Document Document::FromMarkdown(std::string utf8, std::wstring_view path)
{
    // 入力 (Help 埋め込みリソース / テスト文字列) は BOM 無しが保証されている。
    NormalizeNewlines(utf8);
    return FromNormalizedMarkdown(std::move(utf8), path);
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
    const NodeIndices& indices = Storage().indices;
    if (const int footnote = FindFootnoteAnchor(key, indices.footnote_def_indices, indices.footnote_refs); footnote >= 0) {
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
    index.Reserve(indices.toc_entries.size());
    for (size_t i = index.heading_count(); i < indices.toc_entries.size(); ++i) {
        const int node_index = indices.toc_entries[i].node_index;
        if (index.Add(GetNodes()[node_index].GetText(), node_index) == key) {
            return node_index;
        }
    }
    return -1;
}
