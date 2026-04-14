#include <gtest/gtest.h>
#include "flat_map.h"
#include <string>
#include <vector>

static std::vector<int> CollectKeys(const FlatMap<int, int>& m)
{
    std::vector<int> keys;
    for (auto&& [k, v] : m) {
        keys.push_back(k);
    }
    return keys;
}

// ═══════════════════════════════════════════════
// 基本操作
// ═══════════════════════════════════════════════

TEST(FlatMap, EmptyMapIsEmpty)
{
    FlatMap<int, int> m;
    EXPECT_TRUE(m.empty());
    EXPECT_EQ(m.size(), 0u);
}

TEST(FlatMap, TryEmplaceInsertsNew)
{
    FlatMap<int, std::string> m;
    auto [it, inserted] = m.try_emplace(1, "one");
    EXPECT_TRUE(inserted);
    EXPECT_EQ(m.size(), 1u);
    auto&& [k, v] = *it;
    EXPECT_EQ(k, 1);
    EXPECT_EQ(v, "one");
}

TEST(FlatMap, TryEmplaceDuplicateDoesNotOverwrite)
{
    FlatMap<int, std::string> m;
    m.try_emplace(1, "one");
    auto [it, inserted] = m.try_emplace(1, "ONE");
    EXPECT_FALSE(inserted);
    EXPECT_EQ(m.size(), 1u);
    auto&& [k, v] = *it;
    EXPECT_EQ(v, "one");
}

TEST(FlatMap, InsertOrAssignInsertsNew)
{
    FlatMap<int, int> m;
    auto [it, inserted] = m.insert_or_assign(5, 50);
    EXPECT_TRUE(inserted);
    EXPECT_EQ(m.size(), 1u);
    auto&& [k, v] = *it;
    EXPECT_EQ(k, 5);
    EXPECT_EQ(v, 50);
}

TEST(FlatMap, InsertOrAssignOverwritesExisting)
{
    FlatMap<int, int> m;
    m.insert_or_assign(5, 50);
    auto [it, inserted] = m.insert_or_assign(5, 99);
    EXPECT_FALSE(inserted);
    EXPECT_EQ(m.size(), 1u);
    auto&& [k, v] = *it;
    EXPECT_EQ(v, 99);
}

TEST(FlatMap, FindExistingKey)
{
    FlatMap<int, int> m;
    m.try_emplace(3, 30);
    m.try_emplace(1, 10);
    m.try_emplace(5, 50);

    auto it = m.find(3);
    EXPECT_NE(it, m.end());
    auto&& [k, v] = *it;
    EXPECT_EQ(v, 30);
}

TEST(FlatMap, FindMissingKeyReturnsEnd)
{
    FlatMap<int, int> m;
    m.try_emplace(1, 10);
    EXPECT_EQ(m.find(99), m.end());
}

TEST(FlatMap, ContainsExisting)
{
    FlatMap<int, int> m;
    m.try_emplace(42, 0);
    EXPECT_TRUE(m.contains(42));
}

TEST(FlatMap, ContainsMissing)
{
    FlatMap<int, int> m;
    m.try_emplace(42, 0);
    EXPECT_FALSE(m.contains(7));
}

// ═══════════════════════════════════════════════
// ソート順の維持
// ═══════════════════════════════════════════════

TEST(FlatMap, MaintainsSortedOrder)
{
    FlatMap<int, int> m;
    m.try_emplace(5, 50);
    m.try_emplace(1, 10);
    m.try_emplace(3, 30);
    m.try_emplace(7, 70);
    m.try_emplace(2, 20);

    EXPECT_EQ(CollectKeys(m), (std::vector<int>{1, 2, 3, 5, 7}));
}

TEST(FlatMap, InsertOrAssignMaintainsSortedOrder)
{
    FlatMap<int, int> m;
    m.insert_or_assign(10, 100);
    m.insert_or_assign(2, 20);
    m.insert_or_assign(6, 60);

    EXPECT_EQ(CollectKeys(m), (std::vector<int>{2, 6, 10}));
}

// ═══════════════════════════════════════════════
// 削除
// ═══════════════════════════════════════════════

TEST(FlatMap, EraseByKey)
{
    FlatMap<int, int> m;
    m.try_emplace(1, 10);
    m.try_emplace(2, 20);
    m.try_emplace(3, 30);

    m.erase(2);
    EXPECT_EQ(m.size(), 2u);
    EXPECT_FALSE(m.contains(2));
    EXPECT_TRUE(m.contains(1));
    EXPECT_TRUE(m.contains(3));
}

TEST(FlatMap, EraseByKeyMissing)
{
    FlatMap<int, int> m;
    m.try_emplace(1, 10);
    m.erase(99);
    EXPECT_EQ(m.size(), 1u);
}

TEST(FlatMap, EraseByIterator)
{
    FlatMap<int, int> m;
    m.try_emplace(1, 10);
    m.try_emplace(2, 20);
    m.try_emplace(3, 30);

    auto it = m.find(2);
    m.erase(it);
    EXPECT_EQ(m.size(), 2u);
    EXPECT_FALSE(m.contains(2));
}

TEST(FlatMap, EraseEndIteratorIsNoOp)
{
    FlatMap<int, int> m;
    m.try_emplace(1, 10);
    m.erase(m.end());
    EXPECT_EQ(m.size(), 1u);
}

TEST(FlatMap, EraseLastElement)
{
    FlatMap<int, int> m;
    m.try_emplace(1, 10);
    m.erase(1);
    EXPECT_TRUE(m.empty());
}

TEST(FlatMap, EraseFirstElement)
{
    FlatMap<int, int> m;
    m.try_emplace(1, 10);
    m.try_emplace(2, 20);
    m.try_emplace(3, 30);

    m.erase(1);
    EXPECT_EQ(m.size(), 2u);
    EXPECT_FALSE(m.contains(1));
    EXPECT_EQ(CollectKeys(m), (std::vector<int>{2, 3}));
}

// ═══════════════════════════════════════════════
// clear / reserve / shrink_to_fit
// ═══════════════════════════════════════════════

TEST(FlatMap, ClearEmptiesMap)
{
    FlatMap<int, int> m;
    m.try_emplace(1, 10);
    m.try_emplace(2, 20);
    m.clear();
    EXPECT_TRUE(m.empty());
    EXPECT_EQ(m.size(), 0u);
}

TEST(FlatMap, ReserveDoesNotChangeSize)
{
    FlatMap<int, int> m;
    m.reserve(100);
    EXPECT_EQ(m.size(), 0u);
    EXPECT_TRUE(m.empty());
}

TEST(FlatMap, ShrinkToFitDoesNotChangeContents)
{
    FlatMap<int, int> m;
    m.reserve(100);
    m.try_emplace(1, 10);
    m.try_emplace(2, 20);
    m.shrink_to_fit();
    EXPECT_EQ(m.size(), 2u);
    EXPECT_TRUE(m.contains(1));
    EXPECT_TRUE(m.contains(2));
}

// ═══════════════════════════════════════════════
// swap
// ═══════════════════════════════════════════════

TEST(FlatMap, SwapExchangesContents)
{
    FlatMap<int, int> a;
    a.try_emplace(1, 10);
    a.try_emplace(2, 20);

    FlatMap<int, int> b;
    b.try_emplace(3, 30);

    a.swap(b);
    EXPECT_EQ(a.size(), 1u);
    EXPECT_TRUE(a.contains(3));
    EXPECT_EQ(b.size(), 2u);
    EXPECT_TRUE(b.contains(1));
    EXPECT_TRUE(b.contains(2));
}

// ═══════════════════════════════════════════════
// 線形探索 → 二分探索の閾値
// ═══════════════════════════════════════════════

TEST(FlatMap, LinearSearchBelowThreshold)
{
    FlatMap<int, int> m;
    for (int i = 15; i >= 0; i--) {
        m.try_emplace(i, i * 10);
    }
    EXPECT_EQ(m.size(), 16u);

    for (int i = 0; i < 16; i++) {
        auto it = m.find(i);
        ASSERT_NE(it, m.end());
        auto&& [k, v] = *it;
        EXPECT_EQ(v, i * 10);
    }
}

TEST(FlatMap, BinarySearchAboveThreshold)
{
    FlatMap<int, int> m;
    for (int i = 30; i >= 0; i--) {
        m.try_emplace(i, i * 10);
    }
    EXPECT_EQ(m.size(), 31u);

    // 全要素を検索
    for (int i = 0; i <= 30; i++) {
        auto it = m.find(i);
        ASSERT_NE(it, m.end());
        auto&& [k, v] = *it;
        EXPECT_EQ(v, i * 10);
    }
    // 存在しないキー
    EXPECT_EQ(m.find(31), m.end());
    EXPECT_EQ(m.find(-1), m.end());
}

TEST(FlatMap, AtExactThreshold)
{
    // 閾値丁度（16要素）の境界チェック
    constexpr auto threshold = FlatMap<int, int>::linear_search_threshold;
    FlatMap<int, int> m;
    for (int i = 0; i < static_cast<int>(threshold); i++) {
        m.try_emplace(i * 2, i); // 偶数キーのみ
    }
    EXPECT_EQ(m.size(), threshold);

    // 存在するキー（偶数）
    for (int i = 0; i < static_cast<int>(threshold); i++) {
        EXPECT_TRUE(m.contains(i * 2));
    }
    // 存在しないキー（奇数）
    for (int i = 0; i < static_cast<int>(threshold); i++) {
        EXPECT_FALSE(m.contains(i * 2 + 1));
    }
}

// ═══════════════════════════════════════════════
// 文字列キー・値のテスト
// ═══════════════════════════════════════════════

TEST(FlatMap, StringKeyAndValue)
{
    FlatMap<std::pmr::string, std::pmr::string> m;
    m.try_emplace(std::pmr::string{"banana"}, std::pmr::string{"yellow"});
    m.try_emplace(std::pmr::string{"apple"}, std::pmr::string{"red"});
    m.try_emplace(std::pmr::string{"cherry"}, std::pmr::string{"red"});

    EXPECT_EQ(m.size(), 3u);

    auto it = m.find(std::pmr::string{"banana"});
    ASSERT_NE(it, m.end());
    auto&& [k, v] = *it;
    EXPECT_EQ(v, "yellow");

    // ソート順: apple < banana < cherry
    std::vector<std::string> keys;
    for (auto&& [key, val] : m) {
        keys.emplace_back(key);
    }
    EXPECT_EQ(keys, (std::vector<std::string>{"apple", "banana", "cherry"}));
}

// ═══════════════════════════════════════════════
// イテレーション
// ═══════════════════════════════════════════════

TEST(FlatMap, IterateEmpty)
{
    FlatMap<int, int> m;
    EXPECT_EQ(m.begin(), m.end());
}

TEST(FlatMap, ConstIteration)
{
    FlatMap<int, int> m;
    m.try_emplace(1, 10);
    m.try_emplace(2, 20);

    const auto& cm = m;
    int sum = 0;
    for (auto&& [k, v] : cm) {
        sum += v;
    }
    EXPECT_EQ(sum, 30);
}

// ═══════════════════════════════════════════════
// エッジケース
// ═══════════════════════════════════════════════

TEST(FlatMap, FindOnEmptyMap)
{
    FlatMap<int, int> m;
    EXPECT_EQ(m.find(0), m.end());
}

TEST(FlatMap, ContainsOnEmptyMap)
{
    FlatMap<int, int> m;
    EXPECT_FALSE(m.contains(0));
}

TEST(FlatMap, InsertAfterClear)
{
    FlatMap<int, int> m;
    m.try_emplace(1, 10);
    m.clear();
    m.try_emplace(2, 20);
    EXPECT_EQ(m.size(), 1u);
    EXPECT_TRUE(m.contains(2));
    EXPECT_FALSE(m.contains(1));
}

TEST(FlatMap, EraseAllElementsOneByOne)
{
    FlatMap<int, int> m;
    m.try_emplace(1, 10);
    m.try_emplace(2, 20);
    m.try_emplace(3, 30);

    m.erase(2);
    m.erase(1);
    m.erase(3);
    EXPECT_TRUE(m.empty());
}

TEST(FlatMap, InsertAndEraseInterleaved)
{
    FlatMap<int, int> m;
    m.try_emplace(1, 10);
    m.try_emplace(2, 20);
    m.erase(1);
    m.try_emplace(3, 30);
    m.erase(2);
    m.try_emplace(1, 100);

    EXPECT_EQ(m.size(), 2u);
    EXPECT_TRUE(m.contains(1));
    EXPECT_TRUE(m.contains(3));

    auto it = m.find(1);
    ASSERT_NE(it, m.end());
    auto&& [k, v] = *it;
    EXPECT_EQ(v, 100);
}

TEST(FlatMap, ZeroKey)
{
    FlatMap<int, int> m;
    m.try_emplace(0, 42);
    EXPECT_TRUE(m.contains(0));
    auto it = m.find(0);
    ASSERT_NE(it, m.end());
    auto&& [k, v] = *it;
    EXPECT_EQ(v, 42);
}

TEST(FlatMap, NegativeKeys)
{
    FlatMap<int, int> m;
    m.try_emplace(-5, 50);
    m.try_emplace(-1, 10);
    m.try_emplace(3, 30);

    EXPECT_EQ(CollectKeys(m), (std::vector<int>{-5, -1, 3}));
}
