#pragma once
#include <format>
#include <iterator>
#include <memory_resource>
#include <string>

template <typename... Args>
inline std::pmr::wstring PmrFormat(std::wformat_string<Args...> fmt, Args&&... args)
{
    std::pmr::wstring result;
    std::format_to(std::back_inserter(result), fmt, std::forward<Args>(args)...);
    return result;
}
