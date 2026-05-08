#pragma once
#include <format>
#include <iterator>
#include <memory_resource>
#include <string>

// std::pmr::wstring を返す std::format ラッパー。format_to + back_inserter で
// 一回の format_to_string 呼び出しを pmr アロケータで賄う。
template <typename... Args>
inline std::pmr::wstring PmrFormat(std::wformat_string<Args...> fmt, Args&&... args)
{
    std::pmr::wstring result;
    std::format_to(std::back_inserter(result), fmt, std::forward<Args>(args)...);
    return result;
}
