#pragma once
#include <cstdint>
#include <string_view>

// inner が outer の範囲に収まるか。md4c が渡す静的リテラルのように別の配列を指しうるポインタ同士は、
// 減算が UB で大小比較 (std::less<T*> を含む) もアドレスの数値順と一致する保証が無いため、整数で比べる。
inline bool IsSubrangeOf(std::string_view inner, std::string_view outer) noexcept
{
    const auto p = reinterpret_cast<uintptr_t>(inner.data());
    const auto base = reinterpret_cast<uintptr_t>(outer.data());
    return p >= base && p + inner.size() <= base + outer.size();
}
