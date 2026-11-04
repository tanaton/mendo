#include <gtest/gtest.h>
#include "diagram_view.h"

using diagram_view::FitZoom;
using diagram_view::NextZoom;

// 全体表示は余白を除いた窓に収め、等倍より大きくしない。潰した窓では最小の倍率で止める。
TEST(DiagramViewZoom, FitsInsidePaddingWithoutEnlarging)
{
    EXPECT_FLOAT_EQ(FitZoom(1000.0f, 800.0f, 200.0f, 100.0f), 1.0f);
    EXPECT_FLOAT_EQ(FitZoom(432.0f, 1000.0f, 800.0f, 100.0f), 0.5f);
    EXPECT_FLOAT_EQ(FitZoom(1000.0f, 232.0f, 100.0f, 400.0f), 0.5f);
    EXPECT_FLOAT_EQ(FitZoom(10.0f, 10.0f, 1000.0f, 1000.0f), 0.05f);
}

// ボタンの止まる位置は表の値と全体表示の倍率。全体表示のすぐ隣の値は飛ばす。
TEST(DiagramViewZoom, StepsThroughStopsIncludingFit)
{
    EXPECT_FLOAT_EQ(*NextZoom(1.0f, 1.0f, 1), 1.25f);
    EXPECT_FLOAT_EQ(*NextZoom(1.0f, 1.0f, -1), 0.75f);
    // 全体表示 0.7 は止まる位置になり、すぐ隣の 0.67 と 0.75 は飛ばす
    EXPECT_FLOAT_EQ(*NextZoom(0.7f, 0.7f, 1), 1.0f);
    EXPECT_FLOAT_EQ(*NextZoom(0.7f, 0.7f, -1), 0.5f);
    EXPECT_FLOAT_EQ(*NextZoom(1.0f, 0.7f, -1), 0.7f);
    EXPECT_FALSE(NextZoom(8.0f, 1.0f, 1));
    EXPECT_FALSE(NextZoom(0.05f, 0.05f, -1));
}
