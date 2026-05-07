#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

// UTF-8 / UTF-16 の単一 code point decode と境界スナップ。
// 不正バイト・truncated・不正 continuation・overlong・サロゲート・U+10FFFF 超は
// すべて { kReplacement, 1 } を返し、呼び出し側のループが必ず 1 単位以上進むことを保証する。
// すべての関数は pos < text.size() を前提とする (引数チェックは呼び出し側責任)。
namespace utf8_codec {

inline constexpr uint32_t kReplacement = 0xFFFD;

struct DecodedCp {
    uint32_t cp;
    uint32_t len; // 単位は decode 入力の code unit (UTF-8 なら byte 1-4、UTF-16 なら wchar_t 1-2)。
};

// UTF-8: pos がマルチバイト継続バイト (10xxxxxx) を指したら先頭バイトまで戻す。
constexpr uint32_t SnapToCpStart(std::string_view text, uint32_t pos) noexcept
{
    while (pos > 0 && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80) {
        --pos;
    }
    return pos;
}

// UTF-16: pos が low surrogate を指したら直前の high surrogate まで戻す。
constexpr uint32_t SnapToCpStart(std::wstring_view text, uint32_t pos) noexcept
{
    if (pos > 0) {
        const auto c = static_cast<uint16_t>(text[pos]);
        const auto p = static_cast<uint16_t>(text[pos - 1]);
        if (c >= 0xDC00 && c <= 0xDFFF && p >= 0xD800 && p <= 0xDBFF) {
            return pos - 1;
        }
    }
    return pos;
}

constexpr DecodedCp DecodeAt(std::string_view text, uint32_t pos) noexcept
{
    const auto first = static_cast<unsigned char>(text[pos]);
    if (first < 0x80) {
        return { first, 1 };
    }
    uint32_t cp = 0;
    uint32_t len = 0;
    if ((first & 0xE0) == 0xC0) {
        cp = first & 0x1F;
        len = 2;
    }
    else if ((first & 0xF0) == 0xE0) {
        cp = first & 0x0F;
        len = 3;
    }
    else if ((first & 0xF8) == 0xF0) {
        cp = first & 0x07;
        len = 4;
    }
    else {
        return { kReplacement, 1 };
    }
    if (static_cast<size_t>(pos) + len > text.size()) {
        return { kReplacement, 1 };
    }
    for (uint32_t i = 1; i < len; ++i) {
        const auto b = static_cast<unsigned char>(text[pos + i]);
        if ((b & 0xC0) != 0x80) {
            return { kReplacement, 1 };
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    // 非スカラー値の排除:
    //   overlong (より短い符号化が可能な値)、UTF-16 サロゲート領域、Unicode 範囲外。
    //   これらをそのまま返すと UTF-16 化で孤立サロゲートを生むなど後続処理で破綻する。
    constexpr uint32_t min_cp_for_len[] = { 0x80u, 0x800u, 0x10000u }; // len = 2/3/4
    if (cp < min_cp_for_len[len - 2] || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
        return { kReplacement, 1 };
    }
    return { cp, len };
}

constexpr DecodedCp DecodeAt(std::wstring_view text, uint32_t pos) noexcept
{
    const auto c = static_cast<uint16_t>(text[pos]);
    if (c >= 0xD800 && c <= 0xDBFF) {
        if (static_cast<size_t>(pos) + 1 < text.size()) {
            const auto c2 = static_cast<uint16_t>(text[pos + 1]);
            if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
                const uint32_t cp = 0x10000u + ((static_cast<uint32_t>(c) - 0xD800u) << 10) + (static_cast<uint32_t>(c2) - 0xDC00u);
                return { cp, 2 };
            }
        }
        return { kReplacement, 1 }; // 孤立 high surrogate
    }
    if (c >= 0xDC00 && c <= 0xDFFF) {
        return { kReplacement, 1 }; // 孤立 low surrogate
    }
    return { c, 1 };
}

// pos の直前の code point を decode。pos > 0 が前提。
template <typename SV>
constexpr DecodedCp DecodePrev(SV text, uint32_t pos) noexcept
{
    return DecodeAt(text, SnapToCpStart(text, pos - 1));
}

} // namespace utf8_codec
