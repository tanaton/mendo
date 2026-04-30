#pragma once
#include <format>
#include <functional>
#include <iterator>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <cstddef>
#include <windows.h>

// std::visit 用のオーバーロードヘルパー
template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };

// pmr::wstring と wstring_view を等価にハッシュする透過ハッシャ。
// equal_to<> と組み合わせて unordered_map に渡すと wstring_view からの lookup で
// 一時 pmr::wstring の確保をスキップできる。
struct WStringTransparentHash {
    using is_transparent = void;
    size_t operator()(std::wstring_view sv) const noexcept
    {
        return std::hash<std::wstring_view>{}(sv);
    }
    size_t operator()(const std::pmr::wstring& s) const noexcept
    {
        return std::hash<std::wstring_view>{}(s);
    }
};

// Win32リソース（RCDATA）からバイト列を取得する。
// リソースはプロセスのアドレス空間にマップされておりコピー不要。
inline std::span<const std::byte> LoadRcData(UINT resource_id) noexcept
{
    const HRSRC hRes = FindResourceW(nullptr, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (!hRes) {
        return {};
    }
    const HGLOBAL hData = LoadResource(nullptr, hRes);
    if (!hData) {
        return {};
    }
    const DWORD size = SizeofResource(nullptr, hRes);
    const auto* data = static_cast<const std::byte*>(LockResource(hData));
    if (!data || size == 0) {
        return {};
    }
    return { data, size };
}

// std::pmr::wstring を返す std::format ラッパー
template <typename... Args>
std::pmr::wstring PmrFormat(std::wformat_string<Args...> fmt, Args&&... args)
{
    std::pmr::wstring result;
    std::format_to(std::back_inserter(result), fmt, std::forward<Args>(args)...);
    return result;
}
