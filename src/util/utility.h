#pragma once
#include <memory>
#include <memory_resource>
#include <span>
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
