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
