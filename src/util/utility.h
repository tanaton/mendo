#pragma once
#include <functional>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// optional に確保される pmr::vector を std::span として安全に view する。
// 空 vector ヘッダ (24B/個) を全要素分背負わないため
// `unique_ptr<pmr::vector<T>, Deleter>` として保持しているメンバを、呼び出し側で
// nullptr 分岐なしに走査できるようにする。Deleter は型推論パラメータなので
// std::unique_ptr / mendo::pmr_unique_ptr のどちらでも受けられる。
template <class T, class Deleter>
constexpr std::span<const T> SpanOrEmpty(const std::unique_ptr<std::pmr::vector<T>, Deleter>& p) noexcept
{
    return p ? std::span<const T>{ *p } : std::span<const T>{};
}

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
