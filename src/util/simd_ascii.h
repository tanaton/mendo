#pragma once
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <emmintrin.h>
#include <intrin.h>
#include <string_view>

// SSE2 ベースの ASCII 文字列ヘルパ。wchar_t (UTF-16 code unit) を 8 文字並列で扱う。
// 非 ASCII (>= U+0080) を含む場合は std::towlower や逐次比較にフォールバックするので、
// サロゲートペアやマルチバイト境界を破壊することはない。
namespace simd_ascii {

inline constexpr size_t npos = static_cast<size_t>(-1);

namespace detail {

// 8 wchar_t 入りベクタに非 ASCII (>= 0x80) が含まれているか
inline bool HasNonAscii(__m128i c) noexcept
{
    const __m128i high = _mm_and_si128(c, _mm_set1_epi16(static_cast<short>(0xFF80)));
    const __m128i is_ascii = _mm_cmpeq_epi16(high, _mm_setzero_si128());
    return _mm_movemask_epi8(is_ascii) != 0xFFFF;
}

// ASCII 大文字 'A'-'Z' に対して 0x20 を、それ以外には 0 を返す加算ベクタ。
// _mm_cmpgt_epi16 は符号付き比較だが、ASCII 範囲では符号ビットが立たないので問題なし。
// 0x8000 以上 (CJK 等) は負として扱われ、必ず mask = 0 になるため安全側に倒れる。
inline __m128i AsciiUpperToLowerAdd(__m128i c) noexcept
{
    const __m128i a_minus_1 = _mm_set1_epi16(L'A' - 1);
    const __m128i z_plus_1 = _mm_set1_epi16(L'Z' + 1);
    const __m128i diff = _mm_set1_epi16(0x20);
    const __m128i ge_a = _mm_cmpgt_epi16(c, a_minus_1);
    const __m128i le_z = _mm_cmpgt_epi16(z_plus_1, c);
    const __m128i mask = _mm_and_si128(ge_a, le_z);
    return _mm_and_si128(mask, diff);
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
            const __m128i add = detail::AsciiUpperToLowerAdd(c);
            const __m128i r = _mm_add_epi16(c, add);
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
inline void AsciiToLowerOnly(const wchar_t* src, wchar_t* dst, size_t n) noexcept
{
    size_t i = 0;
    while (i + 8 <= n) {
        const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        const __m128i add = detail::AsciiUpperToLowerAdd(c);
        const __m128i r = _mm_add_epi16(c, add);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), r);
        i += 8;
    }
    for (; i < n; ++i) {
        const wchar_t ch = src[i];
        dst[i] = (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
    }
}

// ASCII 大文字 'A'-'Z' を一文字でも含むなら true。
inline bool HasAsciiUpper(const wchar_t* s, size_t n) noexcept
{
    const __m128i a_minus_1 = _mm_set1_epi16(L'A' - 1);
    const __m128i z_plus_1 = _mm_set1_epi16(L'Z' + 1);
    size_t i = 0;
    while (i + 8 <= n) {
        const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i));
        const __m128i ge_a = _mm_cmpgt_epi16(c, a_minus_1);
        const __m128i le_z = _mm_cmpgt_epi16(z_plus_1, c);
        const __m128i mask = _mm_and_si128(ge_a, le_z);
        if (_mm_movemask_epi8(mask) != 0) {
            return true;
        }
        i += 8;
    }
    for (; i < n; ++i) {
        const wchar_t ch = s[i];
        if (ch >= L'A' && ch <= L'Z') {
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
            if (qlen == 1 || std::wmemcmp(tp + pos + 1, qp + 1, qlen - 1) == 0) {
                return pos;
            }
        }
        i += 8;
    }
    // 末尾はスカラで処理 (i + 8 > tlen の領域)
    if (qlen == 1) {
        for (; i <= last; ++i) {
            if (tp[i] == first) {
                return i;
            }
        }
    }
    else {
        for (; i <= last; ++i) {
            if (tp[i] == first && std::wmemcmp(tp + i + 1, qp + 1, qlen - 1) == 0) {
                return i;
            }
        }
    }
    return npos;
}

} // namespace simd_ascii
