#include <gtest/gtest.h>
#include "utility.h"
#include <set>
#include <variant>

// ═══════════════════════════════════════════════
// PmrFormat
// ═══════════════════════════════════════════════

TEST(Utility, PmrFormatPlainText)
{
    auto s = PmrFormat(L"hello");
    EXPECT_EQ(s, L"hello");
}

TEST(Utility, PmrFormatIntegerFormatting)
{
    auto s = PmrFormat(L"count={}", 42);
    EXPECT_EQ(s, L"count=42");
}

TEST(Utility, PmrFormatMultipleArgs)
{
    auto s = PmrFormat(L"{}+{}={}", 1, 2, 3);
    EXPECT_EQ(s, L"1+2=3");
}

TEST(Utility, PmrFormatWideStringArgument)
{
    std::wstring name = L"mendo";
    auto s = PmrFormat(L"hi {}!", name);
    EXPECT_EQ(s, L"hi mendo!");
}

TEST(Utility, PmrFormatReturnsPmrString)
{
    auto s = PmrFormat(L"x={}", 5);
    static_assert(std::is_same_v<decltype(s), std::wstring>,
        "PmrFormat must return std::wstring");
    EXPECT_EQ(s.size(), 3u); // "x=5"
}

TEST(Utility, PmrFormatEmptyFormatString)
{
    auto s = PmrFormat(L"");
    EXPECT_TRUE(s.empty());
}

TEST(Utility, PmrFormatFloatingPoint)
{
    auto s = PmrFormat(L"pi={:.2f}", 3.14159);
    EXPECT_EQ(s, L"pi=3.14");
}

// ═══════════════════════════════════════════════
// overloaded (std::visit 用ヘルパー)
// ═══════════════════════════════════════════════

TEST(Utility, OverloadedDispatchesByType)
{
    std::variant<int, double, std::wstring> v;

    v = 42;
    int tag = std::visit(overloaded{
        [](int) static                 { return 1; },
        [](double) static              { return 2; },
        [](const std::wstring&) static { return 3; },
    }, v);
    EXPECT_EQ(tag, 1);

    v = 1.5;
    tag = std::visit(overloaded{
        [](int) static                 { return 1; },
        [](double) static              { return 2; },
        [](const std::wstring&) static { return 3; },
    }, v);
    EXPECT_EQ(tag, 2);

    v = std::wstring(L"hi");
    tag = std::visit(overloaded{
        [](int) static                 { return 1; },
        [](double) static              { return 2; },
        [](const std::wstring&) static { return 3; },
    }, v);
    EXPECT_EQ(tag, 3);
}

// ═══════════════════════════════════════════════
// Fnv1a64
// ═══════════════════════════════════════════════

TEST(Fnv1a64, EmptyStringReturnsOffsetBasis)
{
    EXPECT_EQ(Fnv1a64(std::string_view{}), kFnv1a64OffsetBasis);
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
    std::set<uint64_t> seen;
    for (int i = 0; i < 1000; ++i) {
        const std::string s = "key_" + std::to_string(i);
        const auto inserted = seen.insert(Fnv1a64(std::string_view{ s })).second;
        ASSERT_TRUE(inserted) << "collision at i=" << i;
    }
}
