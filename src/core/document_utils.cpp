#include "document_utils.h"
#include "ascii_util.h"
#include <array>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iterator>
#include <ranges>

std::pmr::string ToLowerAscii(std::string_view text)
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
    if ((cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF)) {
        return CharCategory::Han;
    }
    if (cp >= 0x20000 && cp <= 0x2FFFF) {
        return CharCategory::Han;
    }
    if ((cp >= 0xFF10 && cp <= 0xFF19) || (cp >= 0xFF21 && cp <= 0xFF3A) || (cp >= 0xFF41 && cp <= 0xFF5A)) {
        return CharCategory::FullwidthAlnum;
    }
    return CharCategory::Other;
}

struct DecodedCp {
    uint32_t cp;
    uint32_t len;
};

// UTF-8: pos がマルチバイト継続バイトを指したら先頭バイトまで戻す。
constexpr uint32_t SnapToCpStart(std::string_view text, uint32_t pos) noexcept
{
    while (pos > 0 && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80) {
        --pos;
    }
    return pos;
}

// UTF-16: pos が low surrogate を指したら直前の high surrogate まで戻す。
constexpr uint32_t SnapToCpStart(std::wstring_view text, uint32_t pos) noexcept
{
    if (pos > 0) {
        const auto c = static_cast<uint16_t>(text[pos]);
        const auto p = static_cast<uint16_t>(text[pos - 1]);
        if (c >= 0xDC00 && c <= 0xDFFF && p >= 0xD800 && p <= 0xDBFF) {
            return pos - 1;
        }
    }
    return pos;
}

// UTF-8 を pos から 1 code point decode。不正バイトは U+FFFD として 1 byte 進める。
constexpr DecodedCp DecodeAt(std::string_view text, uint32_t pos) noexcept
{
    const auto first = static_cast<unsigned char>(text[pos]);
    if (first < 0x80) {
        return { first, 1 };
    }
    uint32_t cp = 0;
    uint32_t len = 0;
    if ((first & 0xE0) == 0xC0) {
        cp = first & 0x1F;
        len = 2;
    }
    else if ((first & 0xF0) == 0xE0) {
        cp = first & 0x0F;
        len = 3;
    }
    else if ((first & 0xF8) == 0xF0) {
        cp = first & 0x07;
        len = 4;
    }
    else {
        return { 0xFFFD, 1 };
    }
    if (static_cast<size_t>(pos) + len > text.size()) {
        return { 0xFFFD, 1 };
    }
    for (uint32_t i = 1; i < len; ++i) {
        const auto b = static_cast<unsigned char>(text[pos + i]);
        if ((b & 0xC0) != 0x80) {
            return { 0xFFFD, 1 };
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    return { cp, len };
}

// UTF-16 を pos から 1 code point decode。サロゲートペアを考慮。
constexpr DecodedCp DecodeAt(std::wstring_view text, uint32_t pos) noexcept
{
    const auto c = static_cast<uint16_t>(text[pos]);
    if (c >= 0xD800 && c <= 0xDBFF && static_cast<size_t>(pos) + 1 < text.size()) {
        const auto c2 = static_cast<uint16_t>(text[pos + 1]);
        if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
            const uint32_t cp = 0x10000u + ((static_cast<uint32_t>(c) - 0xD800u) << 10) + (static_cast<uint32_t>(c2) - 0xDC00u);
            return { cp, 2 };
        }
    }
    return { c, 1 };
}

// pos の直前の code point を decode。pos > 0 が前提。
template <typename SV>
constexpr DecodedCp DecodePrev(SV text, uint32_t pos) noexcept
{
    return DecodeAt(text, SnapToCpStart(text, pos - 1));
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
    pos = SnapToCpStart(text, pos);

    const auto here = DecodeAt(text, pos);
    const auto cat = CategorizeCodePoint(here.cp);
    if (cat == CharCategory::Other) {
        return result;
    }

    uint32_t start = pos;
    while (start > 0) {
        const auto prev = DecodePrev(text, start);
        if (CategorizeCodePoint(prev.cp) != cat) {
            break;
        }
        start -= prev.len;
    }

    uint32_t end = pos + here.len;
    while (end < text.size()) {
        const auto next = DecodeAt(text, end);
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

bool IsMarkdownFile(std::wstring_view path)
{
    // ファイルパス比較は OS API 経由で wstring 固定 (alias 影響外)。
    static constexpr ascii_util::LowercaseAsciiLiteral kMarkdownExts[]{
        L".md",
        L".markdown",
        L".mkd",
    };
    const auto ext = std::filesystem::path(path).extension().wstring();
    return std::ranges::any_of(kMarkdownExts, [&](const auto& e) {
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

namespace {

// GenerateAnchorId の ASCII (0x00–0x7F) 処理を 1 回の lookup で判定するテーブル。
// 値: 0=skip, 1=keep, 2=hyphen, 3=lower (A-Z のみ。実際の変換は c | 0x20 で小文字化)。
enum AsciiSlugAction : uint8_t {
    SlugSkip = 0,
    SlugKeep = 1,
    SlugHyphen = 2,
    SlugLower = 3,
};

constexpr auto kAsciiSlugTable = [] {
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

// CJK ・ 全角文字範囲の句読点・記号はアンカーに含めない。
// code point 単位で判定 (0x3000 以降の Unicode コードポイント)。
constexpr bool IsAnchorSkippableSymbol(uint32_t cp) noexcept
{
    // CJK 記号と句読点 (U+3000–U+303F)
    if (cp <= 0x303F) {
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
        const char* it = text.data();
        const char* const end = it + text.size();
        while (it != end) {
            const auto cu = static_cast<uint32_t>(static_cast<std::make_unsigned_t<char>>(*it));
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
                ++it;
                continue;
            }
            // UTF-8 マルチバイトを decode → code point に対する CJK 判定。
            // 判定が「保持」なら元の UTF-8 byte 列をそのままコピー。
            const unsigned char first = static_cast<unsigned char>(cu);
            uint32_t cp = 0;
            size_t len = 0;
            if ((first & 0xE0) == 0xC0) {
                cp = first & 0x1F;
                len = 2;
            }
            else if ((first & 0xF0) == 0xE0) {
                cp = first & 0x0F;
                len = 3;
            }
            else if ((first & 0xF8) == 0xF0) {
                cp = first & 0x07;
                len = 4;
            }
            else {
                // 不正先頭バイト (continuation 等) はスキップ。
                ++it;
                continue;
            }
            if (static_cast<size_t>(end - it) < len) {
                // truncated。残りスキップ。
                break;
            }
            for (size_t i = 1; i < len; ++i) {
                cp = (cp << 6) | (static_cast<unsigned char>(it[i]) & 0x3F);
            }
            if (cp >= 0x3000 && !IsAnchorSkippableSymbol(cp)) {
                for (size_t i = 0; i < len; ++i) {
                    *dst++ = it[i];
                }
            }
            it += len;
        }
        return static_cast<size_t>(dst - buf);
    });
}

std::pmr::string GenerateAnchorId(std::string_view text)
{
    std::pmr::string slug;
    GenerateAnchorIdInto(text, slug);
    return slug;
}

std::pmr::wstring BuildTitleString(std::wstring_view path, int zoom_percent)
{
    // タイトル文字列はウィンドウタイトル用なので wstring 固定 (alias 影響外)。
    std::pmr::wstring title;
    if (path.empty()) {
        title = L"mendo";
    }
    else {
        const auto filename = ExtractFilename(path);
        title = filename.empty() ? L"mendo" : filename + L" - mendo";
    }
    if (zoom_percent > 0 && zoom_percent != 100) {
        std::format_to(std::back_inserter(title), L" ({}%)", zoom_percent);
    }
    return title;
}
