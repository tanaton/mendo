#pragma once
#include <memory>
#include <memory_resource>
#include <span>
#include <vector>

// optional に確保される pmr::vector を std::span として安全に view する。
// 空 vector ヘッダ (24B/個) を全要素分背負わないため
// `unique_ptr<pmr::vector<T>>` として保持しているメンバを、呼び出し側で
// nullptr 分岐なしに走査できるようにする。
template <class T>
constexpr std::span<const T> SpanOrEmpty(const std::unique_ptr<std::pmr::vector<T>>& p) noexcept
{
    return p ? std::span<const T>{ *p } : std::span<const T>{};
}
