#include <gtest/gtest.h>
#include "fnv1a.h"
#include <string>
#include <string_view>
#include <unordered_set>

using mendo::Fnv1a64;
using mendo::Fnv1a64Update;
using mendo::kFnv1a64OffsetBasis;
using mendo::kFnv1a64Prime;

TEST(Fnv1a64, EmptyStringReturnsOffsetBasis)
{
    EXPECT_EQ(Fnv1a64(std::string_view{}), kFnv1a64OffsetBasis);
    EXPECT_EQ(Fnv1a64(std::wstring_view{}), kFnv1a64OffsetBasis);
}

// FNV-1a 64-bit リファレンスベクター (RFC 草案 / isthe.com の参照値)。
TEST(Fnv1a64, ReferenceVector_a)
{
    // 'a' (0x61) → offset_basis XOR 0x61 → * prime
    constexpr uint64_t expected = (kFnv1a64OffsetBasis ^ 0x61ULL) * kFnv1a64Prime;
    EXPECT_EQ(Fnv1a64(std::string_view{ "a" }), expected);
    EXPECT_EQ(Fnv1a64(std::string_view{ "a" }), 0xaf63dc4c8601ec8cULL);
}

TEST(Fnv1a64, ReferenceVector_foobar)
{
    EXPECT_EQ(Fnv1a64(std::string_view{ "foobar" }), 0x85944171f73967e8ULL);
}

TEST(Fnv1a64, IncrementalUpdateMatchesBatch)
{
    const std::string_view input = "hello world";
    uint64_t h = kFnv1a64OffsetBasis;
    for (char c : input) {
        h = Fnv1a64Update(h, static_cast<unsigned char>(c));
    }
    EXPECT_EQ(h, Fnv1a64(input));
}

TEST(Fnv1a64, DifferentLengthsDifferentHashes)
{
    EXPECT_NE(Fnv1a64(std::string_view{ "abc" }), Fnv1a64(std::string_view{ "abcd" }));
    // string_view{const char*} は null-terminated で長さ 0 になるため、長さを明示する。
    EXPECT_NE(Fnv1a64(std::string_view{ "", 0 }), Fnv1a64(std::string_view{ "\0", 1 }));
}

TEST(Fnv1a64, OneCharDifferenceProducesDifferentHash)
{
    EXPECT_NE(Fnv1a64(std::string_view{ "abcd" }), Fnv1a64(std::string_view{ "abce" }));
}

TEST(Fnv1a64, CharAndWcharDifferentKeySpacesForMultibyte)
{
    // UTF-8 マルチバイト文字 (3 bytes) と UTF-16 1 unit ではハッシュが異なる。
    // ASCII のみで比較すると char/wchar_t のいずれも数値が一致してしまうため、
    // 非 ASCII で「両 CharT を同一 key 空間で混用してはいけない」性質を確認する。
    const auto h_char = Fnv1a64(std::string_view{ "\xE3\x81\x82" }); // UTF-8 'あ'
    const auto h_wchar = Fnv1a64(std::wstring_view{ L"あ" });    // UTF-16 'あ'
    EXPECT_NE(h_char, h_wchar);
}

TEST(Fnv1a64, HighByteHandledAsUnsigned)
{
    // 0x80 が signed sign-extension で混入していないこと。
    const char buf[] = { static_cast<char>(0x80), 0 };
    const std::string_view sv{ buf, 1 };
    constexpr uint64_t expected = (kFnv1a64OffsetBasis ^ 0x80ULL) * kFnv1a64Prime;
    EXPECT_EQ(Fnv1a64(sv), expected);
}

TEST(Fnv1a64, ConstevalContext)
{
    // constexpr 関数として compile-time に解ける
    constexpr auto h = Fnv1a64(std::string_view{ "x" });
    static_assert(h == ((kFnv1a64OffsetBasis ^ 0x78ULL) * kFnv1a64Prime),
                  "Fnv1a64 must be constexpr-evaluable");
    EXPECT_EQ(h, (kFnv1a64OffsetBasis ^ 0x78ULL) * kFnv1a64Prime);
}

TEST(Fnv1a64, NoCollisionsOnSmallSet)
{
    // 1000 個の "key_<i>" 文字列でハッシュ衝突がないことを確認 (実用域での衝突確率は 10^-12 程度)。
    std::unordered_set<uint64_t> seen;
    for (int i = 0; i < 1000; ++i) {
        const std::string s = "key_" + std::to_string(i);
        const auto inserted = seen.insert(Fnv1a64(std::string_view{ s })).second;
        ASSERT_TRUE(inserted) << "collision at i=" << i;
    }
}

TEST(Fnv1a64Update, ZeroByteUpdateStillChangesHash)
{
    // h XOR 0 * prime ≠ h なので、Fnv1a64Update は「empty 文字を append しても何もしない」
    // という性質を持たない (= 入力 0 byte と 0 だけ含む 1 byte 入力は別ハッシュ)。
    EXPECT_NE(Fnv1a64Update(kFnv1a64OffsetBasis, 0), kFnv1a64OffsetBasis);
}
