#include <gtest/gtest.h>
#include "ui_constants.h"

// ═══════════════════════════════════════════════
// PointInRect
// ═══════════════════════════════════════════════

TEST(PointInRectTest, InsideRect) {
    D2D1_RECT_F r = D2D1::RectF(10.0f, 20.0f, 50.0f, 60.0f);
    EXPECT_TRUE(PointInRect(30.0f, 40.0f, r));
}

TEST(PointInRectTest, OnLeftEdgeIsInside) {
    D2D1_RECT_F r = D2D1::RectF(10.0f, 20.0f, 50.0f, 60.0f);
    EXPECT_TRUE(PointInRect(10.0f, 40.0f, r));
}

TEST(PointInRectTest, OnTopEdgeIsInside) {
    D2D1_RECT_F r = D2D1::RectF(10.0f, 20.0f, 50.0f, 60.0f);
    EXPECT_TRUE(PointInRect(30.0f, 20.0f, r));
}

TEST(PointInRectTest, OnRightEdgeIsOutside) {
    D2D1_RECT_F r = D2D1::RectF(10.0f, 20.0f, 50.0f, 60.0f);
    EXPECT_FALSE(PointInRect(50.0f, 40.0f, r));
}

TEST(PointInRectTest, OnBottomEdgeIsOutside) {
    D2D1_RECT_F r = D2D1::RectF(10.0f, 20.0f, 50.0f, 60.0f);
    EXPECT_FALSE(PointInRect(30.0f, 60.0f, r));
}

TEST(PointInRectTest, OutsideLeft) {
    D2D1_RECT_F r = D2D1::RectF(10.0f, 20.0f, 50.0f, 60.0f);
    EXPECT_FALSE(PointInRect(5.0f, 40.0f, r));
}

TEST(PointInRectTest, OutsideAbove) {
    D2D1_RECT_F r = D2D1::RectF(10.0f, 20.0f, 50.0f, 60.0f);
    EXPECT_FALSE(PointInRect(30.0f, 10.0f, r));
}

TEST(PointInRectTest, TopLeftCorner) {
    D2D1_RECT_F r = D2D1::RectF(10.0f, 20.0f, 50.0f, 60.0f);
    EXPECT_TRUE(PointInRect(10.0f, 20.0f, r));
}

TEST(PointInRectTest, BottomRightCornerIsOutside) {
    D2D1_RECT_F r = D2D1::RectF(10.0f, 20.0f, 50.0f, 60.0f);
    EXPECT_FALSE(PointInRect(50.0f, 60.0f, r));
}

TEST(PointInRectTest, ZeroSizeRect) {
    D2D1_RECT_F r = D2D1::RectF(10.0f, 10.0f, 10.0f, 10.0f);
    EXPECT_FALSE(PointInRect(10.0f, 10.0f, r));
}

// ═══════════════════════════════════════════════
// PaneCloseButtonRect
// ═══════════════════════════════════════════════

TEST(PaneCloseButtonRectTest, ButtonFitsInHeader) {
    float pane_width = 220.0f;
    float header_height = 32.0f;
    auto r = PaneCloseButtonRect(pane_width, header_height);

    EXPECT_GE(r.left, 0.0f);
    EXPECT_LE(r.right, pane_width);
    EXPECT_GE(r.top, 0.0f);
    EXPECT_LE(r.bottom, header_height);
}

TEST(PaneCloseButtonRectTest, ButtonIsSquare) {
    auto r = PaneCloseButtonRect(220.0f, 32.0f);
    float width = r.right - r.left;
    float height = r.bottom - r.top;
    EXPECT_FLOAT_EQ(width, height);
}

TEST(PaneCloseButtonRectTest, ButtonIsOnRightSide) {
    float pane_width = 220.0f;
    auto r = PaneCloseButtonRect(pane_width, 32.0f);
    float center_x = (r.left + r.right) / 2.0f;
    EXPECT_GT(center_x, pane_width / 2.0f);
}

TEST(PaneCloseButtonRectTest, ButtonIsVerticallyCentered) {
    float header_height = 32.0f;
    auto r = PaneCloseButtonRect(220.0f, header_height);
    float center_y = (r.top + r.bottom) / 2.0f;
    EXPECT_NEAR(center_y, header_height / 2.0f, 0.01f);
}

TEST(PaneCloseButtonRectTest, ButtonSizeScalesWithHeaderHeight) {
    auto r1 = PaneCloseButtonRect(220.0f, 32.0f);
    auto r2 = PaneCloseButtonRect(220.0f, 64.0f);
    float size1 = r1.right - r1.left;
    float size2 = r2.right - r2.left;
    EXPECT_GT(size2, size1);
}

TEST(PaneCloseButtonRectTest, ButtonPositionAdaptsToWidth) {
    auto r1 = PaneCloseButtonRect(200.0f, 32.0f);
    auto r2 = PaneCloseButtonRect(400.0f, 32.0f);
    // ボタンサイズは同じだが、右寄せなので位置が異なる
    float size1 = r1.right - r1.left;
    float size2 = r2.right - r2.left;
    EXPECT_FLOAT_EQ(size1, size2);
    EXPECT_GT(r2.left, r1.left);
}
