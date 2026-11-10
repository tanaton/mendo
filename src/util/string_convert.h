#pragma once
#include "profiler.h"
#include <limits>
#include <string>
#include <string_view>
#include <windows.h>

namespace string_convert {

inline std::wstring Utf8ToWide(std::string_view utf8)
{
    MENDO_PROFILE("Utf8ToWide");
    std::wstring out;
    if (utf8.empty()) {
        return out;
    }
    // wchar 数 <= UTF-8 byte 数 が常に成立するため、上限確保で API 1 回呼び出し
    out.resize_and_overwrite(utf8.size(), [utf8](wchar_t* buf, size_t count) -> size_t {
        const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), buf, static_cast<int>(count));
        return n > 0 ? static_cast<size_t>(n) : 0;
    });
    return out;
}

inline std::string WideToUtf8(std::wstring_view wide)
{
    std::string out;
    if (wide.empty() || wide.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return out;
    }
    // 上限 (wchar 数 * 3) で確保すると、MSVC の resize_and_overwrite は縮めないので ASCII では容量が 3 倍のまま残る
    // (図のキャッシュの SVG 本文が、予算に数える大きさの 3 倍のヒープを持っていた)。先に長さを問い合わせる。
    const int src_len = static_cast<int>(wide.size());
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), src_len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return out;
    }
    out.resize_and_overwrite(static_cast<size_t>(n), [&](char* buf, size_t count) -> size_t {
        return WideCharToMultiByte(CP_UTF8, 0, wide.data(), src_len, buf, n, nullptr, nullptr) == n ? count : 0;
    });
    return out;
}

} // namespace string_convert
