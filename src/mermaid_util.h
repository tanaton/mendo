#pragma once
#include <string>
#include <cstdint>

namespace mermaid_util {
    // Escape a wstring for safe embedding as a JavaScript string literal.
    std::wstring JsEscape(const std::wstring& input);

    // FNV-1a 64-bit hash, returned as 16-char hex wstring.
    std::wstring SimpleHash(const std::wstring& input);
}
