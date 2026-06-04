#include <gtest/gtest.h>
#include "ui_constants.h"

// ═══════════════════════════════════════════════
// PointInRect
// ═══════════════════════════════════════════════

TEST(PointInRectTest, InsideRect)
{
    D2D1_RECT_F r = D2D1::RectF(10.0f, 20.0f, 50.0f, 60.0f);
    EXPECT_TRUE(PointInRect(30.0f, 40.0f, r));
}

TEST(PointInRectTest, OnLeftEdgeIsInside)
{
    D2D1_RECT_F r = D2D1::RectF(10.0f, 20.0f, 50.0f, 60.0f);
    EXPECT_TRUE(PointInRect(10.0f, 40.0f, r));
}

TEST(PointInRectTest, OnTopEdgeIsInside)
{
    D2D1_RECT_F r = D2D1::RectF(10.0f, 20.0f, 50.0f, 60.0f);
    EXPECT_TRUE(PointInRect(30.0f, 20.0f, r));
}

TEST(PointInRectTest, OnRightEdgeIsOutside)
{
    D2D1_RECT_F r = D2D1::RectF(10.0f, 20.0f, 50.0f, 60.0f);
    EXPECT_FALSE(PointInRect(50.0f, 40.0f, r));
}

TEST(PointInRectTest, OnBottomEdgeIsOutside)
{
    D2D1_RECT_F r = D2D1::RectF(10.0f, 20.0f, 50.0f, 60.0f);
    EXPECT_FALSE(PointInRect(30.0f, 60.0f, r));
}

TEST(PointInRectTest, OutsideLeft)
{
    D2D1_RECT_F r = D2D1::RectF(10.0f, 20.0f, 50.0f, 60.0f);
    EXPECT_FALSE(PointInRect(5.0f, 40.0f, r));
}

TEST(PointInRectTest, OutsideAbove)
{
    D2D1_RECT_F r = D2D1::RectF(10.0f, 20.0f, 50.0f, 60.0f);
    EXPECT_FALSE(PointInRect(30.0f, 10.0f, r));
}

TEST(PointInRectTest, TopLeftCorner)
{
    D2D1_RECT_F r = D2D1::RectF(10.0f, 20.0f, 50.0f, 60.0f);
    EXPECT_TRUE(PointInRect(10.0f, 20.0f, r));
}

TEST(PointInRectTest, BottomRightCornerIsOutside)
{
    D2D1_RECT_F r = D2D1::RectF(10.0f, 20.0f, 50.0f, 60.0f);
    EXPECT_FALSE(PointInRect(50.0f, 60.0f, r));
}

TEST(PointInRectTest, ZeroSizeRect)
{
    D2D1_RECT_F r = D2D1::RectF(10.0f, 10.0f, 10.0f, 10.0f);
    EXPECT_FALSE(PointInRect(10.0f, 10.0f, r));
}

// ═══════════════════════════════════════════════
// SnapToPhysicalPixel
// ═══════════════════════════════════════════════

// 整数値はスナップしても変わらない
TEST(SnapToPhysicalPixelTest, IntegerValueUnchanged_100Percent)
{
    EXPECT_FLOAT_EQ(SnapToPhysicalPixel(10.0f, 1.0f), 10.0f);
    EXPECT_FLOAT_EQ(SnapToPhysicalPixel(0.0f, 1.0f), 0.0f);
    EXPECT_FLOAT_EQ(SnapToPhysicalPixel(999.0f, 1.0f), 999.0f);
}

// 100% DPI: サブピクセル値は最寄りの整数にスナップされる
TEST(SnapToPhysicalPixelTest, SubPixelSnaps_100Percent)
{
    EXPECT_FLOAT_EQ(SnapToPhysicalPixel(10.3f, 1.0f), 10.0f);
    EXPECT_FLOAT_EQ(SnapToPhysicalPixel(10.7f, 1.0f), 11.0f);
    EXPECT_FLOAT_EQ(SnapToPhysicalPixel(10.5f, 1.0f), 11.0f); // std::round は0.5を0から離れる方向に丸める
}

// 150% DPI: 物理ピクセル境界 = 1/1.5 DIP刻み
TEST(SnapToPhysicalPixelTest, SubPixelSnaps_150Percent)
{
    float scale = 1.5f;
    EXPECT_FLOAT_EQ(SnapToPhysicalPixel(10.0f, scale), 10.0f);
    EXPECT_NEAR(SnapToPhysicalPixel(10.2f, scale), 10.0f, 1e-5f);
    EXPECT_NEAR(SnapToPhysicalPixel(10.4f, scale), 16.0f / 1.5f, 1e-5f);
}

// 200% DPI: 物理ピクセル境界 = 0.5 DIP刻み
TEST(SnapToPhysicalPixelTest, SubPixelSnaps_200Percent)
{
    float scale = 2.0f;
    EXPECT_FLOAT_EQ(SnapToPhysicalPixel(10.0f, scale), 10.0f);
    EXPECT_FLOAT_EQ(SnapToPhysicalPixel(10.3f, scale), 10.5f);
    EXPECT_FLOAT_EQ(SnapToPhysicalPixel(10.1f, scale), 10.0f);
    EXPECT_FLOAT_EQ(SnapToPhysicalPixel(10.75f, scale), 11.0f);
}

// 200% DPI: ピクセル境界上の値はそのまま保持される
TEST(SnapToPhysicalPixelTest, HalfDipValuesPreserved_200Percent)
{
    float scale = 2.0f;
    EXPECT_FLOAT_EQ(SnapToPhysicalPixel(10.5f, scale), 10.5f);
    EXPECT_FLOAT_EQ(SnapToPhysicalPixel(11.0f, scale), 11.0f);
}

// ゼロは常にゼロ
TEST(SnapToPhysicalPixelTest, ZeroRemainsZero)
{
    EXPECT_FLOAT_EQ(SnapToPhysicalPixel(0.0f, 1.0f), 0.0f);
    EXPECT_FLOAT_EQ(SnapToPhysicalPixel(0.0f, 1.5f), 0.0f);
    EXPECT_FLOAT_EQ(SnapToPhysicalPixel(0.0f, 2.0f), 0.0f);
}

// 大きい値でも正しくスナップされる
TEST(SnapToPhysicalPixelTest, LargeValueSnaps)
{
    EXPECT_FLOAT_EQ(SnapToPhysicalPixel(12345.3f, 1.0f), 12345.0f);
    EXPECT_FLOAT_EQ(SnapToPhysicalPixel(12345.7f, 1.0f), 12346.0f);
}

// 125% DPI: 整数DIPが非整数物理ピクセルになるケース
TEST(SnapToPhysicalPixelTest, SubPixelSnaps_125Percent)
{
    float scale = 1.25f;
    // 10.0 * 1.25 = 12.5 → std::round(12.5) = 13 → 13 / 1.25 = 10.4
    EXPECT_NEAR(SnapToPhysicalPixel(10.0f, scale), 10.4f, 1e-5f);
}

// スムーススクロール補間で生成される典型的な端数値
TEST(SnapToPhysicalPixelTest, SmoothScrollInterpolationValues)
{
    float scale = 1.0f;
    EXPECT_FLOAT_EQ(SnapToPhysicalPixel(25.0f, scale), 25.0f);
    EXPECT_FLOAT_EQ(SnapToPhysicalPixel(43.75f, scale), 44.0f);
    EXPECT_FLOAT_EQ(SnapToPhysicalPixel(57.8125f, scale), 58.0f);
}

// スナップ前後でスクロール差が1物理ピクセル未満であることを確認
TEST(SnapToPhysicalPixelTest, SnapErrorWithinOnePixel)
{
    float scales[] = { 1.0f, 1.25f, 1.5f, 1.75f, 2.0f };
    for (float scale : scales) {
        for (float v = 0.0f; v < 100.0f; v += 0.1f) {
            float snapped = SnapToPhysicalPixel(v, scale);
            float error_in_pixels = std::abs((snapped - v) * scale);
            EXPECT_LE(error_in_pixels, 0.5f + 1e-5f)
                << "scale=" << scale << " value=" << v;
        }
    }
}

// ═══════════════════════════════════════════════
// PaneCloseButtonRect
// ═══════════════════════════════════════════════

TEST(PaneCloseButtonRectTest, ButtonFitsInHeader)
{
    float pane_width = 220.0f;
    float header_height = 32.0f;
    auto r = PaneCloseButtonRect(pane_width, header_height);

    EXPECT_GE(r.left, 0.0f);
    EXPECT_LE(r.right, pane_width);
    EXPECT_GE(r.top, 0.0f);
    EXPECT_LE(r.bottom, header_height);
}

TEST(PaneCloseButtonRectTest, ButtonIsSquare)
{
    auto r = PaneCloseButtonRect(220.0f, 32.0f);
    float width = r.right - r.left;
    float height = r.bottom - r.top;
    EXPECT_FLOAT_EQ(width, height);
}

TEST(PaneCloseButtonRectTest, ButtonIsOnRightSide)
{
    float pane_width = 220.0f;
    auto r = PaneCloseButtonRect(pane_width, 32.0f);
    float center_x = (r.left + r.right) / 2.0f;
    EXPECT_GT(center_x, pane_width / 2.0f);
}

TEST(PaneCloseButtonRectTest, ButtonIsVerticallyCentered)
{
    float header_height = 32.0f;
    auto r = PaneCloseButtonRect(220.0f, header_height);
    float center_y = (r.top + r.bottom) / 2.0f;
    EXPECT_NEAR(center_y, header_height / 2.0f, 0.01f);
}

TEST(PaneCloseButtonRectTest, ButtonSizeScalesWithHeaderHeight)
{
    auto r1 = PaneCloseButtonRect(220.0f, 32.0f);
    auto r2 = PaneCloseButtonRect(220.0f, 64.0f);
    float size1 = r1.right - r1.left;
    float size2 = r2.right - r2.left;
    EXPECT_GT(size2, size1);
}

TEST(PaneCloseButtonRectTest, ButtonPositionAdaptsToWidth)
{
    auto r1 = PaneCloseButtonRect(200.0f, 32.0f);
    auto r2 = PaneCloseButtonRect(400.0f, 32.0f);
    // ボタンサイズは同じだが、右寄せなので位置が異なる
    float size1 = r1.right - r1.left;
    float size2 = r2.right - r2.left;
    EXPECT_FLOAT_EQ(size1, size2);
    EXPECT_GT(r2.left, r1.left);
}

// ═══════════════════════════════════════════════
// PaneRefreshButtonRect
// ═══════════════════════════════════════════════

TEST(PaneRefreshButtonRectTest, ButtonFitsInHeader)
{
    float pane_width = 220.0f;
    float header_height = 32.0f;
    auto r = PaneRefreshButtonRect(pane_width, header_height);

    EXPECT_GE(r.left, 0.0f);
    EXPECT_LE(r.right, pane_width);
    EXPECT_GE(r.top, 0.0f);
    EXPECT_LE(r.bottom, header_height);
}

TEST(PaneRefreshButtonRectTest, ButtonIsSquare)
{
    auto r = PaneRefreshButtonRect(220.0f, 32.0f);
    float width = r.right - r.left;
    float height = r.bottom - r.top;
    EXPECT_FLOAT_EQ(width, height);
}

TEST(PaneRefreshButtonRectTest, SameSizeAsCloseButton)
{
    float pane_width = 220.0f;
    float header_height = 32.0f;
    auto close = PaneCloseButtonRect(pane_width, header_height);
    auto refresh = PaneRefreshButtonRect(pane_width, header_height);
    float close_size = close.right - close.left;
    float refresh_size = refresh.right - refresh.left;
    EXPECT_FLOAT_EQ(close_size, refresh_size);
}

TEST(PaneRefreshButtonRectTest, PositionedLeftOfCloseButton)
{
    float pane_width = 220.0f;
    float header_height = 32.0f;
    auto close = PaneCloseButtonRect(pane_width, header_height);
    auto refresh = PaneRefreshButtonRect(pane_width, header_height);
    EXPECT_LT(refresh.right, close.left);
}

TEST(PaneRefreshButtonRectTest, ButtonIsVerticallyCentered)
{
    float header_height = 32.0f;
    auto r = PaneRefreshButtonRect(220.0f, header_height);
    float center_y = (r.top + r.bottom) / 2.0f;
    EXPECT_NEAR(center_y, header_height / 2.0f, 0.01f);
}

TEST(PaneRefreshButtonRectTest, NoOverlapWithCloseButton)
{
    float pane_width = 220.0f;
    float header_height = 32.0f;
    auto close = PaneCloseButtonRect(pane_width, header_height);
    auto refresh = PaneRefreshButtonRect(pane_width, header_height);
    // 更新ボタンの右端が閉じるボタンの左端より小さいことを確認
    EXPECT_LE(refresh.right, close.left);
}

TEST(PaneRefreshButtonRectTest, ButtonPositionAdaptsToWidth)
{
    auto r1 = PaneRefreshButtonRect(200.0f, 32.0f);
    auto r2 = PaneRefreshButtonRect(400.0f, 32.0f);
    // ボタンサイズは同じだが、位置が異なる
    float size1 = r1.right - r1.left;
    float size2 = r2.right - r2.left;
    EXPECT_FLOAT_EQ(size1, size2);
    EXPECT_GT(r2.left, r1.left);
}

TEST(PaneRefreshButtonRectTest, ButtonSizeScalesWithHeaderHeight)
{
    auto r1 = PaneRefreshButtonRect(220.0f, 32.0f);
    auto r2 = PaneRefreshButtonRect(220.0f, 64.0f);
    float size1 = r1.right - r1.left;
    float size2 = r2.right - r2.left;
    EXPECT_GT(size2, size1);
}

// ═══════════════════════════════════════════════
// ComputeSearchBarLayout — close ボタンの右寄せ (issue #253)
// ═══════════════════════════════════════════════

// 十分な幅では close ボタンがバー右端に寄せられる
TEST(SearchBarLayoutTest, CloseButtonRightAlignedWhenWide)
{
    const float md_left = 0.0f;
    const float md_width = 1600.0f;
    const float md_bottom = 900.0f;
    auto l = ComputeSearchBarLayout(md_left, md_width, md_bottom, true);

    const float bar_right = md_left + md_width;
    EXPECT_FLOAT_EQ(l.close_btn.right, bar_right - SEARCH_BAR_PADDING);
    EXPECT_FLOAT_EQ(l.close_btn.left, bar_right - SEARCH_BAR_PADDING - SEARCH_BTN_SIZE);
}

// close ボタンは幅=BTN_SIZE・高さ=INPUT_HEIGHT を維持する
TEST(SearchBarLayoutTest, CloseButtonKeepsButtonSize)
{
    auto l = ComputeSearchBarLayout(0.0f, 1600.0f, 900.0f, true);
    EXPECT_FLOAT_EQ(l.close_btn.right - l.close_btn.left, SEARCH_BTN_SIZE);
    EXPECT_FLOAT_EQ(l.close_btn.bottom - l.close_btn.top, SEARCH_INPUT_HEIGHT);
}

// close ボタンは他ボタンと同じ垂直位置に揃う
TEST(SearchBarLayoutTest, CloseButtonVerticallyAlignedWithOthers)
{
    auto l = ComputeSearchBarLayout(0.0f, 1600.0f, 900.0f, true);
    EXPECT_FLOAT_EQ(l.close_btn.top, l.highlight_btn.top);
    EXPECT_FLOAT_EQ(l.close_btn.bottom, l.highlight_btn.bottom);
}

// 右寄せにより highlight と close の間に隙間が空く
TEST(SearchBarLayoutTest, GapBetweenHighlightAndCloseWhenWide)
{
    auto l = ComputeSearchBarLayout(0.0f, 1600.0f, 900.0f, true);
    // 単に隣接 (gap 分) ではなく、明確に離れている
    EXPECT_GT(l.close_btn.left, l.highlight_btn.right + SEARCH_BAR_GAP);
}

// 右寄せしても left 側のボタン位置は従来の左詰めのまま
TEST(SearchBarLayoutTest, LeftButtonsUnaffectedByRightAlign)
{
    auto l = ComputeSearchBarLayout(0.0f, 1600.0f, 900.0f, true);
    // up/down/case/highlight は icon 起点で gap 刻みに左詰めされ、互いに重ならない
    EXPECT_LT(l.up_btn.right, l.down_btn.left);
    EXPECT_LT(l.down_btn.right, l.case_btn.left);
    EXPECT_LT(l.case_btn.right, l.highlight_btn.left);
    EXPECT_LT(l.highlight_btn.right, l.close_btn.left);
}

// 狭幅では右寄せをやめ、highlight の右隣 (左詰め) にフォールバックする
TEST(SearchBarLayoutTest, CloseButtonFallsBackToLeftPackedWhenNarrow)
{
    const float md_width = 300.0f;
    auto l = ComputeSearchBarLayout(0.0f, md_width, 900.0f, true);

    // フォールバック時は highlight の直後 (gap 分だけ離れて) に並ぶ
    EXPECT_FLOAT_EQ(l.close_btn.left, l.highlight_btn.right + SEARCH_BAR_GAP);
    // 右寄せ位置よりも右側に居る = フォールバックが発動している証拠
    const float right_aligned_x = md_width - SEARCH_BAR_PADDING - SEARCH_BTN_SIZE;
    EXPECT_GT(l.close_btn.left, right_aligned_x);
}

// しきい値幅(≈576 DIP)の前後で右寄せ⇔フォールバックが切り替わる。
// std::max の選択方向 (min への誤改変や不等号ミス) を検出する遷移テスト。
TEST(SearchBarLayoutTest, RightAlignTogglesAroundThresholdWidth)
{
    auto narrow = ComputeSearchBarLayout(0.0f, 560.0f, 900.0f, true);
    auto wide = ComputeSearchBarLayout(0.0f, 600.0f, 900.0f, true);
    // 狭側: 左詰めフォールバック (highlight 直後に並ぶ)
    EXPECT_FLOAT_EQ(narrow.close_btn.left, narrow.highlight_btn.right + SEARCH_BAR_GAP);
    // 広側: バー右端寄せ
    EXPECT_FLOAT_EQ(wide.close_btn.right, 600.0f - SEARCH_BAR_PADDING);
}

// 件数表示なし (has_query=false) では count_rect が消えてレイアウトが詰まるが、
// 狭幅では依然フォールバックする (フォールバック境界は has_query で変わる)
TEST(SearchBarLayoutTest, CloseButtonFallsBackWhenNarrowWithoutQuery)
{
    auto l = ComputeSearchBarLayout(0.0f, 300.0f, 900.0f, false);
    EXPECT_FLOAT_EQ(l.close_btn.left, l.highlight_btn.right + SEARCH_BAR_GAP);
}

// ═══════════════════════════════════════════════
// HitTestSearchBar — 右寄せ close ボタンのヒット判定
// ═══════════════════════════════════════════════

// 右寄せされた close ボタンの中心はちゃんと Close と判定される
TEST(SearchBarHitTestTest, CenterOfRightAlignedCloseHitsClose)
{
    auto l = ComputeSearchBarLayout(0.0f, 1600.0f, 900.0f, true);
    const float cx = (l.close_btn.left + l.close_btn.right) / 2.0f;
    const float cy = (l.close_btn.top + l.close_btn.bottom) / 2.0f;
    EXPECT_EQ(HitTestSearchBar(l, cx, cy), SearchBarHitZone::Close);
}

// highlight と close の間の空白はどのゾーンにも当たらない
TEST(SearchBarHitTestTest, GapBetweenHighlightAndCloseHitsNone)
{
    auto l = ComputeSearchBarLayout(0.0f, 1600.0f, 900.0f, true);
    const float gap_x = (l.highlight_btn.right + l.close_btn.left) / 2.0f;
    const float cy = (l.close_btn.top + l.close_btn.bottom) / 2.0f;
    EXPECT_EQ(HitTestSearchBar(l, gap_x, cy), SearchBarHitZone::None);
}
