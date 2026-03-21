#pragma once
#include <string>
#include <string_view>
#include <cstdint>

namespace mermaid_util {
    // wstringをJavaScript文字列リテラルとして安全に埋め込むためにエスケープする。
    std::pmr::wstring JsEscape(std::wstring_view input);

    // FNV-1a 64ビットハッシュ。16文字の16進数wstringとして返す。
    std::pmr::wstring SimpleHash(std::wstring_view input);
}
