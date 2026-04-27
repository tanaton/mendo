#include "html_entity.h"
#include <charconv>
#include <system_error>

std::optional<std::wstring_view> ResolveHtmlEntity(std::string_view entity, wchar_t (&buffer)[2])
{
    const wchar_t* literal = nullptr;
    if (entity == "&amp;") {
        literal = L"&";
    }
    else if (entity == "&lt;") {
        literal = L"<";
    }
    else if (entity == "&gt;") {
        literal = L">";
    }
    else if (entity == "&quot;") {
        literal = L"\"";
    }
    else if (entity == "&apos;") {
        literal = L"'";
    }
    else if (entity == "&nbsp;") {
        literal = L"\u00A0";
    }
    if (literal) {
        return std::wstring_view{ literal, 1 };
    }

    if (entity.size() >= 4 && entity[0] == '&' && entity[1] == '#') {
        unsigned long codepoint = 0;
        bool valid = false;
        if (entity[2] == 'x' || entity[2] == 'X') {
            const auto r = std::from_chars(entity.data() + 3, entity.data() + entity.size() - 1, codepoint, 16);
            valid = (r.ec == std::errc());
        }
        else {
            const auto r = std::from_chars(entity.data() + 2, entity.data() + entity.size() - 1, codepoint, 10);
            valid = (r.ec == std::errc());
        }
        // サロゲート範囲 (U+D800-U+DFFF) は単独で UTF-16 として不正なので除外し、
        // 呼び出し側で元の utf-8 をそのまま再投入させる。
        if (valid && codepoint > 0 && codepoint <= 0xFFFF &&
            !(codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            buffer[0] = static_cast<wchar_t>(codepoint);
            return std::wstring_view{ buffer, 1 };
        }
        if (valid && codepoint > 0xFFFF && codepoint <= 0x10FFFF) {
            // 補助面: UTF-16 サロゲートペア
            const unsigned long adj = codepoint - 0x10000;
            buffer[0] = static_cast<wchar_t>(0xD800 + (adj >> 10));
            buffer[1] = static_cast<wchar_t>(0xDC00 + (adj & 0x3FF));
            return std::wstring_view{ buffer, 2 };
        }
    }

    return std::nullopt;
}
