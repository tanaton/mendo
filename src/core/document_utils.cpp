#include "document_utils.h"
#include "ascii_util.h"
#include "unicode_lower_table.h"
#include "utf8_codec.h"
#include <array>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iterator>
#include <ranges>

std::pmr::string ToLowerAsciiCopy(std::string_view text)
{
    std::pmr::string result;
    result.resize_and_overwrite(text.size(), [&text](char* buf, size_t count) noexcept -> size_t {
        ascii_util::AsciiToLowerOnly(text.data(), buf, count);
        return count;
    });
    return result;
}

namespace {

// ダブルクリック選択時の文字種カテゴリ。同じカテゴリが連続する code point を 1 単語として扱う。
enum class CharCategory : uint8_t {
    AsciiWord,
    Hiragana,
    Katakana,
    Han,
    FullwidthAlnum,
    Other,
};

constexpr CharCategory CategorizeCodePoint(uint32_t cp) noexcept
{
    if (cp < 0x80) {
        return ascii_util::IsAsciiWordChar(static_cast<char>(cp)) ? CharCategory::AsciiWord : CharCategory::Other;
    }
    // 漢字繰り返し記号 (々〆〇) はひらがな/カタカナ範囲より先に判定。
    if (cp == 0x3005 || cp == 0x3006 || cp == 0x3007) {
        return CharCategory::Han;
    }
    if (cp >= 0x3041 && cp <= 0x309F) {
        return CharCategory::Hiragana;
    }
    if ((cp >= 0x30A0 && cp <= 0x30FF) || (cp >= 0x31F0 && cp <= 0x31FF) || (cp >= 0xFF65 && cp <= 0xFF9F)) {
        return CharCategory::Katakana;
    }
    if ((cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x20000 && cp <= 0x2FFFF)) {
        return CharCategory::Han;
    }
    if ((cp >= 0xFF10 && cp <= 0xFF19) || (cp >= 0xFF21 && cp <= 0xFF3A) || (cp >= 0xFF41 && cp <= 0xFF5A)) {
        return CharCategory::FullwidthAlnum;
    }
    return CharCategory::Other;
}

template <typename SV>
WordBoundary FindWordBoundariesImpl(SV text, uint32_t pos) noexcept
{
    WordBoundary result;
    if (text.empty()) {
        return result;
    }
    if (pos >= text.size()) {
        pos = static_cast<uint32_t>(text.size()) - 1;
    }
    pos = utf8_codec::SnapToCpStart(text, pos);

    const auto here = utf8_codec::DecodeAt(text, pos);
    const auto cat = CategorizeCodePoint(here.cp);
    if (cat == CharCategory::Other) {
        return result;
    }

    uint32_t start = pos;
    while (start > 0) {
        const auto prev = utf8_codec::DecodePrev(text, start);
        if (CategorizeCodePoint(prev.cp) != cat) {
            break;
        }
        start -= prev.len;
    }

    uint32_t end = pos + here.len;
    while (end < text.size()) {
        const auto next = utf8_codec::DecodeAt(text, end);
        if (CategorizeCodePoint(next.cp) != cat) {
            break;
        }
        end += next.len;
    }

    result.start = start;
    result.end = end;
    result.found = true;
    return result;
}

} // namespace

WordBoundary FindWordBoundaries(std::string_view text, uint32_t pos) noexcept
{
    return FindWordBoundariesImpl(text, pos);
}

WordBoundary FindWordBoundaries(std::wstring_view text, uint32_t pos) noexcept
{
    return FindWordBoundariesImpl(text, pos);
}

std::wstring_view ExtensionView(std::wstring_view path) noexcept
{
    const auto last_sep = path.find_last_of(L"\\/");
    const auto dot_pos = path.rfind(L'.');
    if (dot_pos == std::wstring_view::npos || (last_sep != std::wstring_view::npos && dot_pos < last_sep)) {
        return {};
    }
    return path.substr(dot_pos);
}

bool IsMarkdownFile(std::wstring_view path)
{
    static constexpr ascii_util::LowercaseAsciiLiteral kMarkdownExts[]{
        L".md",
        L".markdown",
        L".mkd",
    };
    const auto ext = ExtensionView(path);
    return std::ranges::any_of(kMarkdownExts, [&](const auto& e) noexcept {
        return ascii_util::iequal(ext, e);
    });
}

std::pmr::wstring ExtractFilename(std::wstring_view path)
{
    if (path.empty()) {
        return {};
    }
    return std::pmr::wstring{ std::filesystem::path(path).filename().native() };
}

std::pmr::wstring ParentDirectory(std::wstring_view path)
{
    return std::pmr::wstring{ std::filesystem::path(path).parent_path().native() };
}

namespace {

// GitHub のスラグが使う toLowerCase に合わせる (UTF-8 長が伸びる変換は表から除いてある)。
constexpr uint32_t ToLowerNonAscii(uint32_t cp) noexcept
{
    // 見出しに多い CJK などは大文字を持たないので、表を引かずに返す (生成時に確認済み)。
    if (cp >= 0x3000 && cp < 0xA640) {
        return cp;
    }
    const auto it = std::ranges::lower_bound(kUnicodeLowerMappings, cp, {}, &UnicodeLowerMapping::upper);
    return it != std::ranges::end(kUnicodeLowerMappings) && it->upper == cp ? it->lower : cp;
}

static_assert(std::ranges::is_sorted(kUnicodeLowerMappings, {}, &UnicodeLowerMapping::upper));

// 非 ASCII の 1 コードポイントを小文字にして dst に書き、書いたバイト数を返す (decoded.len 以下)。
size_t WriteLowerNonAscii(utf8_codec::DecodedCp decoded, const char* src, char* dst) noexcept
{
    const uint32_t lower = ToLowerNonAscii(decoded.cp);
    if (lower == decoded.cp) {
        std::copy_n(src, decoded.len, dst);
        return decoded.len;
    }
    return utf8_codec::EncodeCp(lower, dst);
}

// GenerateAnchorId の ASCII (0x00–0x7F) 処理を 1 回の lookup で判定するテーブル。
// 値: 0=skip, 1=keep, 2=hyphen, 3=lower (A-Z のみ。実際の変換は c | 0x20 で小文字化)。
enum AsciiSlugAction : uint8_t {
    SlugSkip = 0,
    SlugKeep = 1,
    SlugHyphen = 2,
    SlugLower = 3,
};

constexpr auto kAsciiSlugTable = [] static noexcept {
    std::array<AsciiSlugAction, 128> t{};
    for (size_t i = 0; i < t.size(); ++i) {
        const char c = static_cast<char>(i);
        if ((c >= 'a' && c <= 'z') || ascii_util::IsAsciiDigit(c) || c == '-' || c == '_') {
            t[i] = SlugKeep;
        }
        else if (c >= 'A' && c <= 'Z') {
            t[i] = SlugLower;
        }
        else if (c == ' ' || c == '\t') {
            t[i] = SlugHyphen;
        }
        else {
            t[i] = SlugSkip;
        }
    }
    return t;
}();

// アンカー (GitHub 互換スラグ) に採用しない記号・句読点か。非 ASCII コードポイント単位で判定。
// Unicode 全カテゴリテーブルは持たないため、主要な記号ブロックの近似で判定する
// (文字 (Letter) を誤って落とすより、稀な記号を通す方向に倒す)。
constexpr bool IsAnchorSkippableSymbol(uint32_t cp) noexcept
{
    // ラテン1補助の記号 (U+00A0–U+00BF)
    if (cp >= 0x00A0 && cp <= 0x00BF) {
        return true;
    }
    // 一般句読点・通貨・矢印・数学記号等 (U+2000–U+2BFF)、補助句読点 (U+2E00–U+2E7F)
    if ((cp >= 0x2000 && cp <= 0x2BFF) || (cp >= 0x2E00 && cp <= 0x2E7F)) {
        return true;
    }
    // CJK 記号と句読点 (U+3000–U+303F)
    if (cp >= 0x3000 && cp <= 0x303F) {
        return true;
    }
    // 全角 ASCII 対応の記号関連
    return (cp >= 0xFF01 && cp <= 0xFF0F) || (cp >= 0xFF1A && cp <= 0xFF20) || (cp >= 0xFF3B && cp <= 0xFF40) || (cp >= 0xFF5B && cp <= 0xFF65);
}

} // namespace

void GenerateAnchorIdInto(std::string_view text, std::pmr::string& slug)
{
    slug.clear();
    // 出力は入力サイズ以下で確定のため一括確保して書き出す。
    slug.resize_and_overwrite(text.size(), [text](char* buf, size_t /*count*/) noexcept -> size_t {
        char* dst = buf;
        const size_t n = text.size();
        size_t pos = 0;
        while (pos < n) {
            const auto cu = static_cast<uint32_t>(static_cast<unsigned char>(text[pos]));
            if (cu < 0x80) {
                // ASCII ファストパス: 1 回の table lookup で分岐を絞る。
                switch (kAsciiSlugTable[cu]) {
                case SlugKeep:
                    *dst++ = static_cast<char>(cu);
                    break;
                case SlugLower:
                    *dst++ = static_cast<char>(cu | 0x20);
                    break;
                case SlugHyphen:
                    *dst++ = '-';
                    break;
                case SlugSkip:
                    break;
                default:
                    std::unreachable();
                }
                ++pos;
                continue;
            }
            const auto decoded = utf8_codec::DecodeAt(text, static_cast<uint32_t>(pos));
            // U+FFFD は不正/truncated バイト由来。アンカーには採用しない。
            if (decoded.cp != utf8_codec::kReplacement && !IsAnchorSkippableSymbol(decoded.cp)) {
                dst += WriteLowerNonAscii(decoded, text.data() + pos, dst);
            }
            pos += decoded.len;
        }
        return static_cast<size_t>(dst - buf);
    });
}


std::pmr::string ToLowerAnchorCopy(std::string_view text)
{
    std::pmr::string result;
    result.resize_and_overwrite(text.size(), [text](char* buf, size_t /*count*/) noexcept -> size_t {
        char* dst = buf;
        size_t pos = 0;
        while (pos < text.size()) {
            const char c = text[pos];
            if (static_cast<unsigned char>(c) < 0x80) {
                *dst++ = ascii_util::ToLowerAscii(c);
                ++pos;
                continue;
            }
            const auto decoded = utf8_codec::DecodeAt(text, static_cast<uint32_t>(pos));
            // 不正バイトは U+FFFD に置き換えず元のバイトを残す (どの見出しとも一致しないままにする)。
            if (decoded.cp == utf8_codec::kReplacement) {
                *dst++ = c;
                ++pos;
                continue;
            }
            dst += WriteLowerNonAscii(decoded, text.data() + pos, dst);
            pos += decoded.len;
        }
        return static_cast<size_t>(dst - buf);
    });
    return result;
}

std::pmr::wstring BuildTitleString(std::wstring_view path, int zoom_percent)
{
    const auto filename = ExtractFilename(path);
    std::pmr::wstring title = filename.empty() ? std::pmr::wstring{ L"mendo" } : filename + L" - mendo";
    if (zoom_percent > 0 && zoom_percent != 100) {
        std::format_to(std::back_inserter(title), L" ({}%)", zoom_percent);
    }
    return title;
}

namespace {

bool TableContentEquals(const NodeTableData& a, const NodeTableData& b) noexcept
{
    return a.row_count == b.row_count && a.col_count == b.col_count && a.concat_text == b.concat_text &&
           std::ranges::equal(a.all_runs, b.all_runs) && std::ranges::equal(a.cell_text_starts, b.cell_text_starts) &&
           std::ranges::equal(a.cell_run_starts, b.cell_run_starts) && std::ranges::equal(a.aligns, b.aligns) &&
           std::ranges::equal(a.is_header_row, b.is_header_row);
}

} // namespace

bool NodeContentEquals(const Node& a, const Node& b) noexcept
{
    if (a.type != b.type || a.alert_type != b.alert_type || a.quote_depth != b.quote_depth ||
        a.quote_outer_indent != b.quote_outer_indent || a.indent_level != b.indent_level || a.line_count != b.line_count ||
        a.heading_level() != b.heading_level() || a.code_language() != b.code_language() ||
        a.list_ordered() != b.list_ordered() || a.list_number() != b.list_number() || a.task_checked() != b.task_checked() ||
        a.has_child_blocks() != b.has_child_blocks() || a.alert_label_length() != b.alert_label_length()) {
        return false;
    }
    // 参照定義が後ろで変わると、前のノードはテキストが同じでもリンク先が変わる。
    if (a.GetText() != b.GetText() || !std::ranges::equal(a.runs(), b.runs()) || !std::ranges::equal(a.view_link_urls(), b.view_link_urls())) {
        return false;
    }
    const auto* ai = a.image_data();
    const auto* bi = b.image_data();
    if ((ai == nullptr) != (bi == nullptr) || (ai && ai->src != bi->src)) {
        return false;
    }
    const auto* at = a.table_data();
    const auto* bt = b.table_data();
    return (at == nullptr) == (bt == nullptr) && (!at || TableContentEquals(*at, *bt));
}
