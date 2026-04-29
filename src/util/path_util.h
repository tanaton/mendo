#pragma once
#include <windows.h>
#include <string_view>

// Windows ファイル名・フルパス比較ユーティリティ。
// `CompareStringOrdinal(..., TRUE)` ベースで NTFS と挙動が一致する
// Unicode 込みの ordinal case-insensitive 比較を提供する。
// 拡張子や URL スキーム等の ASCII 確定トークンには ascii_util::iequal を使う。

namespace path_util {

inline bool iequal(std::wstring_view a, std::wstring_view b) noexcept
{
    return ::CompareStringOrdinal(
        a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(b.size()),
        TRUE) == CSTR_EQUAL;
}

inline bool iless(std::wstring_view a, std::wstring_view b) noexcept
{
    return ::CompareStringOrdinal(
        a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(b.size()),
        TRUE) == CSTR_LESS_THAN;
}

} // namespace path_util
