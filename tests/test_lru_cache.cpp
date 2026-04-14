#include "lru_cache.h"
#include <gtest/gtest.h>
#include <string>

TEST(LruCache, InsertAndFind)
{
    LruCache<int, std::string> cache(3);
    cache.Insert(1, "one");
    cache.Insert(2, "two");
    cache.Insert(3, "three");

    auto* v = cache.Find(1);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, "one");

    EXPECT_EQ(cache.Size(), 3u);
}

TEST(LruCache, EvictsOldest)
{
    LruCache<int, std::string> cache(3);
    cache.Insert(1, "one");
    cache.Insert(2, "two");
    cache.Insert(3, "three");

    // 4番目を追加すると最古の1が削除される
    cache.Insert(4, "four");
    EXPECT_EQ(cache.Size(), 3u);
    EXPECT_EQ(cache.Find(1), nullptr);
    EXPECT_NE(cache.Find(2), nullptr);
    EXPECT_NE(cache.Find(3), nullptr);
    EXPECT_NE(cache.Find(4), nullptr);
}

TEST(LruCache, FindUpdatesOrder)
{
    LruCache<int, std::string> cache(3);
    cache.Insert(1, "one");
    cache.Insert(2, "two");
    cache.Insert(3, "three");

    // 1にアクセスしてアクセス順を更新
    cache.Find(1);

    // 4を追加すると、最古は2（1はFindで更新済み）
    cache.Insert(4, "four");
    EXPECT_NE(cache.Find(1), nullptr);
    EXPECT_EQ(cache.Find(2), nullptr);
    EXPECT_NE(cache.Find(3), nullptr);
    EXPECT_NE(cache.Find(4), nullptr);
}

TEST(LruCache, InsertExistingKey)
{
    LruCache<int, std::string> cache(3);
    cache.Insert(1, "one");
    cache.Insert(2, "two");
    cache.Insert(1, "ONE");

    EXPECT_EQ(cache.Size(), 2u);
    auto* v = cache.Find(1);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, "ONE");
}

TEST(LruCache, Clear)
{
    LruCache<int, std::string> cache(3);
    cache.Insert(1, "one");
    cache.Insert(2, "two");

    cache.Clear();
    EXPECT_EQ(cache.Size(), 0u);
    EXPECT_TRUE(cache.Empty());
}

TEST(LruCache, Contains)
{
    LruCache<int, std::string> cache(3);
    cache.Insert(1, "one");

    EXPECT_TRUE(cache.Contains(1));
    EXPECT_FALSE(cache.Contains(2));
}

TEST(LruCache, ConstFind)
{
    LruCache<int, std::string> cache(3);
    cache.Insert(1, "one");

    const auto& cref = cache;
    const auto* v = cref.Find(1);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, "one");
}

TEST(LruCache, SizeOne)
{
    LruCache<int, int> cache(1);
    cache.Insert(1, 100);
    EXPECT_EQ(cache.Size(), 1u);

    cache.Insert(2, 200);
    EXPECT_EQ(cache.Size(), 1u);
    EXPECT_EQ(cache.Find(1), nullptr);
    auto* v = cache.Find(2);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, 200);
}

TEST(LruCache, SizeZero)
{
    LruCache<int, int> cache(0);
    cache.Insert(1, 100);
    EXPECT_EQ(cache.Size(), 0u);
    EXPECT_EQ(cache.Find(1), nullptr);
    EXPECT_FALSE(cache.Contains(1));
}

TEST(LruCache, MaxSize)
{
    LruCache<int, int> cache(5);
    EXPECT_EQ(cache.MaxSize(), 5u);
}

TEST(LruCache, MaxSizeZero)
{
    LruCache<int, int> cache(0);
    EXPECT_EQ(cache.MaxSize(), 0u);
}

TEST(LruCache, EmptyAfterConstruction)
{
    LruCache<int, int> cache(10);
    EXPECT_TRUE(cache.Empty());
    EXPECT_EQ(cache.Size(), 0u);
}

TEST(LruCache, ContainsDoesNotUpdateOrder)
{
    LruCache<int, int> cache(2);
    cache.Insert(1, 10);
    cache.Insert(2, 20);

    // Contains は世代を更新しない
    EXPECT_TRUE(cache.Contains(1));

    // 3を追加すると最古の1が削除される（Containsでは更新されない）
    cache.Insert(3, 30);
    EXPECT_EQ(cache.Find(1), nullptr);
    EXPECT_NE(cache.Find(2), nullptr);
    EXPECT_NE(cache.Find(3), nullptr);
}

TEST(LruCache, StressInsertMany)
{
    LruCache<int, int> cache(100);
    for (int i = 0; i < 1000; i++) {
        cache.Insert(i, i * 10);
    }
    EXPECT_EQ(cache.Size(), 100u);

    // 最後の100個だけ残る
    for (int i = 0; i < 900; i++) {
        EXPECT_EQ(cache.Find(i), nullptr);
    }
    for (int i = 900; i < 1000; i++) {
        auto* v = cache.Find(i);
        ASSERT_NE(v, nullptr);
        EXPECT_EQ(*v, i * 10);
    }
}

TEST(LruCache, InsertExistingKeyUpdatesOrder)
{
    LruCache<int, int> cache(3);
    cache.Insert(1, 10);
    cache.Insert(2, 20);
    cache.Insert(3, 30);

    // key=1 を再挿入して世代を更新
    cache.Insert(1, 100);
    // key=4 を追加すると最古の key=2 が削除される
    cache.Insert(4, 40);
    EXPECT_NE(cache.Find(1), nullptr);
    EXPECT_EQ(cache.Find(2), nullptr);
    EXPECT_NE(cache.Find(3), nullptr);
    EXPECT_NE(cache.Find(4), nullptr);
}

TEST(LruCache, ClearAndReuse)
{
    LruCache<int, int> cache(3);
    cache.Insert(1, 10);
    cache.Insert(2, 20);
    cache.Clear();

    cache.Insert(3, 30);
    EXPECT_EQ(cache.Size(), 1u);
    EXPECT_FALSE(cache.Contains(1));
    EXPECT_FALSE(cache.Contains(2));
    EXPECT_TRUE(cache.Contains(3));
}

TEST(LruCache, FindMissingReturnsNull)
{
    LruCache<int, int> cache(5);
    cache.Insert(1, 10);
    EXPECT_EQ(cache.Find(999), nullptr);
}

TEST(LruCache, ModifyValueThroughFind)
{
    LruCache<int, std::string> cache(3);
    cache.Insert(1, "original");
    auto* v = cache.Find(1);
    ASSERT_NE(v, nullptr);
    *v = "modified";
    auto* v2 = cache.Find(1);
    EXPECT_EQ(*v2, "modified");
}

TEST(LruCache, EvictionOrderWithMultipleAccesses)
{
    LruCache<int, int> cache(3);
    cache.Insert(1, 10);
    cache.Insert(2, 20);
    cache.Insert(3, 30);

    // 1と2にアクセスして世代を更新
    cache.Find(1);
    cache.Find(2);

    // 4を追加 → 最古の3が削除される
    cache.Insert(4, 40);
    EXPECT_NE(cache.Find(1), nullptr);
    EXPECT_NE(cache.Find(2), nullptr);
    EXPECT_EQ(cache.Find(3), nullptr);
    EXPECT_NE(cache.Find(4), nullptr);
}

