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

