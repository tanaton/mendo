#include "transposition_cache.h"
#include <gtest/gtest.h>
#include <string>

namespace {

// [5, 4, 3, 2, 1] の末尾の 1 を promote で 4 回昇らせると先頭に来て、次の Insert で 2 が捨てられる。
template <class Promote>
void ExpectTailClimbsToFront(Promote promote)
{
    TranspositionCache<int, int, 5> cache;
    for (int k = 1; k <= 5; k++) {
        cache.Insert(k, k * 10);
    }
    for (int i = 0; i < 4; i++) {
        promote(cache, i);
    }
    EXPECT_EQ(cache.Size(), 5u);

    cache.Insert(6, 60);
    EXPECT_NE(cache.Find(1), nullptr);
    EXPECT_EQ(cache.Find(2), nullptr);
    EXPECT_NE(cache.Find(5), nullptr);
}

} // namespace

// 作った直後は空。満杯でなくても無いキーは null。Find の値は書き換えられる。
TEST(TranspositionCache, InsertAndFind)
{
    TranspositionCache<int, std::string, 3> cache;
    EXPECT_EQ(cache.Size(), 0u);
    cache.Insert(1, "one");
    EXPECT_EQ(cache.Find(999), nullptr);
    cache.Insert(2, "two");
    cache.Insert(3, "three");
    EXPECT_EQ(cache.Size(), 3u);
    auto* v = cache.Find(1);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, "one");
    *v = "modified";
    EXPECT_EQ(*cache.Find(1), "modified");
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

TEST(TranspositionCache, RepeatedFindClimbsToFront)
{
    ExpectTailClimbsToFront([](auto& cache, int) { EXPECT_NE(cache.Find(1), nullptr); });
}

TEST(TranspositionCache, RepeatedInsertExistingKeyClimbsToFront)
{
    ExpectTailClimbsToFront([](auto& cache, int i) { cache.Insert(1, 100 + i); });
}
