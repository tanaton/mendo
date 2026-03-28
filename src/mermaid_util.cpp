#include "mermaid_util.h"

std::pmr::wstring mermaid_util::JsEscape(std::wstring_view input)
{
    std::pmr::wstring result;
    result.reserve(input.size() + input.size() / 4);
    for (wchar_t c : input) {
        switch (c) {
        case L'\\': result += L"\\\\"; break;
        case L'\'': result += L"\\'"; break;
        case L'"':  result += L"\\\""; break;
        case L'\n': result += L"\\n"; break;
        case L'\r': result += L"\\r"; break;
        case L'\t': result += L"\\t"; break;
        case L'`':  result += L"\\`"; break;
        case L'$':  result += L"\\$"; break;
        default:
            if (c < 0x20 || c == 0x2028 || c == 0x2029) {
                // 制御文字およびJS文字列リテラルで特殊なU+2028/U+2029をエスケープ
                wchar_t buf[8];
                swprintf_s(buf, L"\\u%04x", static_cast<unsigned>(c));
                result += buf;
            }
            else {
                result += c;
            }
            break;
        }
    }
    return result;
}

uint64_t mermaid_util::HashRaw(std::wstring_view input)
{
    uint64_t hash = 14695981039346656037ULL;
    for (wchar_t c : input) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::pmr::wstring mermaid_util::SimpleHash(std::wstring_view input)
{
    wchar_t buf[20];
    swprintf_s(buf, L"%016llx", HashRaw(input));
    return buf;
}

std::pmr::wstring mermaid_util::CombinedHash(std::wstring_view code, int max_width_int, bool dark_mode)
{
    // コード全体をコピーせず、直接ハッシュして幅・モードをミックスする
    uint64_t h = HashRaw(code);
    h ^= static_cast<uint64_t>(max_width_int) * 1099511628211ULL;
    h ^= static_cast<uint64_t>(dark_mode) * 2654435761ULL;
    wchar_t buf[20];
    swprintf_s(buf, L"%016llx", h);
    return buf;
}
