#include "document.h"
#include "document_utils.h"
#include "file_io.h"
#include "parser.h"
#include "profiler.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>

namespace {

// GitHub は見出しのスラグを見出しの文字 (textContent) から作るので、画像の alt (属性) は含めない。
std::string_view AnchorTextOf(const Node& heading, std::string& scratch)
{
    const std::string_view text = heading.GetText();
    const auto runs = heading.runs();
    if (std::ranges::none_of(runs, &TextRun::image)) {
        return text;
    }
    scratch.clear();
    uint32_t pos = 0;
    for (const auto& r : runs) {
        if (r.image()) {
            scratch.append(text.substr(pos, r.start - pos));
            pos = r.start + r.length;
        }
    }
    scratch.append(text.substr(pos));
    return scratch;
}

} // namespace

Document Document::FromNormalizedMarkdown(std::string text, std::wstring_view path, std::stop_token stop_token)
{
    MENDO_PROFILE("Document::FromMarkdown");
    assert(std::string_view{ text }.find_first_of(std::string_view{ "\r\0", 2 }) == std::string_view::npos);
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
    NormalizeSource(utf8);
    return FromNormalizedMarkdown(std::move(utf8), path);
}

bool Document::HasBackingFile() const noexcept
{
    return !file_path_.empty() && !IsEmbeddedPath(file_path_);
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
    std::string scratch;
    for (size_t i = index.heading_count(); i < indices.toc_entries.size(); ++i) {
        const int node_index = indices.toc_entries[i].node_index;
        if (index.Add(AnchorTextOf(GetNodes()[node_index], scratch), node_index) == key) {
            return node_index;
        }
    }
    return -1;
}
