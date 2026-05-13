#pragma once
#include "doc_text.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <concepts>
#include <emmintrin.h>
#include <intrin.h>
#include <string_view>
#include <type_traits>

// ASCII 文字列ヘルパ。Document テキスト (UTF-8 char) と OS API 経路 (wchar_t、ファイル
// パス比較等) の両方に対応。バルク処理は SSE2 で 16 byte / 8 wchar_t 並列、非 ASCII を
// 含むチャンクはフォールバック (UTF-8 multi-byte / サロゲートペア境界を破壊しない)。
// 1 文字版 helper はスカラ constexpr。
namespace ascii_util {

inline constexpr size_t npos = static_cast<size_t>(-1);

// 1 文字 ASCII case 変換ヘルパ。`std::tolower` の locale 依存
// (トルコ語の I → ı 等) を避けたい用途用。非 ASCII および ASCII 小文字は素通し。
// 関数オブジェクトにすることで std::ranges アルゴリズムの projection に直接渡せる。
struct ToLowerAsciiFn {
    static constexpr wchar_t operator()(wchar_t c) noexcept
    {
        return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
    }
    static constexpr char operator()(char c) noexcept
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
};
inline constexpr ToLowerAsciiFn ToLowerAscii{};

template <typename Char>
constexpr Char ToUpperAscii(Char c) noexcept
{
    return (c >= static_cast<Char>('a') && c <= static_cast<Char>('z')) ? static_cast<Char>(c - static_cast<Char>('a') + static_cast<Char>('A')) : c;
}

// 純粋な ASCII 範囲の文字種判定。locale や CJK の影響を受けない。
template <typename Char>
constexpr bool IsAsciiDigit(Char c) noexcept
{
    return c >= static_cast<Char>('0') && c <= static_cast<Char>('9');
}

template <typename Char>
constexpr bool IsAsciiHexDigit(Char c) noexcept
{
    return IsAsciiDigit(c) ||
           (c >= static_cast<Char>('a') && c <= static_cast<Char>('f')) ||
           (c >= static_cast<Char>('A') && c <= static_cast<Char>('F'));
}

// ダブルクリック単語選択の単語構成文字。ASCII 英数 + '_'。CJK は対象外。
template <typename Char>
constexpr bool IsAsciiWordChar(Char c) noexcept
{
    return IsAsciiDigit(c) ||
           (c >= static_cast<Char>('a') && c <= static_cast<Char>('z')) ||
           (c >= static_cast<Char>('A') && c <= static_cast<Char>('Z')) ||
           c == static_cast<Char>('_');
}

namespace detail {

// 1 SIMD レジスタあたりの要素数 (wchar_t=8, char=16)。
template <typename CharT>
inline constexpr size_t kSimdStep = 16 / sizeof(CharT);

// 8 wchar_t 入りベクタに非 ASCII (>= 0x80) が含まれているか
inline bool HasNonAscii(__m128i c) noexcept
{
    const __m128i high = _mm_and_si128(c, _mm_set1_epi16(static_cast<short>(0xFF80)));
    const __m128i is_ascii = _mm_cmpeq_epi16(high, _mm_setzero_si128());
    return _mm_movemask_epi8(is_ascii) != 0xFFFF;
}

// 各レーンに対して 'A'-'Z' なら全 1、それ以外は 0 を立てる比較マスク。
// epi16 (wchar_t) と epi8 (char) は符号付き比較だが ASCII 範囲は正値で問題なし。
// 非 ASCII (wchar_t は 0x8000+、char は signed の負値) は ge_a で必ず弾かれる。
template <typename CharT>
inline __m128i AsciiUpperRangeMask(__m128i c) noexcept
{
    if constexpr (sizeof(CharT) == 2) {
        const __m128i ge_a = _mm_cmpgt_epi16(c, _mm_set1_epi16(L'A' - 1));
        const __m128i le_z = _mm_cmpgt_epi16(_mm_set1_epi16(L'Z' + 1), c);
        return _mm_and_si128(ge_a, le_z);
    }
    else {
        const __m128i ge_a = _mm_cmpgt_epi8(c, _mm_set1_epi8(static_cast<char>('A' - 1)));
        const __m128i le_z = _mm_cmpgt_epi8(_mm_set1_epi8(static_cast<char>('Z' + 1)), c);
        return _mm_and_si128(ge_a, le_z);
    }
}

// ASCII 大文字レーンにだけ 0x20 を載せた加算ベクタ (それ以外は 0)。
template <typename CharT>
inline __m128i AsciiUpperToLowerAdd(__m128i c) noexcept
{
    if constexpr (sizeof(CharT) == 2) {
        return _mm_and_si128(AsciiUpperRangeMask<CharT>(c), _mm_set1_epi16(0x20));
    }
    else {
        return _mm_and_si128(AsciiUpperRangeMask<CharT>(c), _mm_set1_epi8(0x20));
    }
}

// ASCII 大文字の有無を 1bit にまとめた movemask。値が非 0 なら最低 1 レーンが大文字。
template <typename CharT>
inline unsigned AsciiUpperMask(__m128i c) noexcept
{
    return static_cast<unsigned>(_mm_movemask_epi8(AsciiUpperRangeMask<CharT>(c)));
}

} // namespace detail

// 全文字を小文字化する。ASCII チャンクは SSE2 高速パス、非 ASCII チャンクは
// std::towlower にフォールバック。dst と src は同サイズで src と重ならないこと。
// ASCII 部分は SIMD/スカラ どちらの経路を通っても 'A'-'Z'→'a'-'z' に揃える
// (towlower は locale 依存で、トルコ語では 'I'→'ı'(U+0131) になり得るため明示変換)。
inline void ToLower(const wchar_t* src, wchar_t* dst, size_t n) noexcept
{
    const auto lower_one = [](wchar_t ch) noexcept -> wchar_t {
        if (ch >= L'A' && ch <= L'Z') {
            return static_cast<wchar_t>(ch - L'A' + L'a');
        }
        if (static_cast<unsigned>(ch) < 0x80) {
            return ch;
        }
        return static_cast<wchar_t>(std::towlower(ch));
    };
    size_t i = 0;
    while (i + 8 <= n) {
        const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        if (!detail::HasNonAscii(c)) {
            const __m128i r = _mm_add_epi16(c, detail::AsciiUpperToLowerAdd<wchar_t>(c));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), r);
        }
        else {
            for (size_t k = 0; k < 8; ++k) {
                dst[i + k] = lower_one(src[i + k]);
            }
        }
        i += 8;
    }
    for (; i < n; ++i) {
        dst[i] = lower_one(src[i]);
    }
}

// ASCII 大文字 'A'-'Z' のみ小文字化し、それ以外はそのままコピーする。
// シンタックスハイライタの ASCII キーワード正規化用 (towlower の locale 動作は不要)。
// char 版 (UTF-8) では continuation byte (10xxxxxx) は signed 比較で必ず弾かれるため
// multi-byte シーケンスを破壊しない。
template <typename CharT>
inline void AsciiToLowerOnly(const CharT* src, CharT* dst, size_t n) noexcept
{
    constexpr size_t kStep = detail::kSimdStep<CharT>;
    size_t i = 0;
    while (i + kStep <= n) {
        const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        const __m128i add = detail::AsciiUpperToLowerAdd<CharT>(c);
        __m128i r;
        if constexpr (sizeof(CharT) == 2) {
            r = _mm_add_epi16(c, add);
        }
        else {
            r = _mm_add_epi8(c, add);
        }
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), r);
        i += kStep;
    }
    for (; i < n; ++i) {
        const CharT ch = src[i];
        dst[i] = (ch >= static_cast<CharT>('A') && ch <= static_cast<CharT>('Z'))
                     ? static_cast<CharT>(ch - static_cast<CharT>('A') + static_cast<CharT>('a'))
                     : ch;
    }
}

// ASCII 大文字 'A'-'Z' を一文字でも含むなら true。
// UTF-8 continuation byte (>= 0x80) は signed では負値で AsciiUpperMask が必ず弾く。
template <typename CharT>
inline bool HasAsciiUpper(const CharT* s, size_t n) noexcept
{
    constexpr size_t kStep = detail::kSimdStep<CharT>;
    size_t i = 0;
    while (i + kStep <= n) {
        const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i));
        if (detail::AsciiUpperMask<CharT>(c) != 0) {
            return true;
        }
        i += kStep;
    }
    for (; i < n; ++i) {
        const CharT ch = s[i];
        if (ch >= static_cast<CharT>('A') && ch <= static_cast<CharT>('Z')) {
            return true;
        }
    }
    return false;
}

// std::wstring_view::find 互換 (start 既定 0)。
// query の先頭文字を SSE2 でブロードキャスト比較し、合致候補だけ wmemcmp で詳細比較する。
// query が空の場合の戻り値は std::basic_string_view::find と同じ意味論
// (start <= text.size() なら start、超えていれば npos)。
inline size_t Find(std::wstring_view text, std::wstring_view query, size_t start = 0) noexcept
{
    const size_t qlen = query.size();
    const size_t tlen = text.size();
    if (qlen == 0) {
        return start <= tlen ? start : npos;
    }
    if (start >= tlen || tlen - start < qlen) {
        return npos;
    }

    const wchar_t* tp = text.data();
    const wchar_t* qp = query.data();
    const wchar_t first = qp[0];
    const __m128i bcast = _mm_set1_epi16(static_cast<short>(first));
    const size_t last = tlen - qlen; // 最後の有効な開始位置 (start <= last は保証済み)

    size_t i = start;

    // qlen == 1 専用ハイパス。最初のヒットで即 return できるため 16 wchar_t
    // (128-bit × 2) アンロールし、ループ判定は OR 後の単一 movemask で行う。
    // MSVC STL の wmemchr 系最適化と互角の速度を得るためのホットパス。
    if (qlen == 1) {
        while (i + 16 <= tlen) {
            const __m128i c0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(tp + i));
            const __m128i c1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(tp + i + 8));
            const __m128i eq0 = _mm_cmpeq_epi16(c0, bcast);
            const __m128i eq1 = _mm_cmpeq_epi16(c1, bcast);
            const __m128i any = _mm_or_si128(eq0, eq1);
            if (_mm_movemask_epi8(any) != 0) {
                const unsigned m0 = static_cast<unsigned>(_mm_movemask_epi8(eq0)) & 0x5555u;
                unsigned long bit_idx;
                if (m0 != 0) {
                    _BitScanForward(&bit_idx, m0);
                    return i + (bit_idx / 2);
                }
                // any != 0 かつ m0 == 0 なので m1 は必ず非零。
                const unsigned m1 = static_cast<unsigned>(_mm_movemask_epi8(eq1)) & 0x5555u;
                _BitScanForward(&bit_idx, m1);
                return i + 8 + (bit_idx / 2);
            }
            i += 16;
        }
        if (i + 8 <= tlen) {
            const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(tp + i));
            const __m128i eq = _mm_cmpeq_epi16(c, bcast);
            const unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(eq)) & 0x5555u;
            if (mask != 0) {
                unsigned long bit_idx;
                _BitScanForward(&bit_idx, mask);
                return i + (bit_idx / 2);
            }
            i += 8;
        }
        for (; i <= last; ++i) {
            if (tp[i] == first) {
                return i;
            }
        }
        return npos;
    }

    // SIMD は tlen-7 まで読む。i > last の合致は後で弾くので tlen 基準で十分。
    while (i + 8 <= tlen) {
        const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(tp + i));
        const __m128i eq = _mm_cmpeq_epi16(c, bcast);
        // _mm_movemask_epi8 は各バイトの MSB を取る。16bit 一致なら隣接 2bit が両方 1 なので
        // 0x5555 でフィルタして wchar_t 1 個あたり 1 ビットに圧縮する。
        unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(eq)) & 0x5555u;
        while (mask != 0) {
            unsigned long bit_idx;
            _BitScanForward(&bit_idx, mask);
            mask &= mask - 1;
            const size_t pos = i + (bit_idx / 2);
            if (pos > last) {
                return npos;
            }
            if (std::wmemcmp(tp + pos + 1, qp + 1, qlen - 1) == 0) {
                return pos;
            }
        }
        i += 8;
    }
    // 末尾はスカラで処理 (i + 8 > tlen の領域)
    for (; i <= last; ++i) {
        if (tp[i] == first && std::wmemcmp(tp + i + 1, qp + 1, qlen - 1) == 0) {
            return i;
        }
    }
    return npos;
}

// `Find(text, query) != npos` を読みやすく書くための薄いラッパ。
inline bool Contains(std::wstring_view text, std::wstring_view query) noexcept
{
    return Find(text, query) != npos;
}

// char 版 Find (UTF-8 / 16-byte 並列)。query 先頭バイトを broadcast 比較し、合致候補を memcmp で確認。
// UTF-8 multi-byte シーケンスの中間バイトがクエリ先頭バイトと衝突する可能性はあるが、
// 後続の memcmp で正確に弾けるため正しさは保たれる (UTF-8 self-synchronizing 性は使わずに済む)。
inline size_t Find(std::string_view text, std::string_view query, size_t start = 0) noexcept
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
    const __m128i bcast = _mm_set1_epi8(first);
    const size_t last = tlen - qlen;

    size_t i = start;

    if (qlen == 1) {
        while (i + 16 <= tlen) {
            const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(tp + i));
            const __m128i eq = _mm_cmpeq_epi8(c, bcast);
            const unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(eq));
            if (mask != 0) {
                unsigned long bit_idx;
                _BitScanForward(&bit_idx, mask);
                return i + bit_idx;
            }
            i += 16;
        }
        for (; i <= last; ++i) {
            if (tp[i] == first) {
                return i;
            }
        }
        return npos;
    }

    while (i + 16 <= tlen) {
        const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(tp + i));
        const __m128i eq = _mm_cmpeq_epi8(c, bcast);
        unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(eq));
        while (mask != 0) {
            unsigned long bit_idx;
            _BitScanForward(&bit_idx, mask);
            mask &= mask - 1;
            const size_t pos = i + bit_idx;
            if (pos > last) {
                return npos;
            }
            if (std::memcmp(tp + pos + 1, qp + 1, qlen - 1) == 0) {
                return pos;
            }
        }
        i += 16;
    }
    for (; i <= last; ++i) {
        if (tp[i] == first && std::memcmp(tp + i + 1, qp + 1, qlen - 1) == 0) {
            return i;
        }
    }
    return npos;
}

inline bool Contains(std::string_view text, std::string_view query) noexcept
{
    return Find(text, query) != npos;
}

// consteval 契約違反を CTE (compile-time error) として表面化させるためのタグ。
// 関数本体で throw を実行することで、constant evaluation 中に呼ばれると
// 「constant expression で例外を投げられない」CTE になる仕組み。runtime からは
// 呼ばれない (consteval 文脈以外では消える) 想定で、msg はエラー診断に出る。
namespace ascii_util_detail {
[[noreturn]] inline void consteval_fail(const char* msg)
{
    throw msg;
}
} // namespace ascii_util_detail

// 小文字 ASCII リテラル専用の引数型 (wchar_t 用)。コンパイル時に契約違反を検出する。
// 検証する契約:
//  - NUL 終端 (literal[N-1] == L'\0')。
//  - 全文字が ASCII 範囲 (<= 0x7F)。
//  - 大文字 'A'-'Z' を含まない (RHS は小文字確定でなければならない)。
struct LowercaseAsciiLiteral {
    std::wstring_view value;

    template <size_t N>
    consteval LowercaseAsciiLiteral(const wchar_t (&literal)[N]) noexcept
        : value(literal, N - 1)
    {
        if (literal[N - 1] != L'\0') {
            ascii_util_detail::consteval_fail("LowercaseAsciiLiteral: literal must be NUL-terminated");
        }
        for (size_t i = 0; i < N - 1; ++i) {
            if (literal[i] > 0x7F) {
                ascii_util_detail::consteval_fail("LowercaseAsciiLiteral: literal must contain only ASCII characters");
            }
            if (literal[i] >= L'A' && literal[i] <= L'Z') {
                ascii_util_detail::consteval_fail("LowercaseAsciiLiteral: literal must be lowercase ASCII");
            }
        }
    }
};

// 小文字 ASCII リテラル専用の引数型 (char 用、UTF-8 ビルド向け)。LowercaseAsciiLiteral の char 版。
struct LowercaseAsciiLiteralChar {
    std::string_view value;

    template <size_t N>
    consteval LowercaseAsciiLiteralChar(const char (&literal)[N]) noexcept
        : value(literal, N - 1)
    {
        if (literal[N - 1] != '\0') {
            ascii_util_detail::consteval_fail("LowercaseAsciiLiteralChar: literal must be NUL-terminated");
        }
        for (size_t i = 0; i < N - 1; ++i) {
            // char (signed) で 0x80+ は負値になるので unsigned 変換で判定。
            if (static_cast<unsigned char>(literal[i]) > 0x7F) {
                ascii_util_detail::consteval_fail("LowercaseAsciiLiteralChar: literal must contain only ASCII characters");
            }
            if (literal[i] >= 'A' && literal[i] <= 'Z') {
                ascii_util_detail::consteval_fail("LowercaseAsciiLiteralChar: literal must be lowercase ASCII");
            }
        }
    }
};

// Document テキスト (UTF-8) 用エイリアス。
using DocLowercaseLiteral = LowercaseAsciiLiteralChar;

// 大小無視の等価比較。ASCII 'A'-'Z' のみ小文字化、他は素通し。locale 非依存。
// LHS のみ projection で小文字化する高速版。
constexpr bool iequal(std::wstring_view a, LowercaseAsciiLiteral b) noexcept
{
    return std::ranges::equal(a, b.value, {}, ToLowerAscii);
}

constexpr bool iequal(std::string_view a, LowercaseAsciiLiteralChar b) noexcept
{
    return std::ranges::equal(a, b.value, {}, ToLowerAscii);
}

// 大小無視のプレフィックスマッチ。s が prefix で始まれば true。
constexpr bool istarts_with(std::wstring_view s, LowercaseAsciiLiteral prefix) noexcept
{
    return s.size() >= prefix.value.size() &&
           std::ranges::equal(s.substr(0, prefix.value.size()), prefix.value, {}, ToLowerAscii);
}

constexpr bool istarts_with(std::string_view s, LowercaseAsciiLiteralChar prefix) noexcept
{
    return s.size() >= prefix.value.size() &&
           std::ranges::equal(s.substr(0, prefix.value.size()), prefix.value, {}, ToLowerAscii);
}

template <typename T, std::unsigned_integral U>
constexpr const T* from_chars(const T* start, std::size_t len, U& value, U base = 10) noexcept
{
    const auto end = start + len;
    U b;

    value = 0;
    if (base <= 10) {
        for (; (start < end) && ((b = *start - static_cast<T>('0')) < base); ++start) {
            value = (value * base) + b;
        }
    }
    else {
        for (; start < end; ++start) {
            const U c = static_cast<std::make_unsigned_t<T>>(*start);
            b = c - static_cast<T>('0');
            if (b >= 10) {
                b = (c | 0x20) - static_cast<T>('a');
                if (b >= (base - 10)) {
                    break;
                }
                b += 10;
            }
            value = (value * base) + b;
        }
    }
    return start;
}

} // namespace ascii_util
