#include "lru_cache.h"
#include <gtest/gtest.h>
#include <string>

TEST(LruCache, InsertAndFind)
{
    LruCache<int, std::string, 3> cache;
    cache.Insert(1, "one");
    cache.Insert(2, "two");
    cache.Insert(3, "three");

    auto* v = cache.Find(1);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, "one");

    EXPECT_EQ(cache.Size(), 3u);
}

TEST(LruCache, EvictsLeastRecentlyInserted)
{
    LruCache<int, std::string, 3> cache;
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

TEST(LruCache, FindPromotesAwayFromTail)
{
    LruCache<int, std::string, 3> cache;
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

TEST(LruCache, InsertExistingKeyUpdatesValueAndPromotes)
{
    LruCache<int, std::string, 3> cache;
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

TEST(LruCache, Clear)
{
    LruCache<int, std::string, 3> cache;
    cache.Insert(1, "one");
    cache.Insert(2, "two");

    cache.Clear();
    EXPECT_EQ(cache.Size(), 0u);
    EXPECT_TRUE(cache.Empty());
}

TEST(LruCache, Contains)
{
    LruCache<int, std::string, 3> cache;
    cache.Insert(1, "one");

    EXPECT_TRUE(cache.Contains(1));
    EXPECT_FALSE(cache.Contains(2));
}

TEST(LruCache, ConstFind)
{
    LruCache<int, std::string, 3> cache;
    cache.Insert(1, "one");
    cache.Insert(2, "two");

    const auto& cref = cache;
    const auto* v = cref.Find(2);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, "two");
}

TEST(LruCache, ConstFindDoesNotChangeOrder)
{
    LruCache<int, int, 3> cache;
    cache.Insert(1, 10);
    cache.Insert(2, 20);
    cache.Insert(3, 30);
    // 内部 [3, 2, 1]

    // const Find は順序を変更しない → [3, 2, 1] のまま
    const auto& cref = cache;
    (void)cref.Find(1);

    // Insert(4) で末尾 (1) が捨てられる
    cache.Insert(4, 40);
    EXPECT_EQ(cache.Find(1), nullptr);
    EXPECT_NE(cache.Find(4), nullptr);
}

TEST(LruCache, SizeOne)
{
    LruCache<int, int, 1> cache;
    cache.Insert(1, 100);
    EXPECT_EQ(cache.Size(), 1u);

    cache.Insert(2, 200);
    EXPECT_EQ(cache.Size(), 1u);
    EXPECT_EQ(cache.Find(1), nullptr);
    auto* v = cache.Find(2);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, 200);
}

TEST(LruCache, MaxSize)
{
    LruCache<int, int, 5> cache;
    EXPECT_EQ(cache.MaxSize(), 5u);
}

TEST(LruCache, EmptyAfterConstruction)
{
    LruCache<int, int, 10> cache;
    EXPECT_TRUE(cache.Empty());
    EXPECT_EQ(cache.Size(), 0u);
}

TEST(LruCache, ContainsDoesNotUpdateOrder)
{
    LruCache<int, int, 2> cache;
    cache.Insert(1, 10);
    cache.Insert(2, 20);
    // 内部 [2, 1]

    // Contains は順序を変更しない
    EXPECT_TRUE(cache.Contains(1));

    // Insert(3) で末尾 (1) が捨てられる
    cache.Insert(3, 30);
    EXPECT_EQ(cache.Find(1), nullptr);
    EXPECT_NE(cache.Find(2), nullptr);
    EXPECT_NE(cache.Find(3), nullptr);
}

TEST(LruCache, StressInsertManyKeepsLatest)
{
    LruCache<int, int, 100> cache;
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

TEST(LruCache, ClearAndReuse)
{
    LruCache<int, int, 3> cache;
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
    LruCache<int, int, 5> cache;
    cache.Insert(1, 10);
    EXPECT_EQ(cache.Find(999), nullptr);
}

TEST(LruCache, ModifyValueThroughFind)
{
    LruCache<int, std::string, 3> cache;
    cache.Insert(1, "original");
    auto* v = cache.Find(1);
    ASSERT_NE(v, nullptr);
    *v = "modified";
    auto* v2 = cache.Find(1);
    EXPECT_EQ(*v2, "modified");
}

TEST(LruCache, RepeatedFindClimbsToFront)
{
    LruCache<int, int, 5> cache;
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

TEST(LruCache, RepeatedInsertExistingKeyClimbsToFront)
{
    LruCache<int, int, 5> cache;
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
