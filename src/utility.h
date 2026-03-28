#pragma once
#include <format>
#include <iterator>
#include <memory_resource>

// std::visit 用のオーバーロードヘルパー
template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };

// std::pmr::wstring を返す std::format ラッパー
template <typename... Args>
std::pmr::wstring PmrFormat(std::wformat_string<Args...> fmt, Args&&... args)
{
    std::pmr::wstring result;
    std::format_to(std::back_inserter(result), fmt, std::forward<Args>(args)...);
    return result;
}
