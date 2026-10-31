#include "transposition_cache.h"
#include <gtest/gtest.h>
#include <string>

TEST(TranspositionCache, InsertAndFind)
{
    TranspositionCache<int, std::string, 3> cache;
    cache.Insert(1, "one");
    cache.Insert(2, "two");
    cache.Insert(3, "three");

    auto* v = cache.Find(1);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, "one");

    EXPECT_EQ(cache.Size(), 3u);
}

TEST(TranspositionCache, EvictsLeastRecentlyInserted)
{
    TranspositionCache<int, std::string, 3> cache;
    cache.Insert(1, "one");
    cache.Insert(2, "two");
    cache.Insert(3, "three");

    // 新規 Insert は先頭に入る → 内部 [3, 2, 1]
    // Insert(4) で末尾 (= 最も古く Insert された 1) が捨てられる
    cache.Insert(4, "four");
    EXPECT_EQ(cache.Size(), 3u);
    EXPECT_EQ(cache.Find(1), nullptr);
    EXPECT_NE(cache.Find(2), nullptr);
    EXPECT_NE(cache.Find(3), nullptr);
    EXPECT_NE(cache.Find(4), nullptr);
}

TEST(TranspositionCache, FindPromotesAwayFromTail)
{
    TranspositionCache<int, std::string, 3> cache;
    cache.Insert(1, "one");
    cache.Insert(2, "two");
    cache.Insert(3, "three");
    // 内部 [3, 2, 1]

    // 末尾 (1) を Find すると 1 つ前と swap → [3, 1, 2]
    cache.Find(1);

    // Insert(4) で末尾 (2) が捨てられる
    cache.Insert(4, "four");
    EXPECT_NE(cache.Find(1), nullptr);
    EXPECT_EQ(cache.Find(2), nullptr);
    EXPECT_NE(cache.Find(3), nullptr);
    EXPECT_NE(cache.Find(4), nullptr);
}

TEST(TranspositionCache, InsertExistingKeyUpdatesValueAndPromotes)
{
    TranspositionCache<int, std::string, 3> cache;
    cache.Insert(1, "one");
    cache.Insert(2, "two");
    cache.Insert(3, "three");
    // 内部 [3, 2, 1]

    // 末尾の既存キー (1) を再 Insert すると値更新 + 1 つ前と swap → [3, 1, 2]
    cache.Insert(1, "ONE");
    EXPECT_EQ(cache.Size(), 3u);
    auto* v = cache.Find(1);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, "ONE");

    // Insert(4) で末尾 (2) が捨てられる
    cache.Insert(4, "four");
    EXPECT_NE(cache.Find(1), nullptr);
    EXPECT_EQ(cache.Find(2), nullptr);
    EXPECT_NE(cache.Find(3), nullptr);
    EXPECT_NE(cache.Find(4), nullptr);
}

TEST(TranspositionCache, Clear)
{
    TranspositionCache<int, std::string, 3> cache;
    cache.Insert(1, "one");
    cache.Insert(2, "two");

    cache.Clear();
    EXPECT_EQ(cache.Size(), 0u);
}

TEST(TranspositionCache, SizeOne)
{
    TranspositionCache<int, int, 1> cache;
    cache.Insert(1, 100);
    EXPECT_EQ(cache.Size(), 1u);

    cache.Insert(2, 200);
    EXPECT_EQ(cache.Size(), 1u);
    EXPECT_EQ(cache.Find(1), nullptr);
    auto* v = cache.Find(2);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, 200);
}

TEST(TranspositionCache, EmptyAfterConstruction)
{
    TranspositionCache<int, int, 10> cache;
    EXPECT_EQ(cache.Size(), 0u);
}

TEST(TranspositionCache, StressInsertManyKeepsLatest)
{
    TranspositionCache<int, int, 100> cache;
    for (int i = 0; i < 1000; i++) {
        cache.Insert(i, i * 10);
    }
    EXPECT_EQ(cache.Size(), 100u);

    // 先頭挿入 + 末尾捨てなので、最後に Insert した 100 個が残る
    for (int i = 0; i < 900; i++) {
        EXPECT_EQ(cache.Find(i), nullptr) << "key=" << i;
    }
    for (int i = 900; i < 1000; i++) {
        auto* v = cache.Find(i);
        ASSERT_NE(v, nullptr) << "key=" << i;
        EXPECT_EQ(*v, i * 10);
    }
}

TEST(TranspositionCache, ClearAndReuse)
{
    TranspositionCache<int, int, 3> cache;
    cache.Insert(1, 10);
    cache.Insert(2, 20);
    cache.Clear();

    cache.Insert(3, 30);
    EXPECT_EQ(cache.Size(), 1u);
    EXPECT_EQ(cache.Find(1), nullptr);
    EXPECT_EQ(cache.Find(2), nullptr);
    EXPECT_NE(cache.Find(3), nullptr);
}

TEST(TranspositionCache, FindMissingReturnsNull)
{
    TranspositionCache<int, int, 5> cache;
    cache.Insert(1, 10);
    EXPECT_EQ(cache.Find(999), nullptr);
}

TEST(TranspositionCache, ModifyValueThroughFind)
{
    TranspositionCache<int, std::string, 3> cache;
    cache.Insert(1, "original");
    auto* v = cache.Find(1);
    ASSERT_NE(v, nullptr);
    *v = "modified";
    auto* v2 = cache.Find(1);
    EXPECT_EQ(*v2, "modified");
}

TEST(TranspositionCache, RepeatedFindClimbsToFront)
{
    TranspositionCache<int, int, 5> cache;
    cache.Insert(1, 10);
    cache.Insert(2, 20);
    cache.Insert(3, 30);
    cache.Insert(4, 40);
    cache.Insert(5, 50);
    // 内部 [5, 4, 3, 2, 1]

    // 末尾 (1) を 4 回 Find すると先頭まで昇る → [1, 5, 4, 3, 2]
    for (int i = 0; i < 4; i++) {
        ASSERT_NE(cache.Find(1), nullptr);
    }

    // Insert(6) で末尾 (2) が捨てられる
    cache.Insert(6, 60);
    EXPECT_NE(cache.Find(1), nullptr);
    EXPECT_EQ(cache.Find(2), nullptr);
    EXPECT_NE(cache.Find(5), nullptr);
}

TEST(TranspositionCache, RepeatedInsertExistingKeyClimbsToFront)
{
    TranspositionCache<int, int, 5> cache;
    cache.Insert(1, 10);
    cache.Insert(2, 20);
    cache.Insert(3, 30);
    cache.Insert(4, 40);
    cache.Insert(5, 50);
    // 内部 [5, 4, 3, 2, 1]

    // 末尾 (1) を 4 回 Insert すると先頭まで昇る → [1, 5, 4, 3, 2]
    for (int i = 0; i < 4; i++) {
        cache.Insert(1, 100 + i);
    }
    EXPECT_EQ(cache.Size(), 5u);

    // Insert(6) で末尾 (2) が捨てられる
    cache.Insert(6, 60);
    EXPECT_NE(cache.Find(1), nullptr);
    EXPECT_EQ(cache.Find(2), nullptr);
    EXPECT_NE(cache.Find(5), nullptr);
}

TEST(TranspositionCache, TrimToBudgetEvictsFromTail)
{
    TranspositionCache<int, int, 5> cache;
    cache.Insert(1, 10);
    cache.Insert(2, 20);
    cache.Insert(3, 30);
    // 内部 [3, 2, 1]、cost 合計 60

    cache.TrimToBudget(50, [](int v) static { return static_cast<size_t>(v); });
    EXPECT_EQ(cache.Size(), 2u);
    EXPECT_EQ(cache.Find(1), nullptr);
    EXPECT_NE(cache.Find(3), nullptr);

    // 挿入は引き続き可能 (size_ と head_ の整合が保たれる)
    cache.Insert(4, 1);
    EXPECT_NE(cache.Find(4), nullptr);
    EXPECT_EQ(cache.Size(), 3u);
}

TEST(TranspositionCache, TrimToBudgetKeepsMostRecentEvenIfOverBudget)
{
    TranspositionCache<int, int, 3> cache;
    cache.Insert(1, 100);
    cache.Insert(2, 200);

    cache.TrimToBudget(10, [](int v) static { return static_cast<size_t>(v); });
    EXPECT_EQ(cache.Size(), 1u);
    EXPECT_NE(cache.Find(2), nullptr);
}
