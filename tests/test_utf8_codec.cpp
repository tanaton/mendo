#include <gtest/gtest.h>
#include "utf8_codec.h"

namespace {

using utf8_codec::DecodeAt;
using utf8_codec::DecodePrev;
using utf8_codec::EncodeCp;
using utf8_codec::SnapToCpStart;
using utf8_codec::kReplacement;
using namespace std::literals;

// ---- DecodeAt ----

template <class SV>
struct DecodeCase {
    const char* name;
    SV units;
    uint32_t cp;
    uint32_t len;
};

// 不正系はすべて { kReplacement, 1 }
constexpr DecodeCase<std::string_view> kUtf8DecodeCases[] = {
    { "Ascii", "A"sv, 0x41u, 1u },
    // U+0000 は 1 byte で正規。overlong (C0 80) との対比
    { "AsciiNul", "\x00"sv, 0u, 1u },
    { "TwoByte U+00E9", "\xC3\xA9"sv, 0xE9u, 2u },
    { "ThreeByte U+3042", "\xE3\x81\x82"sv, 0x3042u, 3u },
    { "FourByte U+1F600", "\xF0\x9F\x98\x80"sv, 0x1F600u, 4u },
    { "MaxValid U+10FFFF", "\xF4\x8F\xBF\xBF"sv, 0x10FFFFu, 4u },
    { "ContinuationByteAsLeading", "\x80"sv, kReplacement, 1u },
    { "TruncatedTwoByte", "\xC3"sv, kReplacement, 1u },
    { "InvalidContinuationByte", "\xC3\x41"sv, kReplacement, 1u },
    { "OverlongTwoByte U+0000", "\xC0\x80"sv, kReplacement, 1u },
    { "OverlongThreeByte U+0000", "\xE0\x80\x80"sv, kReplacement, 1u },
    { "OverlongFourByte U+0020", "\xF0\x80\x80\xA0"sv, kReplacement, 1u },
    { "Surrogate U+D800", "\xED\xA0\x80"sv, kReplacement, 1u },
    { "AboveUnicodeRange U+110000", "\xF4\x90\x80\x80"sv, kReplacement, 1u },
};

constexpr DecodeCase<std::wstring_view> kUtf16DecodeCases[] = {
    { "IsolatedHighSurrogate", L"\xD800" L"A"sv, kReplacement, 1u },
    { "IsolatedLowSurrogate", L"\xDC00" L"A"sv, kReplacement, 1u },
    { "HighSurrogateAtEnd", L"\xD800"sv, kReplacement, 1u },
    { "ValidSurrogatePair U+1F600", L"\xD83D\xDE00"sv, 0x1F600u, 2u },
};

template <class SV, size_t N>
void ExpectDecodes(const DecodeCase<SV> (&cases)[N])
{
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        const auto r = DecodeAt(c.units, 0);
        EXPECT_EQ(r.cp, c.cp);
        EXPECT_EQ(r.len, c.len);
    }
}

TEST(Utf8Codec, DecodeUtf8)
{
    ExpectDecodes(kUtf8DecodeCases);
}

TEST(Utf8Codec, DecodeUtf16)
{
    ExpectDecodes(kUtf16DecodeCases);
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

// ---- EncodeCp: 正常系 ----

// EncodeDecodeRoundTrip は encoder と decoder の双方に同種の bit-shift バグがあると
// 検出できない。bit pattern を直接検証するスモークとして 1 件だけ残す。
TEST(Utf8Codec, EncodeFourByteBitPattern)
{
    // U+1F600 (😀) = F0 9F 98 80
    char buf[4]{};
    const uint32_t len = EncodeCp(0x1F600u, buf);
    EXPECT_EQ(len, 4u);
    EXPECT_EQ(static_cast<unsigned char>(buf[0]), 0xF0u);
    EXPECT_EQ(static_cast<unsigned char>(buf[1]), 0x9Fu);
    EXPECT_EQ(static_cast<unsigned char>(buf[2]), 0x98u);
    EXPECT_EQ(static_cast<unsigned char>(buf[3]), 0x80u);
}

TEST(Utf8Codec, EncodeBoundaries)
{
    // 各 byte 長の境界値: U+0000 / U+007F / U+0080 / U+07FF / U+0800 / U+FFFF / U+10000 / U+10FFFF
    char buf[4]{};
    EXPECT_EQ(EncodeCp(0x0000u, buf), 1u);
    EXPECT_EQ(EncodeCp(0x007Fu, buf), 1u);
    EXPECT_EQ(EncodeCp(0x0080u, buf), 2u);
    EXPECT_EQ(EncodeCp(0x07FFu, buf), 2u);
    EXPECT_EQ(EncodeCp(0x0800u, buf), 3u);
    EXPECT_EQ(EncodeCp(0xFFFFu, buf), 3u);
    EXPECT_EQ(EncodeCp(0x10000u, buf), 4u);
    EXPECT_EQ(EncodeCp(0x10FFFFu, buf), 4u);
}

// ---- EncodeCp: 不正系 (戻り値 0、buf 不変) ----

// 部分書き込み (途中の byte だけ更新) を将来の refactor で混入させないため、
// sentinel で全 byte 不変を確認する保険。
TEST(Utf8Codec, EncodeRejectsAndPreservesBuf)
{
    constexpr char kSentinel[4] = { '\x5A', '\x5A', '\x5A', '\x5A' };
    for (const uint32_t cp : { 0xD800u, 0xDFFFu, 0x110000u, 0xFFFFFFFFu }) {
        SCOPED_TRACE(cp);
        char buf[4] = { kSentinel[0], kSentinel[1], kSentinel[2], kSentinel[3] };
        EXPECT_EQ(EncodeCp(cp, buf), 0u);
        for (size_t i = 0; i < 4; ++i) {
            EXPECT_EQ(buf[i], kSentinel[i]) << "i=" << i;
        }
    }
}

// ---- EncodeCp <-> DecodeAt 往復 ----

TEST(Utf8Codec, EncodeDecodeRoundTrip)
{
    // 各 byte 長の代表値で encode → decode が元の cp に戻ることを確認。
    constexpr uint32_t samples[] = { 0x0000u, 0x0041u, 0x007Fu, 0x0080u, 0x00A9u, 0x07FFu,
                                     0x0800u, 0x3042u, 0xFFFFu, 0x10000u, 0x1F600u, 0x10FFFFu };
    for (const uint32_t cp : samples) {
        char buf[4]{};
        const uint32_t len = EncodeCp(cp, buf);
        ASSERT_GT(len, 0u) << "cp=" << cp;
        const auto r = DecodeAt(std::string_view{ buf, len }, 0);
        EXPECT_EQ(r.cp, cp) << "cp=" << cp;
        EXPECT_EQ(r.len, len) << "cp=" << cp;
    }
}

} // namespace
