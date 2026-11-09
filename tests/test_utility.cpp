#include <gtest/gtest.h>
#include "utility.h"
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
// RapidHash
// ═══════════════════════════════════════════════

// 図のディスクキャッシュのファイル名と InterProcessFileLock の名前になる値。rapidhash.h を替えてここが落ちたら
// MermaidFileCache::VERSION を上げる。
// 長さは rapidhash の分岐 (0・1〜3・4〜7・8〜16・17〜112・113 以上) を一通り通す。
TEST(RapidHash, KeepsPersistedValues)
{
    const std::string x200(200, 'x');
    struct Case {
        std::string_view input;
        uint64_t expected;
    };
    const Case kCases[] = {
        { {},                  0x0338dc4be2cecdaeULL },
        { "a",                 0x599f47df33a2e1ebULL },
        { { "\0", 1 },         0x4f23c791b16eba02ULL },
        { "foobar",            0xd76f53d9bff5e23cULL },
        { "0123456789abcdef",  0x8b249eb98f29bff6ULL },
        { "0123456789abcdefg", 0x62b040278b0412a6ULL },
        { x200,                0x25adb114ca9ea76aULL },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::string{ c.input }));
        EXPECT_EQ(RapidHash(c.input), c.expected);
    }
}

TEST(RapidHash, WideStringHashesUtf16Bytes)
{
    EXPECT_EQ(RapidHash(L"ab"), RapidHash(std::string_view{ "a\0b\0", 4 }));
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
