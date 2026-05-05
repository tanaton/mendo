#include "document_utils.h"
#include "ascii_util.h"
#include <array>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iterator>
#include <ranges>

mendo::doc_string ToLowerAscii(mendo::doc_string_view text)
{
    mendo::doc_string result;
    result.resize_and_overwrite(text.size(), [&text](mendo::doc_char* buf, size_t count) noexcept -> size_t {
        ascii_util::AsciiToLowerOnly(text.data(), buf, count);
        return count;
    });
    return result;
}

WordBoundary FindWordBoundaries(mendo::doc_string_view text, uint32_t pos) noexcept
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
    const auto is_word_char = [](mendo::doc_char c) static noexcept {
        return (c >= MENDO_LIT('0') && c <= MENDO_LIT('9')) || (c >= MENDO_LIT('A') && c <= MENDO_LIT('Z')) || (c >= MENDO_LIT('a') && c <= MENDO_LIT('z')) || c == MENDO_LIT('_');
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
        const mendo::doc_char c = static_cast<mendo::doc_char>(i);
        if ((c >= MENDO_LIT('a') && c <= MENDO_LIT('z')) || ascii_util::IsAsciiDigit(c) || c == MENDO_LIT('-') || c == MENDO_LIT('_')) {
            t[i] = SlugKeep;
        }
        else if (c >= MENDO_LIT('A') && c <= MENDO_LIT('Z')) {
            t[i] = SlugLower;
        }
        else if (c == MENDO_LIT(' ') || c == MENDO_LIT('\t')) {
            t[i] = SlugHyphen;
        }
        else {
            t[i] = SlugSkip;
        }
    }
    return t;
}();

// CJK ・ 全角文字範囲の句読点・記号はアンカーに含めない。
// 注意: 0x3000 以降の判定は UTF-16 code unit ベース。UTF-8 ビルド切替時 (C7 以降) は
// バイト単位の判定にならないため、UTF-8 multi-byte 文字を意識した実装に置換が必要。
constexpr bool IsAnchorSkippableSymbol(mendo::doc_char c) noexcept
{
    // CJK 記号と句読点 (U+3000–U+303F)
    if (c <= 0x303F) {
        return true;
    }
    // 全角 ASCII 対応の記号関連
    return (c >= 0xFF01 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20) || (c >= 0xFF3B && c <= 0xFF40) || (c >= 0xFF5B && c <= 0xFF65);
}

} // namespace

void GenerateAnchorIdInto(mendo::doc_string_view text, mendo::doc_string& slug)
{
    slug.clear();
    // 出力は入力サイズ以下で確定のため一括確保して書き出す。
    slug.resize_and_overwrite(text.size(), [text](mendo::doc_char* buf, size_t /*count*/) noexcept -> size_t {
        mendo::doc_char* dst = buf;
        for (const mendo::doc_char c : text) {
            if (c < 0x80) {
                // ASCII ファストパス: 1 回の table lookup で分岐を絞る。
                switch (kAsciiSlugTable[static_cast<size_t>(c)]) {
                case SlugKeep:
                    *dst++ = c;
                    break;
                case SlugLower:
                    // A-Z になることは table で保証済み。
                    *dst++ = static_cast<mendo::doc_char>(c | 0x20);
                    break;
                case SlugHyphen:
                    *dst++ = MENDO_LIT('-');
                    break;
                case SlugSkip:
                    break;
                default:
                    std::unreachable();
                }
            }
            else if (c >= 0x3000) {
                // CJK 文字・全角文字を判定。句読点・記号だけ除外して残す。
                if (!IsAnchorSkippableSymbol(c)) {
                    *dst++ = c;
                }
            }
            // 0x80–0x2FFF はスキップ。
        }
        return static_cast<size_t>(dst - buf);
    });
}

mendo::doc_string GenerateAnchorId(mendo::doc_string_view text)
{
    mendo::doc_string slug;
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
