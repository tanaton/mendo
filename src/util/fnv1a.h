#pragma once
#include <cstdint>
#include <string_view>

namespace mendo {

inline constexpr std::uint64_t kFnv1a64OffsetBasis = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnv1a64Prime = 1099511628211ULL;

constexpr std::uint64_t Fnv1a64Update(std::uint64_t h, std::uint64_t byte) noexcept
{
    return (h ^ byte) * kFnv1a64Prime;
}

// 短い ASCII/UTF-8 文字列に対して衝突分布が良好な 64bit ハッシュ。
// N=10^4 規模で衝突確率 ~10^-12 と実質ゼロ。
constexpr std::uint64_t Fnv1a64(std::string_view sv) noexcept
{
    std::uint64_t h = kFnv1a64OffsetBasis;
    for (unsigned char c : sv) {
        h = Fnv1a64Update(h, c);
    }
    return h;
}

// UTF-16 (wchar_t = 16bit on Windows) を 1 単位ずつ取り込む。
// byte 単位の Fnv1a64(string_view) とは hash 値が一致しないため、両者を同じキー空間で
// 混用してはならない。
constexpr std::uint64_t Fnv1a64(std::wstring_view sv) noexcept
{
    std::uint64_t h = kFnv1a64OffsetBasis;
    for (wchar_t c : sv) {
        h = Fnv1a64Update(h, static_cast<std::uint64_t>(c));
    }
    return h;
}

} // namespace mendo
