#include <gtest/gtest.h>
#include "utility.h"
#include <set>
#include <variant>

// ═══════════════════════════════════════════════
// overloaded (std::visit 用ヘルパー)
// ═══════════════════════════════════════════════

TEST(Utility, OverloadedDispatchesByType)
{
    const auto tag_of = [](const std::variant<int, double, std::wstring>& v) static {
        return std::visit(overloaded{
            [](int) static                 { return 1; },
            [](double) static              { return 2; },
            [](const std::wstring&) static { return 3; },
        }, v);
    };
    EXPECT_EQ(tag_of(42), 1);
    EXPECT_EQ(tag_of(1.5), 2);
    EXPECT_EQ(tag_of(std::wstring(L"hi")), 3);
}

// ═══════════════════════════════════════════════
// Fnv1a64
// ═══════════════════════════════════════════════

// 空は offset basis。1 バイトは (basis ^ byte) * prime で、0x80 以上も符号拡張せずに混ぜる。
// 長さの違い・1 文字の違い・NUL 1 バイトでも値が変わる。参照値は isthe.com の FNV-1a 64-bit。
TEST(Fnv1a64, MatchesReferenceValues)
{
    struct Case {
        std::string_view input;
        uint64_t expected;
    };
    constexpr Case kCases[] = {
        { {}, kFnv1a64OffsetBasis },
        { "a", 0xaf63dc4c8601ec8cULL },
        { "a", (kFnv1a64OffsetBasis ^ 0x61ULL) * kFnv1a64Prime },
        { "foobar", 0x85944171f73967e8ULL },
        { { "\x80", 1 }, (kFnv1a64OffsetBasis ^ 0x80ULL) * kFnv1a64Prime },
        { { "\0", 1 }, (kFnv1a64OffsetBasis ^ 0x00ULL) * kFnv1a64Prime },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::string{ c.input }));
        EXPECT_EQ(Fnv1a64(c.input), c.expected);
    }
    EXPECT_NE(Fnv1a64(std::string_view{ "abc" }), Fnv1a64(std::string_view{ "abcd" }));
    EXPECT_NE(Fnv1a64(std::string_view{ "abcd" }), Fnv1a64(std::string_view{ "abce" }));
    // constexpr 関数としてコンパイル時に解ける。
    static_assert(Fnv1a64(std::string_view{ "x" }) == ((kFnv1a64OffsetBasis ^ 0x78ULL) * kFnv1a64Prime));
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

// ═══════════════════════════════════════════════
// FlatHashMap
// ═══════════════════════════════════════════════

TEST(FlatHashMap, EmptyFindsNothing)
{
    const FlatHashMap<int> map;
    EXPECT_EQ(map.Find(1), nullptr);
    EXPECT_EQ(map.size(), 0u);
}

TEST(FlatHashMap, TryEmplaceKeepsExistingValue)
{
    FlatHashMap<int> map;
    const auto [value, added] = map.TryEmplace(42);
    ASSERT_TRUE(added);
    EXPECT_EQ(*value, 0) << "足した要素は既定値";
    *value = 7;

    const auto [again, added_again] = map.TryEmplace(42);
    EXPECT_FALSE(added_again);
    EXPECT_EQ(*again, 7);
    EXPECT_EQ(map.size(), 1u);
}

// 配列を広げても全部の要素が残る。連番のキーと、下位のビットがそろったキーの両方で確かめる。
TEST(FlatHashMap, KeepsEntriesAcrossGrowth)
{
    FlatHashMap<uint64_t> map;
    constexpr uint64_t kCount = 10000;
    for (uint64_t i = 1; i <= kCount; ++i) {
        *map.TryEmplace(i).first = i * 2;
        *map.TryEmplace(i << 32).first = i * 3;
    }
    EXPECT_EQ(map.size(), kCount * 2);
    for (uint64_t i = 1; i <= kCount; ++i) {
        ASSERT_NE(map.Find(i), nullptr);
        EXPECT_EQ(*map.Find(i), i * 2);
        ASSERT_NE(map.Find(i << 32), nullptr);
        EXPECT_EQ(*map.Find(i << 32), i * 3);
    }
    EXPECT_EQ(map.Find(kCount + 1), nullptr);
}

// 0 も最大値も普通のキーとして扱う。
TEST(FlatHashMap, AnyKeyValueIsUsable)
{
    FlatHashMap<int> map;
    constexpr uint64_t kMax = ~uint64_t{ 0 };
    *map.TryEmplace(0).first = 1;
    *map.TryEmplace(1).first = 2;
    *map.TryEmplace(kMax).first = 3;
    EXPECT_EQ(map.size(), 3u);
    EXPECT_EQ(*map.Find(0), 1);
    EXPECT_EQ(*map.Find(1), 2);
    EXPECT_EQ(*map.Find(kMax), 3);
}

TEST(FlatHashMap, EraseIfKeepsTheRest)
{
    FlatHashMap<int> map;
    for (int i = 1; i <= 100; ++i) {
        *map.TryEmplace(static_cast<uint64_t>(i)).first = i;
    }
    map.EraseIf([](int value) static { return value % 2 == 0; });
    EXPECT_EQ(map.size(), 50u);
    for (int i = 1; i <= 100; ++i) {
        EXPECT_EQ(map.Find(static_cast<uint64_t>(i)) != nullptr, i % 2 != 0) << i;
    }
}

// 消した要素の後ろに詰まっていた要素も、配列の端をまたぐものも引ける。
TEST(FlatHashMap, EraseIfKeepsProbeChains)
{
    FlatHashMap<uint64_t> map;
    constexpr uint64_t kCount = 5000;
    for (uint64_t i = 0; i < kCount; ++i) {
        *map.TryEmplace(i * 0x9E3779B1ULL).first = i;
    }
    for (const uint64_t divisor : { 2u, 3u, 5u }) {
        map.EraseIf([divisor](uint64_t value) { return value % divisor == 0; });
    }
    for (uint64_t i = 0; i < kCount; ++i) {
        const bool kept = i % 2 != 0 && i % 3 != 0 && i % 5 != 0;
        const auto* const value = map.Find(i * 0x9E3779B1ULL);
        ASSERT_EQ(value != nullptr, kept) << i;
        if (kept) {
            EXPECT_EQ(*value, i);
        }
    }
}

// 既存のキーを引くだけでは配列が伸びず、ポインタも動かない。
TEST(FlatHashMap, TryEmplaceOnExistingKeyDoesNotGrow)
{
    FlatHashMap<int> map;
    for (uint64_t key = 0; key < 8; ++key) {
        map.TryEmplace(key);
    }
    const int* const before = map.Find(0);
    for (uint64_t key = 0; key < 8; ++key) {
        EXPECT_FALSE(map.TryEmplace(key).second);
    }
    EXPECT_EQ(map.Find(0), before);
}

// ムーブ元は配列が空なので、引いても範囲外を読まず、使い直せる。
TEST(FlatHashMap, MovedFromMapIsUsable)
{
    FlatHashMap<int> map;
    *map.TryEmplace(1).first = 5;
    FlatHashMap<int> moved = std::move(map);
    EXPECT_EQ(*moved.Find(1), 5);
    EXPECT_EQ(map.Find(1), nullptr);
    *map.TryEmplace(2).first = 7;
    EXPECT_EQ(*map.Find(2), 7);
}

TEST(FlatHashMap, ClearAllowsReuse)
{
    FlatHashMap<int> map;
    *map.TryEmplace(3).first = 1;
    map.Clear();
    EXPECT_EQ(map.size(), 0u);
    EXPECT_EQ(map.Find(3), nullptr);
    const auto [value, added] = map.TryEmplace(3);
    EXPECT_TRUE(added);
    EXPECT_EQ(*value, 0) << "前の値が残らない";
}
