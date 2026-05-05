#pragma once
#include "profiler.h"
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace mendo::utf {

// UTF-16 wide 入力を UTF-8 化する 1 パスエンコーダ (map なし版、std::pmr::string 出力)。
// OS の WideCharToMultiByte に直接渡し、SIMD 最適化された実装を活用する。
// API 呼び出しは per-parse 1 回なのでオーバーヘッドは無視できる。
// OnText 経路では「UTF-8 byte → UTF-16 code unit」の変換を ParseContext に保持する
// 累積カウンタ (utf8_walked / wide_walked) で amortized O(1) で行うため、N byte の
// uint32_t 配列を保持しなくてよい (167KB 入力で 600KB の節約)。
inline void BuildUtf8FromWide(std::wstring_view wide, std::pmr::string& utf8_out)
{
    MENDO_PROFILE("BuildUtf8FromWide");
    utf8_out.clear();
    if (wide.empty()) {
        return;
    }
    if (wide.size() > static_cast<size_t>(std::numeric_limits<int>::max()) / 3) {
        return;
    }
    // UTF-8 byte 数 ≤ UTF-16 wchar 数 × 3 が常に成立 (BMP 1〜3 byte / surrogate pair 4 byte で
    // wide 2 unit に対応するため最大比率 3:1 を超えない)。上限確保で API 1 回呼び出し。
    utf8_out.resize_and_overwrite(wide.size() * 3, [wide](char* buf, size_t count) -> size_t {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           buf, static_cast<int>(count), nullptr, nullptr);
        return n > 0 ? static_cast<size_t>(n) : 0;
    });
}

// UTF-16 wide 入力を UTF-8 化しつつ、UTF-8 byte index → UTF-16 code unit index
// の単方向 map を構築する 1 パスエンコーダ。
// map のサイズは utf8.size() + 1 で、最後の番兵は wide.size() (UTF-16 終端)。
// 規則:
//   - ASCII (U+0000-007F): 1 byte。同一 u16_idx 1 個 push
//   - U+0080-07FF: 2 byte。同一 u16_idx を 2 個 push (lead + continuation)
//   - U+0800-FFFF (BMP non-surrogate): 3 byte。同一 u16_idx を 3 個 push
//   - surrogate pair (U+D800-DBFF + U+DC00-DFFF): 4 byte。high surrogate の
//     u16_idx を 4 個 push し、低位サロゲートの code unit はスキップ
//   - 不正な孤立サロゲート: 3 byte の生バイトとして扱う
inline void BuildUtf8AndOffsetMap(std::wstring_view wide,
                                  std::pmr::string& utf8_out,
                                  std::pmr::vector<uint32_t>& map_out)
{
    MENDO_PROFILE("BuildUtf8AndOffsetMap");
    utf8_out.clear();
    map_out.clear();
    const size_t wide_size = wide.size();
    if (wide_size == 0) {
        map_out.push_back(0);
        return;
    }
    utf8_out.reserve(wide_size * 3);
    map_out.reserve(wide_size * 3 + 1);

    size_t i = 0;
    while (i < wide_size) {
        // ASCII fast path: 連続 ASCII 範囲を一気に走査して resize + 直接書き込みでまとめる。
        // 英語ドキュメント / コード本体は ASCII 比率が極めて高いため、push_back の per-byte
        // capacity チェックを避けると BuildUtf8AndOffsetMap 全体が O(N) のままで定数項が縮む。
        const size_t ascii_start = i;
        while (i < wide_size && static_cast<uint16_t>(wide[i]) < 0x80) {
            ++i;
        }
        if (i > ascii_start) {
            const size_t ascii_count = i - ascii_start;
            const size_t old_utf8 = utf8_out.size();
            const size_t old_map = map_out.size();
            utf8_out.resize(old_utf8 + ascii_count);
            map_out.resize(old_map + ascii_count);
            char* const u8_dst = utf8_out.data() + old_utf8;
            uint32_t* const map_dst = map_out.data() + old_map;
            const wchar_t* const w_src = wide.data() + ascii_start;
            for (size_t k = 0; k < ascii_count; ++k) {
                u8_dst[k] = static_cast<char>(static_cast<uint16_t>(w_src[k]));
                map_dst[k] = static_cast<uint32_t>(ascii_start + k);
            }
        }
        if (i == wide_size) {
            break;
        }

        const wchar_t wc = wide[i];
        const uint32_t u16_idx = static_cast<uint32_t>(i);
        const uint32_t cu = static_cast<uint16_t>(wc);
        if (cu < 0x800) {
            map_out.push_back(u16_idx);
            map_out.push_back(u16_idx);
            utf8_out.push_back(static_cast<char>(0xC0 | (cu >> 6)));
            utf8_out.push_back(static_cast<char>(0x80 | (cu & 0x3F)));
            ++i;
        }
        else if (cu >= 0xD800 && cu <= 0xDBFF && i + 1 < wide_size
                 && static_cast<uint16_t>(wide[i + 1]) >= 0xDC00
                 && static_cast<uint16_t>(wide[i + 1]) <= 0xDFFF) {
            const uint32_t high = cu - 0xD800;
            const uint32_t low = static_cast<uint16_t>(wide[i + 1]) - 0xDC00;
            const uint32_t cp = 0x10000u + (high << 10) + low;
            map_out.push_back(u16_idx);
            map_out.push_back(u16_idx);
            map_out.push_back(u16_idx);
            map_out.push_back(u16_idx);
            utf8_out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            utf8_out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            utf8_out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            utf8_out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            i += 2;
        }
        else {
            map_out.push_back(u16_idx);
            map_out.push_back(u16_idx);
            map_out.push_back(u16_idx);
            utf8_out.push_back(static_cast<char>(0xE0 | (cu >> 12)));
            utf8_out.push_back(static_cast<char>(0x80 | ((cu >> 6) & 0x3F)));
            utf8_out.push_back(static_cast<char>(0x80 | (cu & 0x3F)));
            ++i;
        }
    }
    map_out.push_back(static_cast<uint32_t>(wide_size));
}

// UTF-8 byte index から UTF-16 code unit index を引く。
// map は BuildUtf8AndOffsetMap で構築されたものを想定。
// utf8_idx は 0..utf8.size() の範囲を取り、map[utf8_idx] が UTF-16 code unit index。
// utf8.size() の番兵は wide.size() (UTF-16 終端)。
[[nodiscard]] constexpr uint32_t U8ToU16(std::span<const uint32_t> map, uint32_t utf8_idx) noexcept
{
    return map[utf8_idx];
}

} // namespace mendo::utf
