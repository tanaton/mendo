#pragma once
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <emmintrin.h>
#include <intrin.h>
#include <string>
#include <string_view>
#include <type_traits>

namespace ascii_util {

inline constexpr size_t npos = std::wstring_view::npos;

template <typename Char>
constexpr bool IsAsciiUpper(Char c) noexcept
{
    return c >= static_cast<Char>('A') && c <= static_cast<Char>('Z');
}

template <typename Char>
constexpr bool IsAsciiLower(Char c) noexcept
{
    return c >= static_cast<Char>('a') && c <= static_cast<Char>('z');
}

// std::tolower の locale 依存 (トルコ語の I→ı 等) を回避。
struct ToLowerAsciiFn {
    static constexpr wchar_t operator()(wchar_t c) noexcept
    {
        return IsAsciiUpper(c) ? static_cast<wchar_t>(c - L'A' + L'a') : c;
    }
    static constexpr char operator()(char c) noexcept
    {
        return IsAsciiUpper(c) ? static_cast<char>(c - 'A' + 'a') : c;
    }
};
inline constexpr ToLowerAsciiFn ToLowerAscii{};

template <typename Char>
constexpr bool IsAsciiDigit(Char c) noexcept
{
    return c >= static_cast<Char>('0') && c <= static_cast<Char>('9');
}

template <typename Char>
constexpr bool IsAsciiAlpha(Char c) noexcept
{
    return IsAsciiLower(c) || IsAsciiUpper(c);
}

template <typename Char>
constexpr bool IsAsciiAlnum(Char c) noexcept
{
    return IsAsciiDigit(c) || IsAsciiAlpha(c);
}

// CJK は対象外。
template <typename Char>
constexpr bool IsAsciiWordChar(Char c) noexcept
{
    return IsAsciiAlnum(c) || c == static_cast<Char>('_');
}

namespace detail {

inline constexpr size_t kSimdStep = 16;

// 各レーンに対して 'A'-'Z' なら全 1、それ以外は 0 を立てる比較マスク。
// epi8 は符号付き比較だが ASCII 範囲は正値で問題なし。非 ASCII (signed の負値) は ge_a で必ず弾かれる。
inline __m128i AsciiUpperRangeMask(__m128i c) noexcept
{
    const __m128i ge_a = _mm_cmpgt_epi8(c, _mm_set1_epi8(static_cast<char>('A' - 1)));
    const __m128i le_z = _mm_cmpgt_epi8(_mm_set1_epi8(static_cast<char>('Z' + 1)), c);
    return _mm_and_si128(ge_a, le_z);
}

// ASCII 大文字レーンにだけ 0x20 を載せた加算ベクタ (それ以外は 0)。
inline __m128i AsciiUpperToLowerAdd(__m128i c) noexcept
{
    return _mm_and_si128(AsciiUpperRangeMask(c), _mm_set1_epi8(0x20));
}

} // namespace detail

// ASCII の英字だけを小文字にした写し。
inline std::string ToLowerAsciiCopy(std::string_view text)
{
    std::string result(text);
    std::ranges::transform(result, result.begin(), ToLowerAscii);
    return result;
}

namespace detail {

template <bool kFold>
inline __m128i LoadBytes(const char* p) noexcept
{
    const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    if constexpr (kFold) {
        return _mm_add_epi8(c, AsciiUpperToLowerAdd(c));
    }
    else {
        return c;
    }
}

template <bool kFold>
constexpr char FoldByte(char c) noexcept
{
    if constexpr (kFold) {
        return ToLowerAscii(c);
    }
    else {
        return c;
    }
}

template <bool kFold>
inline bool EqualBytes(const char* t, const char* q, size_t n) noexcept
{
    if constexpr (kFold) {
        for (size_t k = 0; k < n; ++k) {
            if (ToLowerAscii(t[k]) != q[k]) {
                return false;
            }
        }
        return true;
    }
    else {
        return std::memcmp(t, q, n) == 0;
    }
}

// kFold=true のとき query は ASCII 小文字化済みで、text 側をその場で ASCII 小文字化して比較する
// (文書全体の小文字コピーを持たずに済む)。
// 候補は先頭バイトと末尾バイトの 2 点一致で絞る。UTF-8 の日本語は先頭バイトが 0xE3〜0xE9 に
// 集中するため、先頭 1 点だけだと約 3 バイトに 1 回候補が立ち memcmp が支配的になる。
// multi-byte シーケンスの中間バイトとの偶然一致は後続の比較で正確に弾ける。
// 1 バイトのクエリもここを通る。専用のループより 64MB で約 2ms 遅いが、検索は MAX_MATCHES で止まる。
template <bool kFold>
inline size_t FindImpl(std::string_view text, std::string_view query, size_t start) noexcept
{
    const size_t qlen = query.size();
    const size_t tlen = text.size();
    if (qlen == 0) {
        return start <= tlen ? start : npos;
    }
    if (start >= tlen || tlen - start < qlen) {
        return npos;
    }

    const char* tp = text.data();
    const char* qp = query.data();
    const char first = qp[0];
    const __m128i v_first = _mm_set1_epi8(first);
    const size_t last = tlen - qlen;

    size_t i = start;

    const size_t tail = qlen - 1;
    const __m128i v_last = _mm_set1_epi8(qp[tail]);
    // kSimdStep 個の開始候補を 1 ブロックで判定する。末尾側のロードが text 内に収まる範囲。
    while (i + kSimdStep <= last + 1) {
        const __m128i eq_first = _mm_cmpeq_epi8(LoadBytes<kFold>(tp + i), v_first);
        const __m128i eq_last = _mm_cmpeq_epi8(LoadBytes<kFold>(tp + i + tail), v_last);
        unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(_mm_and_si128(eq_first, eq_last)));
        while (mask != 0) {
            unsigned long bit_idx;
            _BitScanForward(&bit_idx, mask);
            mask &= mask - 1;
            const size_t pos = i + bit_idx;
            if (EqualBytes<kFold>(tp + pos + 1, qp + 1, tail)) {
                return pos;
            }
        }
        i += kSimdStep;
    }
    for (; i <= last; ++i) {
        if (FoldByte<kFold>(tp[i]) == first && FoldByte<kFold>(tp[i + tail]) == qp[tail] &&
            EqualBytes<kFold>(tp + i + 1, qp + 1, tail)) {
            return i;
        }
    }
    return npos;
}

} // namespace detail

inline size_t Find(std::string_view text, std::string_view query, size_t start) noexcept
{
    return detail::FindImpl<false>(text, query, start);
}

// ASCII の大文字小文字を区別しない検索。lower_query は ASCII 小文字化済みであること。
inline size_t FindAsciiCaseInsensitive(std::string_view text, std::string_view lower_query, size_t start) noexcept
{
    return detail::FindImpl<true>(text, lower_query, start);
}

// ASCII 英字を含むか。含まなければ ASCII 大文字小文字無視の検索は通常検索と同一の結果になる。
inline bool HasAsciiLetter(std::string_view s) noexcept
{
    return std::ranges::any_of(s, IsAsciiAlpha<char>);
}

// コンパイル時に契約違反を検出する (定数評価中に throw へ到達するとコンパイルエラーになる)。
//  - NUL 終端 (literal[N-1] == '\0')。
//  - 全文字が ASCII 範囲 (<= 0x7F)。
//  - 大文字 'A'-'Z' を含まない (RHS は小文字確定でなければならない)。
template <typename CharT>
struct BasicLowercaseAsciiLiteral {
    std::basic_string_view<CharT> value;

    template <size_t N>
    consteval BasicLowercaseAsciiLiteral(const CharT (&literal)[N]) noexcept
        : value(literal, N - 1)
    {
        if (literal[N - 1] != CharT{}) {
            throw "LowercaseAsciiLiteral: literal must be NUL-terminated";
        }
        for (size_t i = 0; i < N - 1; ++i) {
            // char (signed) で 0x80+ は負値になるので unsigned 変換で判定。
            if (static_cast<std::make_unsigned_t<CharT>>(literal[i]) > 0x7F) {
                throw "LowercaseAsciiLiteral: literal must contain only ASCII characters";
            }
            if (IsAsciiUpper(literal[i])) {
                throw "LowercaseAsciiLiteral: literal must be lowercase ASCII";
            }
        }
    }
};

using LowercaseAsciiLiteral = BasicLowercaseAsciiLiteral<wchar_t>;
using DocLowercaseLiteral = BasicLowercaseAsciiLiteral<char>;

// LHS のみ projection で小文字化する高速版。
constexpr bool iequal(std::wstring_view a, LowercaseAsciiLiteral b) noexcept
{
    return std::ranges::equal(a, b.value, {}, ToLowerAscii);
}

constexpr bool iequal(std::string_view a, DocLowercaseLiteral b) noexcept
{
    return std::ranges::equal(a, b.value, {}, ToLowerAscii);
}

constexpr bool istarts_with(std::string_view s, DocLowercaseLiteral prefix) noexcept
{
    return std::ranges::starts_with(s, prefix.value, {}, ToLowerAscii);
}

} // namespace ascii_util
