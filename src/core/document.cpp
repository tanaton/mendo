#include "document.h"
#include "document_utils.h"
#include "parser.h"
#include "profiler.h"
#include "string_convert.h"
#include <cassert>
#include <cstring>
#include <cwchar>
#include <filesystem>

namespace {

// CRLF / 旧式 CR を LF に正規化する (in-place)。
// 改行を LF に揃えておくと、パーサ中の current_text と raw_wide_ の memcmp 一致判定が成立し、
// 大半の code block / 複数行 paragraph で view モード化 (owned_text_ 確保ゼロ) が選べる。
// wmemchr で次の CR まで一気にスキップし、その間は memmove でブロックコピーする (MSVC UCRT で
// 共に SIMD ディスパッチ)。LF-only ファイルでは wmemchr 1 回で素通し。
void NormalizeNewlines(std::pmr::wstring& s)
{
    MENDO_PROFILE("NormalizeNewlines");
    const size_t n = s.size();
    if (n == 0) {
        return;
    }

    wchar_t* const data = s.data();
    wchar_t* const end = data + n;

    wchar_t* first_cr = std::wmemchr(data, L'\r', n);
    if (!first_cr) {
        MENDO_STATF("NormalizeNewlines: in=%zu out=%zu shrunk=0 (fast LF-only)", n, n);
        return;
    }

    // ループ進入時、src は必ず CR を指す (first_cr または直前反復の wmemchr 結果)。
    wchar_t* dst = first_cr;
    wchar_t* src = first_cr;
    do {
        *dst++ = L'\n';
        ++src;
        if (src < end && *src == L'\n') {
            ++src; // CRLF を LF 1 つに縮約
        }

        // wmemchr(_, _, 0) は規格上 nullptr を返すので size==0 ガード不要。
        wchar_t* next_cr = std::wmemchr(src, L'\r', static_cast<size_t>(end - src));
        const size_t chunk = next_cr ? static_cast<size_t>(next_cr - src) : static_cast<size_t>(end - src);
        if (chunk > 0) {
            std::memmove(dst, src, chunk * sizeof(wchar_t));
            src += chunk;
            dst += chunk;
        }
    } while (src < end);

    s.resize(static_cast<size_t>(dst - data));
    MENDO_STATF("NormalizeNewlines: in=%zu out=%zu shrunk=%zu", n, s.size(), n - s.size());
}

} // namespace

Document::Document(Document&& other) noexcept
    : nodes_(std::move(other.nodes_))
    , file_path_(std::move(other.file_path_))
    , raw_wide_(std::move(other.raw_wide_))
    , loaded_byte_size_(other.loaded_byte_size_)
    , toc_(std::move(other.toc_))
    , anchor_index_(std::move(other.anchor_index_))
    , image_node_indices_(std::move(other.image_node_indices_))
    , diagram_node_indices_(std::move(other.diagram_node_indices_))
{
    InjectViewBase();
}

Document& Document::operator=(Document&& other) noexcept
{
    if (this != &other) {
        nodes_ = std::move(other.nodes_);
        file_path_ = std::move(other.file_path_);
        raw_wide_ = std::move(other.raw_wide_);
        loaded_byte_size_ = other.loaded_byte_size_;
        toc_ = std::move(other.toc_);
        anchor_index_ = std::move(other.anchor_index_);
        image_node_indices_ = std::move(other.image_node_indices_);
        diagram_node_indices_ = std::move(other.diagram_node_indices_);
        InjectViewBase();
    }
    return *this;
}

Document Document::FromMarkdown(std::pmr::wstring wide, size_t byte_size, std::wstring_view path)
{
    Document doc;
    doc.file_path_ = path;
    doc.raw_wide_ = std::move(wide);
    NormalizeNewlines(doc.raw_wide_);
    doc.loaded_byte_size_ = byte_size;
    doc.ReplaceContent(ParseMarkdown(doc.raw_wide_));
    return doc;
}

Document Document::FromMarkdown(std::pmr::string utf8, std::wstring_view path)
{
    const size_t byte_size = utf8.size();
    std::pmr::wstring wide;
    string_convert::Utf8ToWideStripBom(utf8, wide);
    return FromMarkdown(std::move(wide), byte_size, path);
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
    // 注意: view_base_ は parser が ParseMarkdown 呼び出し時の markdown_text.data() を既に注入済み。
    // raw_wide_ を差し替える経路 (FromMarkdown / ReplaceFromMarkdown) では ParseMarkdown(raw_wide_)
    // を渡しているため view_base_ = raw_wide_.data() が一致し、ここでの再注入は不要。
    nodes_ = std::move(result.nodes);
    image_node_indices_ = std::move(result.image_indices);
    diagram_node_indices_ = std::move(result.diagram_indices);
    BuildHeadingIndices(result.heading_indices);
}

void Document::InjectViewBase() noexcept
{
    const wchar_t* const base = raw_wide_.data();
    [[maybe_unused]] const size_t raw_size = raw_wide_.size();
    for (auto& n : nodes_) {
        if (n.IsViewMode()) {
            n.view_base_ = base;
            // view 範囲は必ず raw_wide_ 内に収まること。範囲外だと GetText() が OOB になる。
            assert(static_cast<size_t>(n.source_offset) + n.view_length <= raw_size);
        }
    }
}

void Document::ReplaceFromMarkdown(std::pmr::wstring wide, size_t byte_size)
{
    raw_wide_ = std::move(wide);
    NormalizeNewlines(raw_wide_);
    loaded_byte_size_ = byte_size;
    ReplaceContent(ParseMarkdown(raw_wide_));
}

void Document::ReplaceFromMarkdown(std::pmr::string utf8)
{
    const size_t byte_size = utf8.size();
    std::pmr::wstring wide;
    string_convert::Utf8ToWideStripBom(utf8, wide);
    ReplaceFromMarkdown(std::move(wide), byte_size);
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

int Document::FindNormalizedAnchorIndex(std::wstring_view anchor) const
{
    if (anchor.empty()) {
        return -1;
    }
    // 透過ハッシュにより wstring_view のまま確保なしで lookup できる。
    const auto it = anchor_index_.find(anchor);
    return (it != anchor_index_.end()) ? it->second : -1;
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
            std::pmr::wstring key{ sv, anchor_index_.get_allocator().resource() };
            anchor_index_.try_emplace(std::move(key), static_cast<int>(i));
        }
    }
}
