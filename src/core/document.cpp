#include "document.h"
#include "document_utils.h"
#include "fnv1a.h"
#include "parser.h"
#include "profiler.h"
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <filesystem>

namespace {

// CR は ASCII 1 byte なので UTF-8 multi-byte シーケンスの中間バイトとは絶対に衝突しない
// (UTF-8 continuation byte は 10xxxxxx で 0x80-0xBF)。memchr(_, _, 0) は規格上 nullptr を返す。
inline char* FindDocCr(char* p, size_t len) noexcept
{
    return static_cast<char*>(std::memchr(p, mendo::doc_cr, len));
}

// CRLF / 旧式 CR を LF に正規化する (in-place)。
// 改行を LF に揃えておくと、パーサ中の current_text と raw_text_ の memcmp 一致判定が成立し、
// 大半の code block / 複数行 paragraph で view モード化 (owned_text_ 確保ゼロ) が選べる。
// CR まで一気にスキップし、その間は memmove でブロックコピーする (MSVC UCRT で SIMD)。
// LF-only ファイルでは memchr 1 回で素通し。
void NormalizeNewlines(std::pmr::string& s)
{
    MENDO_PROFILE("NormalizeNewlines");
    const size_t n = s.size();
    if (n == 0) {
        return;
    }

    char* const data = s.data();
    char* const end = data + n;

    char* first_cr = FindDocCr(data, n);
    if (!first_cr) {
        MENDO_STATF("NormalizeNewlines: in={} out={} shrunk=0 (fast LF-only)", n, n);
        return;
    }

    // ループ進入時、src は必ず CR を指す (first_cr または直前反復の FindDocCr 結果)。
    char* dst = first_cr;
    char* src = first_cr;
    do {
        *dst++ = mendo::doc_lf;
        ++src;
        if (src < end && *src == mendo::doc_lf) {
            ++src; // CRLF を LF 1 つに縮約
        }

        char* next_cr = FindDocCr(src, static_cast<size_t>(end - src));
        const size_t chunk = next_cr ? static_cast<size_t>(next_cr - src) : static_cast<size_t>(end - src);
        if (chunk > 0) {
            std::memmove(dst, src, chunk * sizeof(char));
            src += chunk;
            dst += chunk;
        }
    } while (src < end);

    s.resize(static_cast<size_t>(dst - data));
    MENDO_STATF("NormalizeNewlines: in={} out={} shrunk={}", n, s.size(), n - s.size());
}

} // namespace

Document::Document(Document&& other) noexcept
    : nodes_(std::move(other.nodes_)), file_path_(std::move(other.file_path_)), raw_text_(std::move(other.raw_text_)), loaded_byte_size_(other.loaded_byte_size_), toc_(std::move(other.toc_)), anchor_index_(std::move(other.anchor_index_)), image_node_indices_(std::move(other.image_node_indices_)), diagram_node_indices_(std::move(other.diagram_node_indices_))
{
    InjectViewBase();
}

Document& Document::operator=(Document&& other) noexcept
{
    if (this != &other) {
        nodes_ = std::move(other.nodes_);
        file_path_ = std::move(other.file_path_);
        raw_text_ = std::move(other.raw_text_);
        loaded_byte_size_ = other.loaded_byte_size_;
        toc_ = std::move(other.toc_);
        anchor_index_ = std::move(other.anchor_index_);
        image_node_indices_ = std::move(other.image_node_indices_);
        diagram_node_indices_ = std::move(other.diagram_node_indices_);
        InjectViewBase();
    }
    return *this;
}

Document Document::FromMarkdown(std::pmr::string text, size_t byte_size, std::wstring_view path)
{
    Document doc;
    doc.file_path_ = path;
    // RawText に入った後の relocate を避けるため、normalize は Replace の前に行う。
    NormalizeNewlines(text);
    doc.raw_text_.Replace(std::move(text));
    doc.loaded_byte_size_ = byte_size;
    doc.ReplaceContent(ParseMarkdown(doc.raw_text_));
    return doc;
}

Document Document::FromMarkdown(std::pmr::string utf8, std::wstring_view path)
{
    // 入力 (Help 埋め込みリソース / テスト文字列) は BOM 無しが保証されているため、
    // size 計算のみ行い 3 引数版へ委譲する。
    const size_t byte_size = utf8.size();
    return FromMarkdown(std::move(utf8), byte_size, path);
}

std::pmr::wstring Document::GetDirectory() const
{
    const auto dir = std::filesystem::path(file_path_).parent_path();
    return std::pmr::wstring{ dir.native() };
}

void Document::ReplaceContent(ParseResult&& result)
{
    // 注意: view_base_ は parser が ParseMarkdown 呼び出し時の markdown_text.data() を既に注入済み。
    // raw_text_ を差し替える経路 (FromMarkdown / ReplaceFromMarkdown) では ParseMarkdown(raw_text_)
    // を渡しているため view_base_ = raw_text_.data() が一致し、ここでの再注入は不要。
    nodes_ = std::move(result.nodes);
    image_node_indices_ = std::move(result.image_indices);
    diagram_node_indices_ = std::move(result.diagram_indices);
    BuildHeadingIndices(result.heading_indices);
}

void Document::InjectViewBase() noexcept
{
    const char* const base = raw_text_.data();
    for (auto& n : nodes_) {
        if (n.IsViewMode()) {
            n.view_base_ = base;
        }
    }
}

void Document::ReplaceFromMarkdown(std::pmr::string text, size_t byte_size)
{
    MENDO_PROFILE("Document::ReplaceFromMarkdown");
    NormalizeNewlines(text);
    raw_text_.Replace(std::move(text));
    loaded_byte_size_ = byte_size;
    ReplaceContent(ParseMarkdown(raw_text_));
}

int Document::FindAnchorIndex(std::string_view anchor) const
{
    if (anchor.empty()) {
        return -1;
    }
    // クエリ引数（外部リンク等）は大文字混在の可能性があるため正規化する。
    const std::pmr::string target = ToLowerAscii(anchor);
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
