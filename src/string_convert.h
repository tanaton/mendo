#pragma once
#include <string>
#include <string_view>
#include <memory_resource>
#include <windows.h>

namespace string_convert {

inline std::pmr::wstring Utf8ToWide(std::string_view utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wlen <= 0) {
        return {};
    }
    std::pmr::wstring result;
    result.resize_and_overwrite(static_cast<size_t>(wlen), [utf8](wchar_t* buf, size_t count) -> size_t {
        return static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), buf, static_cast<int>(count)));
    });
    return result;
}

inline std::string WideToUtf8(std::wstring_view wide)
{
    if (wide.empty()) {
        return {};
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        return {};
    }
    std::string result;
    result.resize_and_overwrite(static_cast<size_t>(len), [wide](char* buf, size_t count) -> size_t {
        return static_cast<size_t>(WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), buf, static_cast<int>(count), nullptr, nullptr));
    });
    return result;
}

} // namespace string_convert
