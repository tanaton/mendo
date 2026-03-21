#pragma once
#include <string>
#include <string_view>
#include <cstdint>

namespace mermaid_util {
    // Escape a wstring for safe embedding as a JavaScript string literal.
    std::wstring JsEscape(std::wstring_view input);

    // FNV-1a 64-bit hash, returned as 16-char hex wstring.
    std::wstring SimpleHash(std::wstring_view input);
}
