#include "document.h"
#include "document_utils.h"
#include "parser.h"
#include "profiler.h"
#include "string_convert.h"
#include <cassert>
#include <filesystem>

namespace {

// CRLF / 旧式 CR を LF に正規化する (in-place)。
// md4c は OnText で改行を LF (\n) として返してくる契約のため、raw_wide_ の改行を LF に揃えておくと
// パーサ中の current_text と raw_wide_ の memcmp 一致による view 化判定が機能し、
// 大半の code block / 複数行 paragraph で Node::owned_text_ の確保を省ける。
//
// 計測指標: Tracy ON 時に "NormalizeNewlines" zone と入出力サイズの stat を出力する。
// CR を含まない LF-only ファイルでは fast path (走査のみ・書き戻しなし) で素通しになるよう
// 実装しているため、典型的な現代の Markdown ファイルでは memmove 相当のコストすら発生しない。
void NormalizeNewlines(std::pmr::wstring& s)
{
    MENDO_PROFILE("NormalizeNewlines");
    [[maybe_unused]] const size_t before = s.size();

    // Fast path: CR が一つも無いなら何も書き戻さない。LF-only ファイル (現代 Linux/macOS の標準、
    // Git の autocrlf=input 経路) では走査 1 回だけで完了する。
    const auto end = s.end();
    auto first_cr = std::find(s.begin(), end, L'\r');
    if (first_cr == end) {
        MENDO_STATF("NormalizeNewlines: in=%zu out=%zu shrunk=0 (fast LF-only)", before, before);
        return;
    }

    // Slow path: CR を見つけた位置から書き戻し開始。それまでの prefix は in-place で touch しない。
    auto src = first_cr;
    auto dst = first_cr;
    while (src != end) {
        if (*src == L'\r') {
            *dst++ = L'\n';
            ++src;
            // CRLF を LF 1 つに縮約 (CR のみは LF 1 つに置換、すでに上の代入で済んでいる)
            if (src != end && *src == L'\n') {
                ++src;
            }
        }
        else {
            if (dst != src) {
                *dst = *src;
            }
            ++src;
            ++dst;
        }
    }
    s.erase(dst, end);
    MENDO_STATF("NormalizeNewlines: in=%zu out=%zu shrunk=%zu", before, s.size(), before - s.size());
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
    // pmr::wstring の move でヒープポインタが引き継がれても、move 元/先で data() の同一性は保証されない
    // (SBO 圏や allocator 不一致経路でコピーが起きうる)。view モードノードに raw_wide_.data() を再注入する。
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
    // ParseMarkdown は各ノードのテキスト位置 (view モード時は source_offset/view_length/view_base_、
    // owned モード時は owned_text_) を確定させて返す契約。view_base_ は parser が呼び出し元の
    // markdown_text のベースポインタを既に注入済みなので、ここでは再注入しない (このメソッドは
    // raw_wide_ を更新しない経路でも呼ばれる)。raw_wide_ を差し替えた直後に呼ぶのは
    // FromMarkdown / ReplaceFromMarkdown のみで、それらは ParseMarkdown(raw_wide_) を渡しているため
    // view_base_ = raw_wide_.data() が一致する。
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
        if (n.view_length > 0) {
            n.view_base_ = base;
            // 不変条件 C-1: view 範囲は必ず raw_wide_ 内に収まること。
            // 範囲外の view_length が紛れ込むと GetText() で OOB アクセスになる。
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
