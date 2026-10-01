#include "document.h"
#include "ascii_util.h"
#include "document_utils.h"
#include "fnv1a.h"
#include "newline_util.h"
#include "parser.h"
#include "profiler.h"
#include <algorithm>
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
    // private 化された内部 helper。呼び出し元 (FromMarkdown / ReplaceFromMarkdown) は
    // 必ず ParseMarkdown(raw_text_) を渡しており、各ノードの view_.data() のベースが
    // raw_text_.data() と一致するため rebase 不要。
    nodes_ = std::move(result.nodes);
    image_node_indices_ = std::move(result.image_indices);
    diagram_node_indices_ = std::move(result.diagram_indices);
    table_node_indices_ = std::move(result.table_indices);
    BuildHeadingIndices(result.heading_indices);
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

int Document::FindAnchorIndex(std::string_view anchor) const
{
    if (anchor.empty()) {
        return -1;
    }
    char stack_buf[256];
    if (anchor.size() <= sizeof(stack_buf)) {
        ascii_util::AsciiToLowerOnly(anchor.data(), stack_buf, anchor.size());
        return FindNormalizedAnchorIndex(std::string_view{ stack_buf, anchor.size() });
    }
    const std::pmr::string target = ToLowerAsciiCopy(anchor);
    return FindNormalizedAnchorIndex(target);
}

int Document::FindNormalizedAnchorIndex(std::string_view anchor) const
{
    if (anchor.empty()) {
        return -1;
    }
    const std::uint64_t h = mendo::Fnv1a64(anchor);
    // FNV-1a 衝突時に異なる anchor_id を取り違えないよう、hash 一致範囲を文字列比較で絞る。
    const auto [lo, hi] = std::ranges::equal_range(anchor_index_, h, {}, &decltype(anchor_index_)::value_type::first);
    for (auto it = lo; it != hi; ++it) {
        if (nodes_[it->second].anchor_id() == anchor) {
            return it->second;
        }
    }
    return -1;
}

void Document::BuildHeadingIndices(const std::pmr::vector<size_t>& heading_indices)
{
    MENDO_PROFILE("BuildHeadingIndices");
    toc_.Clear();
    toc_.Reserve(heading_indices.size());
    anchor_index_.clear();
    anchor_index_.reserve(heading_indices.size());

    for (size_t i : heading_indices) {
        const auto& node = nodes_[i];
        toc_.AddEntry(node, static_cast<int>(i));
        const auto sv = node.anchor_id();
        if (!sv.empty()) {
            anchor_index_.emplace_back(mendo::Fnv1a64(sv), static_cast<int>(i));
        }
    }
    // pair のデフォルト辞書順 (hash 昇順 → node_index 昇順) でソートする。unique は取らず、
    // 同 anchor_id の重複見出しと、稀な hash 衝突の両方をエントリとして保持する。lookup 側
    // (FindNormalizedAnchorIndex) で文字列比較して先勝ちを選ぶ。
    {
        MENDO_PROFILE("BuildHeadingIndices.Sort");
        std::ranges::sort(anchor_index_);
    }
}
