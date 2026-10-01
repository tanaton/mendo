#pragma once
#include "profiler.h"
#include <cstddef>
#include <cstring>
#include <memory_resource>
#include <string>

namespace newline_util_detail {

// CR は ASCII 1 byte なので UTF-8 multi-byte シーケンスの中間バイトとは絶対に衝突しない
// (UTF-8 continuation byte は 10xxxxxx で 0x80-0xBF)。memchr(_, _, 0) は規格上 nullptr を返す。
inline char* FindCr(char* p, size_t len) noexcept
{
    return static_cast<char*>(std::memchr(p, '\r', len));
}

} // namespace newline_util_detail

// CRLF / 旧式 CR を LF に正規化する (in-place)。
// 改行を LF に揃えておくと、パーサ中の current_text と raw_text_ の memcmp 一致判定が成立し、
// 大半の code block / 複数行 paragraph で view モード化 (owned_text_ 確保ゼロ) が選べる。
// CR まで一気にスキップし、その間は memmove でブロックコピーする (MSVC UCRT で SIMD)。
// LF-only ファイルでは memchr 1 回で素通し。
inline void NormalizeNewlines(std::pmr::string& s)
{
    using newline_util_detail::FindCr;
    MENDO_PROFILE("NormalizeNewlines");
    const size_t n = s.size();
    if (n == 0) {
        return;
    }

    char* const data = s.data();
    char* const end = data + n;

    char* first_cr = FindCr(data, n);
    if (!first_cr) {
        MENDO_STATF("NormalizeNewlines: in={} out={} shrunk=0 (fast LF-only)", n, n);
        return;
    }

    // ループ進入時、src は必ず CR を指す (first_cr または直前反復の FindCr 結果)。
    char* dst = first_cr;
    char* src = first_cr;
    do {
        *dst++ = '\n';
        ++src;
        if (src < end && *src == '\n') {
            ++src; // CRLF を LF 1 つに縮約
        }

        char* next_cr = FindCr(src, static_cast<size_t>(end - src));
        const size_t chunk = next_cr ? static_cast<size_t>(next_cr - src) : static_cast<size_t>(end - src);
        if (chunk > 0) {
            std::memmove(dst, src, chunk * sizeof(char));
            src += chunk;
            dst += chunk;
        }
    } while (src < end);

    s.resize(static_cast<size_t>(dst - data));
    MENDO_STATF("NormalizeNewlines: in={} out={} shrunk={}", n, s.size(), n - s.size());
}
