#pragma once
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

// inner が outer の範囲に収まるか。md4c が渡す静的リテラルのように別の配列を指しうるポインタ同士は、
// 減算が UB で大小比較 (std::less<T*> を含む) もアドレスの数値順と一致する保証が無いため、整数で比べる。
inline bool IsSubrangeOf(std::string_view inner, std::string_view outer) noexcept
{
    const auto p = reinterpret_cast<uintptr_t>(inner.data());
    const auto base = reinterpret_cast<uintptr_t>(outer.data());
    return p >= base && p + inner.size() <= base + outer.size();
}

// optional に確保される pmr::vector を std::span として安全に view する。
// 空 vector ヘッダ (24B/個) を全要素分背負わないため
// `unique_ptr<pmr::vector<T>>` として保持しているメンバを、呼び出し側で
// nullptr 分岐なしに走査できるようにする。
template <class T>
constexpr std::span<const T> SpanOrEmpty(const std::unique_ptr<std::pmr::vector<T>>& p) noexcept
{
    return p ? std::span<const T>{ *p } : std::span<const T>{};
}
