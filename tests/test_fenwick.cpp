#include <gtest/gtest.h>
#include "fenwick.h"

using mendo::FloatFenwick;

TEST(FenwickTest, EmptyAfterDefault)
{
    FloatFenwick fw;
    EXPECT_EQ(fw.size(), 0u);
    EXPECT_TRUE(fw.empty());
    EXPECT_FLOAT_EQ(fw.PrefixSum(0), 0.0f);
}

TEST(FenwickTest, ResizeFillsZero)
{
    FloatFenwick fw;
    fw.Resize(5);
    EXPECT_EQ(fw.size(), 5u);
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_FLOAT_EQ(fw.GetPoint(i), 0.0f);
    }
    EXPECT_FLOAT_EQ(fw.PrefixSum(5), 0.0f);
}

TEST(FenwickTest, SetAndPrefixSum)
{
    FloatFenwick fw;
    fw.Resize(5);
    fw.Set(0, 1.0f);
    fw.Set(1, 2.0f);
    fw.Set(2, 3.0f);
    fw.Set(3, 4.0f);
    fw.Set(4, 5.0f);

    EXPECT_FLOAT_EQ(fw.PrefixSum(0), 0.0f);
    EXPECT_FLOAT_EQ(fw.PrefixSum(1), 1.0f);
    EXPECT_FLOAT_EQ(fw.PrefixSum(2), 3.0f);
    EXPECT_FLOAT_EQ(fw.PrefixSum(3), 6.0f);
    EXPECT_FLOAT_EQ(fw.PrefixSum(4), 10.0f);
    EXPECT_FLOAT_EQ(fw.PrefixSum(5), 15.0f);
}

TEST(FenwickTest, RangeSum)
{
    FloatFenwick fw;
    fw.Resize(5);
    fw.Set(0, 10.0f);
    fw.Set(1, 20.0f);
    fw.Set(2, 30.0f);
    fw.Set(3, 40.0f);
    fw.Set(4, 50.0f);

    EXPECT_FLOAT_EQ(fw.RangeSum(0, 5), 150.0f);
    EXPECT_FLOAT_EQ(fw.RangeSum(1, 4), 90.0f);
    EXPECT_FLOAT_EQ(fw.RangeSum(2, 3), 30.0f);
    EXPECT_FLOAT_EQ(fw.RangeSum(3, 3), 0.0f);
}

TEST(FenwickTest, GetPoint)
{
    FloatFenwick fw;
    fw.Resize(4);
    fw.Set(0, 1.5f);
    fw.Set(1, 2.5f);
    fw.Set(2, 3.5f);
    fw.Set(3, 4.5f);

    EXPECT_FLOAT_EQ(fw.GetPoint(0), 1.5f);
    EXPECT_FLOAT_EQ(fw.GetPoint(1), 2.5f);
    EXPECT_FLOAT_EQ(fw.GetPoint(2), 3.5f);
    EXPECT_FLOAT_EQ(fw.GetPoint(3), 4.5f);
}

TEST(FenwickTest, SetOverwrite)
{
    FloatFenwick fw;
    fw.Resize(3);
    fw.Set(0, 5.0f);
    fw.Set(1, 10.0f);
    fw.Set(2, 15.0f);
    EXPECT_FLOAT_EQ(fw.PrefixSum(3), 30.0f);

    // 2 番目を 100 に書き換え
    fw.Set(1, 100.0f);
    EXPECT_FLOAT_EQ(fw.GetPoint(1), 100.0f);
    EXPECT_FLOAT_EQ(fw.PrefixSum(3), 120.0f);
    EXPECT_FLOAT_EQ(fw.PrefixSum(1), 5.0f);
    EXPECT_FLOAT_EQ(fw.PrefixSum(2), 105.0f);
}

TEST(FenwickTest, Add)
{
    FloatFenwick fw;
    fw.Resize(3);
    fw.Add(0, 1.0f);
    fw.Add(0, 2.0f);
    fw.Add(2, 5.0f);

    EXPECT_FLOAT_EQ(fw.GetPoint(0), 3.0f);
    EXPECT_FLOAT_EQ(fw.GetPoint(1), 0.0f);
    EXPECT_FLOAT_EQ(fw.GetPoint(2), 5.0f);
    EXPECT_FLOAT_EQ(fw.PrefixSum(3), 8.0f);
}

TEST(FenwickTest, GrowToAddsZeros)
{
    FloatFenwick fw;
    fw.Resize(3);
    fw.Set(0, 1.0f);
    fw.Set(1, 2.0f);
    fw.Set(2, 3.0f);

    fw.GrowTo(5);
    EXPECT_EQ(fw.size(), 5u);
    EXPECT_FLOAT_EQ(fw.PrefixSum(3), 6.0f);
    EXPECT_FLOAT_EQ(fw.GetPoint(3), 0.0f);
    EXPECT_FLOAT_EQ(fw.GetPoint(4), 0.0f);
    EXPECT_FLOAT_EQ(fw.PrefixSum(5), 6.0f);

    // 末尾追加領域を Set しても prefix が壊れない
    fw.Set(3, 10.0f);
    fw.Set(4, 20.0f);
    EXPECT_FLOAT_EQ(fw.PrefixSum(5), 36.0f);
    EXPECT_FLOAT_EQ(fw.PrefixSum(3), 6.0f);
}

TEST(FenwickTest, GrowToShrinkIsNoop)
{
    FloatFenwick fw;
    fw.Resize(5);
    fw.Set(0, 1.0f);
    fw.Set(4, 5.0f);

    fw.GrowTo(3); // 縮小要求は無視
    EXPECT_EQ(fw.size(), 5u);
    EXPECT_FLOAT_EQ(fw.PrefixSum(5), 6.0f);
}

TEST(FenwickTest, BuildLinearTime)
{
    FloatFenwick fw;
    const float values[] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f };
    fw.Resize(std::size(values));
    fw.Build(std::span<const float>(values, std::size(values)));

    EXPECT_EQ(fw.size(), 7u);
    EXPECT_FLOAT_EQ(fw.PrefixSum(7), 28.0f);
    EXPECT_FLOAT_EQ(fw.PrefixSum(3), 6.0f);
    EXPECT_FLOAT_EQ(fw.GetPoint(0), 1.0f);
    EXPECT_FLOAT_EQ(fw.GetPoint(6), 7.0f);
    EXPECT_FLOAT_EQ(fw.RangeSum(2, 5), 12.0f);

    // Build 後も Set が正常に動作する
    fw.Set(3, 100.0f);
    EXPECT_FLOAT_EQ(fw.GetPoint(3), 100.0f);
    EXPECT_FLOAT_EQ(fw.PrefixSum(7), 124.0f);
}

TEST(FenwickTest, BuildOverwritesExisting)
{
    FloatFenwick fw;
    fw.Resize(3);
    fw.Set(0, 99.0f);
    fw.Set(2, 88.0f);

    const float values[] = { 1.0f, 2.0f, 3.0f };
    fw.Build(std::span<const float>(values, std::size(values)));

    EXPECT_EQ(fw.size(), 3u);
    EXPECT_FLOAT_EQ(fw.GetPoint(0), 1.0f);
    EXPECT_FLOAT_EQ(fw.GetPoint(1), 2.0f);
    EXPECT_FLOAT_EQ(fw.GetPoint(2), 3.0f);
    EXPECT_FLOAT_EQ(fw.PrefixSum(3), 6.0f);
}

TEST(FenwickTest, ResetClearsAll)
{
    FloatFenwick fw;
    fw.Resize(3);
    fw.Set(0, 1.0f);
    fw.Set(1, 2.0f);
    fw.Set(2, 3.0f);

    fw.Reset();
    EXPECT_EQ(fw.size(), 0u);
    EXPECT_TRUE(fw.empty());
}

TEST(FenwickTest, NegativeAndZeroValues)
{
    FloatFenwick fw;
    fw.Resize(4);
    fw.Set(0, -5.0f);
    fw.Set(1, 0.0f);
    fw.Set(2, 5.0f);
    fw.Set(3, -10.0f);

    EXPECT_FLOAT_EQ(fw.PrefixSum(4), -10.0f);
    EXPECT_FLOAT_EQ(fw.RangeSum(0, 2), -5.0f);
    EXPECT_FLOAT_EQ(fw.RangeSum(2, 4), -5.0f);
}

TEST(FenwickTest, FindIndexLowerBoundEmpty)
{
    FloatFenwick fw;
    EXPECT_EQ(fw.FindIndexLowerBound(0.0f), 0u);
    EXPECT_EQ(fw.FindIndexLowerBound(100.0f), 0u);
}

TEST(FenwickTest, FindIndexLowerBoundAllZeros)
{
    FloatFenwick fw;
    fw.Resize(5);
    // 全要素 0 で target=0 は PrefixSum(*) = 0 で常に PrefixSum > 0 が成立しない → size() を返す。
    EXPECT_EQ(fw.FindIndexLowerBound(0.0f), 5u);
    EXPECT_EQ(fw.FindIndexLowerBound(-1.0f), 0u);
}

TEST(FenwickTest, FindIndexLowerBoundMonotonic)
{
    FloatFenwick fw;
    const float values[] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f };
    fw.Resize(std::size(values));
    fw.Build(std::span<const float>(values, std::size(values)));
    // PrefixSum: [0, 1, 3, 6, 10, 15] (index 0 は累積開始位置)
    // target=0  → PrefixSum(1)=1 > 0   → 0
    // target=0.5→ PrefixSum(1)=1 > 0.5 → 0
    // target=1  → PrefixSum(1)=1 <= 1, PrefixSum(2)=3 > 1 → 1
    // target=2.9→ PrefixSum(2)=3 > 2.9 → 1
    // target=3  → PrefixSum(2)=3 <= 3, PrefixSum(3)=6 > 3 → 2
    // target=14 → PrefixSum(4)=10 <= 14, PrefixSum(5)=15 > 14 → 4
    // target=15 → PrefixSum(5)=15 <= 15, → 5 (該当なし)
    // target=100 → 5
    EXPECT_EQ(fw.FindIndexLowerBound(0.0f), 0u);
    EXPECT_EQ(fw.FindIndexLowerBound(0.5f), 0u);
    EXPECT_EQ(fw.FindIndexLowerBound(1.0f), 1u);
    EXPECT_EQ(fw.FindIndexLowerBound(2.9f), 1u);
    EXPECT_EQ(fw.FindIndexLowerBound(3.0f), 2u);
    EXPECT_EQ(fw.FindIndexLowerBound(14.0f), 4u);
    EXPECT_EQ(fw.FindIndexLowerBound(15.0f), 5u);
    EXPECT_EQ(fw.FindIndexLowerBound(100.0f), 5u);
}

TEST(FenwickTest, FindIndexLowerBoundSingle)
{
    FloatFenwick fw;
    fw.Resize(1);
    fw.Set(0, 10.0f);
    EXPECT_EQ(fw.FindIndexLowerBound(0.0f), 0u);
    EXPECT_EQ(fw.FindIndexLowerBound(9.99f), 0u);
    EXPECT_EQ(fw.FindIndexLowerBound(10.0f), 1u);
    EXPECT_EQ(fw.FindIndexLowerBound(20.0f), 1u);
}

TEST(FenwickTest, FindIndexLowerBoundEqualValues)
{
    FloatFenwick fw;
    const float values[] = { 5.0f, 5.0f, 5.0f, 5.0f };
    fw.Resize(std::size(values));
    fw.Build(std::span<const float>(values, std::size(values)));
    // PrefixSum: [0, 5, 10, 15, 20]
    EXPECT_EQ(fw.FindIndexLowerBound(0.0f), 0u);
    EXPECT_EQ(fw.FindIndexLowerBound(4.99f), 0u);
    EXPECT_EQ(fw.FindIndexLowerBound(5.0f), 1u);
    EXPECT_EQ(fw.FindIndexLowerBound(10.0f), 2u);
    EXPECT_EQ(fw.FindIndexLowerBound(15.0f), 3u);
    EXPECT_EQ(fw.FindIndexLowerBound(19.99f), 3u);
    EXPECT_EQ(fw.FindIndexLowerBound(20.0f), 4u);
}

TEST(FenwickTest, FindIndexLowerBoundAfterGrowTo)
{
    FloatFenwick fw;
    fw.Resize(3);
    fw.Set(0, 1.0f);
    fw.Set(1, 2.0f);
    fw.Set(2, 3.0f);
    fw.GrowTo(5);
    // PrefixSum: [0, 1, 3, 6, 6, 6]。新規範囲は 0 値なのでヒットしない。
    EXPECT_EQ(fw.FindIndexLowerBound(0.5f), 0u);
    EXPECT_EQ(fw.FindIndexLowerBound(2.5f), 1u);
    EXPECT_EQ(fw.FindIndexLowerBound(5.0f), 2u);
    EXPECT_EQ(fw.FindIndexLowerBound(6.0f), 5u); // 該当なし
}

TEST(FenwickTest, FindIndexLowerBoundNonPow2Size)
{
    // n=7 (非 2 冪) で std::bit_floor(7)=4 から走査が始まる。境界条件確認。
    FloatFenwick fw;
    const float values[] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    fw.Resize(std::size(values));
    fw.Build(std::span<const float>(values, std::size(values)));
    for (size_t k = 0; k < 7; ++k) {
        EXPECT_EQ(fw.FindIndexLowerBound(static_cast<float>(k)), k);
    }
    EXPECT_EQ(fw.FindIndexLowerBound(7.0f), 7u);
}

TEST(FenwickTest, LargeSize)
{
    constexpr size_t N = 10000;
    FloatFenwick fw;
    fw.Resize(N);
    for (size_t i = 0; i < N; ++i) {
        fw.Set(i, 1.0f);
    }
    EXPECT_FLOAT_EQ(fw.PrefixSum(N), static_cast<float>(N));
    EXPECT_FLOAT_EQ(fw.PrefixSum(N / 2), static_cast<float>(N / 2));
}
