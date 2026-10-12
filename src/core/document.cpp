#include "document.h"
#include "document_utils.h"
#include "newline_util.h"
#include "parser.h"
#include "profiler.h"
#include <cassert>
#include <cstring>

Document Document::FromMarkdown(std::pmr::string text, size_t byte_size, std::wstring_view path,
                                std::stop_token stop_token)
{
    Document doc;
    doc.SetFilePath(path);
    doc.ReplaceFromMarkdown(std::move(text), byte_size, std::move(stop_token));
    return doc;
}

Document Document::FromMarkdown(std::pmr::string utf8, std::wstring_view path)
{
    // 入力 (Help 埋め込みリソース / テスト文字列) は BOM 無しが保証されている。
    // FileLoader を通らないため、ここで LF 正規化してから 3 引数版へ委譲する。
    const size_t byte_size = utf8.size();
    NormalizeNewlines(utf8);
    return FromMarkdown(std::move(utf8), byte_size, path);
}

void Document::RebuildCachedDirectory()
{
    cached_directory_ = ParentDirectory(file_path_);
}

void Document::ReplaceContent(ParseResult&& result)
{
    // ParseMarkdown(raw_text_) の結果なので view_.data() は raw_text_ を指しており rebase 不要。
    nodes_ = std::move(result.nodes);
    image_node_indices_ = std::move(result.image_indices);
    diagram_node_indices_ = std::move(result.diagram_indices);
    table_node_indices_ = std::move(result.table_indices);
    footnote_def_indices_ = std::move(result.footnote_def_indices);
    footnote_refs_ = std::move(result.footnote_refs);
    BuildHeadingIndices(result.heading_indices);
    anchor_index_.reset();
}

void Document::ReplaceFromMarkdown(std::pmr::string text, size_t byte_size, std::stop_token stop_token)
{
    MENDO_PROFILE("Document::ReplaceFromMarkdown");
    assert(std::memchr(text.data(), '\r', text.size()) == nullptr);
    raw_text_.Replace(std::move(text));
    loaded_byte_size_ = byte_size;
    ReplaceContent(ParseMarkdown(raw_text_, std::move(stop_token)));
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
    // 割り当てるアンカーは ASCII 小文字なので query 側だけ畳む。
    const auto key = ToLowerAsciiCopy(anchor);
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
    const auto& entries = toc_.GetEntries();
    index.Reserve(entries.size());
    for (size_t i = index.heading_count(); i < entries.size(); ++i) {
        const int node_index = entries[i].node_index;
        if (index.Add(nodes_[node_index].GetText(), node_index) == key) {
            return node_index;
        }
    }
    return -1;
}

void Document::BuildHeadingIndices(const std::pmr::vector<size_t>& heading_indices)
{
    MENDO_PROFILE("BuildHeadingIndices");
    toc_.Clear();
    toc_.Reserve(heading_indices.size());
    for (size_t i : heading_indices) {
        toc_.AddEntry(nodes_[i], static_cast<int>(i));
    }
}
