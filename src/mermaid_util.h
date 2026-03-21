#pragma once
#include <string>
#include <string_view>
#include <cstdint>

namespace mermaid_util {
    // wstringをJavaScript文字列リテラルとして安全に埋め込むためにエスケープする。
    std::wstring JsEscape(std::wstring_view input);

    // FNV-1a 64ビットハッシュ。16文字の16進数wstringとして返す。
    std::wstring SimpleHash(std::wstring_view input);
}
