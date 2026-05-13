#pragma once
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace mendo {

inline constexpr std::uint64_t kFnv1a64OffsetBasis = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnv1a64Prime = 1099511628211ULL;

constexpr std::uint64_t Fnv1a64Update(std::uint64_t h, std::uint64_t byte) noexcept
{
    return (h ^ byte) * kFnv1a64Prime;
}

// 短い文字列に対して衝突分布が良好な 64bit ハッシュ。
// N=10^4 規模で衝突確率 ~10^-12 と実質ゼロ。
// 入力は 1 文字単位 (char は 0x00-0xFF、wchar_t は UTF-16 code unit) を取り込むため、
// 同一文字列でも CharT が異なれば hash 値は一致しない。両者を同じキー空間で混用しないこと。
template <typename CharT>
    requires std::is_same_v<CharT, char> || std::is_same_v<CharT, wchar_t>
constexpr std::uint64_t Fnv1a64(std::basic_string_view<CharT> sv) noexcept
{
    std::uint64_t h = kFnv1a64OffsetBasis;
    for (CharT c : sv) {
        h = Fnv1a64Update(h, static_cast<std::uint64_t>(std::make_unsigned_t<CharT>(c)));
    }
    return h;
}

} // namespace mendo
