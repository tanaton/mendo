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

WordBoundary FindWordBoundaries(std::string_view text, uint32_t pos) noexcept
{
    WordBoundary result;
    if (text.empty()) {
        return result;
    }
    if (pos >= text.size()) {
        pos = static_cast<uint32_t>(text.size()) - 1;
    }

    // ダブルクリック単語選択では ASCII 英数 + '_' のみを単語構成文字とする。
    // CJK 文字は個別の文字として扱い選択対象外（既存 UX を維持）。
    const auto is_word_char = [](char c) static noexcept {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    };

    if (!is_word_char(text[pos])) {
        return result;
    }

    uint32_t word_start = pos;
    while (word_start > 0 && is_word_char(text[word_start - 1])) {
        word_start--;
    }

    uint32_t word_end = pos + 1;
    while (word_end < text.size() && is_word_char(text[word_end])) {
        word_end++;
    }

    result.start = word_start;
    result.end = word_end;
    result.found = true;
    return result;
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
