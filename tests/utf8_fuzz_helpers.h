#pragma once
#include <cstdint>
#include <format>
#include <iterator>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "utf8_codec.h"

// 不正 UTF-8 を含むランダム入力のプロパティテスト用ヘルパ。
namespace utf8_fuzz {

inline constexpr uint32_t kFuzzSeeds[] = { 1u, 2u, 3u, 0xC0FFEEu, 20261007u };

// 一様乱数では有効な多バイト列も「継続バイトの連なり」もほぼ出ないため、
// 境界処理が壊れやすい byte (継続・各長さの先頭・overlong/サロゲート/範囲外の先頭) に寄せる。
inline char PickMalformedUtf8Byte(std::mt19937& rng)
{
    static constexpr unsigned char kLeads[] = { 0xC0, 0xC2, 0xDF, 0xE0, 0xE3, 0xED, 0xEF, 0xF0, 0xF4, 0xF5, 0xFF };
    switch (std::uniform_int_distribution<int>(0, 3)(rng)) {
    case 0:
        return static_cast<char>(std::uniform_int_distribution<int>('0', 'z')(rng));
    case 1:
    case 2:
        return static_cast<char>(std::uniform_int_distribution<int>(0x80, 0xBF)(rng));
    default:
        return static_cast<char>(kLeads[std::uniform_int_distribution<size_t>(0, std::size(kLeads) - 1)(rng)]);
    }
}

inline std::string RandomMalformedUtf8(std::mt19937& rng, size_t len)
{
    std::string s(len, '\0');
    for (auto& c : s) {
        c = PickMalformedUtf8Byte(rng);
    }
    return s;
}

// pieces から [min_pieces, max_pieces] 個を選んで連結する。選択肢には pieces の他に不正 byte 1 個が加わる。
inline std::string RandomPiecesWithMalformed(std::mt19937& rng, std::span<const std::string_view> pieces, int min_pieces, int max_pieces)
{
    std::string text;
    const int count = std::uniform_int_distribution<int>(min_pieces, max_pieces)(rng);
    std::uniform_int_distribution<size_t> pick(0, pieces.size());
    for (int i = 0; i < count; ++i) {
        const size_t p = pick(rng);
        if (p == pieces.size()) {
            text.push_back(PickMalformedUtf8Byte(rng));
        }
        else {
            text += pieces[p];
        }
    }
    return text;
}

// 先頭から DecodeAt を繰り返したときの code point 区切り (0 と size を含む昇順)。
// 描画側 (WideViewForDWrite / Utf16OffsetCursor) の UTF-16 写像もこの区切りに従う。
template <typename SV>
std::vector<uint32_t> ForwardDecodeBoundaries(SV text)
{
    std::vector<uint32_t> b;
    uint32_t pos = 0;
    while (pos < text.size()) {
        b.push_back(pos);
        pos += utf8_codec::DecodeAt(text, pos).len;
    }
    b.push_back(static_cast<uint32_t>(text.size()));
    return b;
}

// 失敗時に再現できるよう入力をエスケープ表示する。
inline std::string HexEscape(std::string_view s)
{
    std::string out;
    for (const char ch : s) {
        const auto u = static_cast<unsigned char>(ch);
        if (u >= 0x20 && u < 0x7F && ch != '\\') {
            out.push_back(ch);
        }
        else {
            out += std::format("\\x{:02X}", u);
        }
    }
    return out;
}

} // namespace utf8_fuzz
