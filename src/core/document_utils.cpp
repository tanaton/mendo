#include "document_utils.h"
#include "ascii_util.h"
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <format>
#include <iterator>

std::pmr::wstring ToLowerAscii(std::wstring_view text)
{
    std::pmr::wstring result;
    result.resize_and_overwrite(text.size(), [&text](wchar_t* buf, size_t count) noexcept -> size_t {
        ascii_util::AsciiToLowerOnly(text.data(), buf, count);
        return count;
    });
    return result;
}

WordBoundary FindWordBoundaries(std::wstring_view text, uint32_t pos) noexcept
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
    const auto is_word_char = [](wchar_t c) static noexcept {
        return (c >= L'0' && c <= L'9')
            || (c >= L'A' && c <= L'Z')
            || (c >= L'a' && c <= L'z')
            || c == L'_';
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
    auto ext = std::filesystem::path(path).extension().wstring();
    for (auto& c : ext) {
        c = std::towlower(c);
    }
    return ext == L".md" || ext == L".markdown" || ext == L".mkd";
}

std::pmr::wstring ExtractFilename(std::wstring_view path)
{
    if (path.empty()) {
        return {};
    }
    return std::pmr::wstring{ std::filesystem::path(path).filename().native() };
}

std::pmr::wstring BuildTitleString(std::wstring_view path, int zoom_percent)
{
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
