#include <gtest/gtest.h>
#include "utf8_codec.h"

namespace {

using utf8_codec::DecodeAt;
using utf8_codec::DecodePrev;
using utf8_codec::SnapToCpStart;
using utf8_codec::kReplacement;

// ---- UTF-8: 正常系 ----

TEST(Utf8Codec, DecodeAsciiByte)
{
    const auto r = DecodeAt(std::string_view{ "A" }, 0);
    EXPECT_EQ(r.cp, 0x41u);
    EXPECT_EQ(r.len, 1u);
}

TEST(Utf8Codec, DecodeAsciiNul)
{
    // U+0000 は 1 byte で正規。overlong (C0 80) との対比。
    const auto r = DecodeAt(std::string_view{ "\x00", 1 }, 0);
    EXPECT_EQ(r.cp, 0u);
    EXPECT_EQ(r.len, 1u);
}

TEST(Utf8Codec, DecodeTwoByteSequence)
{
    // U+00E9 (é) = C3 A9
    const auto r = DecodeAt(std::string_view{ "\xC3\xA9" }, 0);
    EXPECT_EQ(r.cp, 0xE9u);
    EXPECT_EQ(r.len, 2u);
}

TEST(Utf8Codec, DecodeThreeByteSequence)
{
    // U+3042 (あ) = E3 81 82
    const auto r = DecodeAt(std::string_view{ "\xE3\x81\x82" }, 0);
    EXPECT_EQ(r.cp, 0x3042u);
    EXPECT_EQ(r.len, 3u);
}

TEST(Utf8Codec, DecodeFourByteSequence)
{
    // U+1F600 (😀) = F0 9F 98 80
    const auto r = DecodeAt(std::string_view{ "\xF0\x9F\x98\x80" }, 0);
    EXPECT_EQ(r.cp, 0x1F600u);
    EXPECT_EQ(r.len, 4u);
}

// ---- UTF-8: 不正系。すべて { kReplacement, 1 } ----

TEST(Utf8Codec, ContinuationByteAsLeading)
{
    // 0x80: 単独の continuation byte は不正 leading
    const auto r = DecodeAt(std::string_view{ "\x80" }, 0);
    EXPECT_EQ(r.cp, kReplacement);
    EXPECT_EQ(r.len, 1u);
}

TEST(Utf8Codec, TruncatedTwoByte)
{
    // C3 (続きのバイトなし)
    const auto r = DecodeAt(std::string_view{ "\xC3" }, 0);
    EXPECT_EQ(r.cp, kReplacement);
    EXPECT_EQ(r.len, 1u);
}

TEST(Utf8Codec, InvalidContinuationByte)
{
    // C3 41: 2 byte目が継続バイトでない
    const auto r = DecodeAt(std::string_view{ "\xC3\x41" }, 0);
    EXPECT_EQ(r.cp, kReplacement);
    EXPECT_EQ(r.len, 1u);
}

TEST(Utf8Codec, OverlongTwoByte)
{
    // C0 80: U+0000 の overlong エンコード (本来は 1 byte)
    const auto r = DecodeAt(std::string_view{ "\xC0\x80" }, 0);
    EXPECT_EQ(r.cp, kReplacement);
    EXPECT_EQ(r.len, 1u);
}

TEST(Utf8Codec, OverlongThreeByte)
{
    // E0 80 80: U+0000 の overlong エンコード (本来は 1 byte)
    const auto r = DecodeAt(std::string_view{ "\xE0\x80\x80" }, 0);
    EXPECT_EQ(r.cp, kReplacement);
    EXPECT_EQ(r.len, 1u);
}

TEST(Utf8Codec, OverlongFourByteAsAscii)
{
    // F0 80 80 A0: U+0020 の overlong エンコード (本来は 1 byte)
    const auto r = DecodeAt(std::string_view{ "\xF0\x80\x80\xA0" }, 0);
    EXPECT_EQ(r.cp, kReplacement);
    EXPECT_EQ(r.len, 1u);
}

TEST(Utf8Codec, SurrogateInUtf8)
{
    // ED A0 80: U+D800 (high surrogate) の UTF-8 表現は無効
    const auto r = DecodeAt(std::string_view{ "\xED\xA0\x80" }, 0);
    EXPECT_EQ(r.cp, kReplacement);
    EXPECT_EQ(r.len, 1u);
}

TEST(Utf8Codec, CodePointAboveUnicodeRange)
{
    // F4 90 80 80: U+110000 (Unicode 範囲外、最大は U+10FFFF)
    const auto r = DecodeAt(std::string_view{ "\xF4\x90\x80\x80" }, 0);
    EXPECT_EQ(r.cp, kReplacement);
    EXPECT_EQ(r.len, 1u);
}

TEST(Utf8Codec, MaxValidCodePoint)
{
    // F4 8F BF BF: U+10FFFF (最大有効 code point)
    const auto r = DecodeAt(std::string_view{ "\xF4\x8F\xBF\xBF" }, 0);
    EXPECT_EQ(r.cp, 0x10FFFFu);
    EXPECT_EQ(r.len, 4u);
}

// ---- UTF-16: 孤立サロゲート ----

TEST(Utf8Codec, Utf16IsolatedHighSurrogate)
{
    const wchar_t s[] = { 0xD800, L'A', 0 };
    const auto r = DecodeAt(std::wstring_view{ s, 2 }, 0);
    EXPECT_EQ(r.cp, kReplacement);
    EXPECT_EQ(r.len, 1u);
}

TEST(Utf8Codec, Utf16IsolatedLowSurrogate)
{
    const wchar_t s[] = { 0xDC00, L'A', 0 };
    const auto r = DecodeAt(std::wstring_view{ s, 2 }, 0);
    EXPECT_EQ(r.cp, kReplacement);
    EXPECT_EQ(r.len, 1u);
}

TEST(Utf8Codec, Utf16HighSurrogateAtEnd)
{
    // 末尾の high surrogate (low が続かない)
    const wchar_t s[] = { 0xD800, 0 };
    const auto r = DecodeAt(std::wstring_view{ s, 1 }, 0);
    EXPECT_EQ(r.cp, kReplacement);
    EXPECT_EQ(r.len, 1u);
}

TEST(Utf8Codec, Utf16ValidSurrogatePair)
{
    // U+1F600 のサロゲートペア: D83D DE00
    const wchar_t s[] = { 0xD83D, 0xDE00, 0 };
    const auto r = DecodeAt(std::wstring_view{ s, 2 }, 0);
    EXPECT_EQ(r.cp, 0x1F600u);
    EXPECT_EQ(r.len, 2u);
}

// ---- SnapToCpStart ----

TEST(Utf8Codec, SnapUtf8FromContinuation)
{
    // U+3042 (あ) = E3 81 82。pos=1 (continuation) → 0
    EXPECT_EQ(SnapToCpStart(std::string_view{ "\xE3\x81\x82" }, 1), 0u);
    EXPECT_EQ(SnapToCpStart(std::string_view{ "\xE3\x81\x82" }, 2), 0u);
    EXPECT_EQ(SnapToCpStart(std::string_view{ "\xE3\x81\x82" }, 0), 0u);
}

TEST(Utf8Codec, SnapUtf16FromLowSurrogate)
{
    const wchar_t s[] = { 0xD83D, 0xDE00, 0 };
    EXPECT_EQ(SnapToCpStart(std::wstring_view{ s, 2 }, 1), 0u);
    EXPECT_EQ(SnapToCpStart(std::wstring_view{ s, 2 }, 0), 0u);
}

// ---- DecodePrev ----

TEST(Utf8Codec, DecodePrevWalksBack)
{
    // "AあB" = 41 E3 81 82 42。pos=4 (B の位置) → prev は あ (E3 81 82)
    const std::string_view s{ "\x41\xE3\x81\x82\x42" };
    const auto r = DecodePrev(s, 4);
    EXPECT_EQ(r.cp, 0x3042u);
    EXPECT_EQ(r.len, 3u);
}

} // namespace
