#include <gtest/gtest.h>
#include <iterator>
#include <string>
#include "ui_constants.h"

// ═══════════════════════════════════════════════
// PointInRect
// ═══════════════════════════════════════════════

// left/top は内側、right/bottom は外側 (half-open)
TEST(PointInRectTest, HalfOpenBounds)
{
    const D2D1_RECT_F r = D2D1::RectF(10.0f, 20.0f, 50.0f, 60.0f);
    struct Case {
        const char* name;
        float x;
        float y;
        bool inside;
    };
    constexpr Case kCases[] = {
        { "Inside", 30.0f, 40.0f, true },
        { "LeftEdge", 10.0f, 40.0f, true },
        { "TopEdge", 30.0f, 20.0f, true },
        { "TopLeftCorner", 10.0f, 20.0f, true },
        { "RightEdge", 50.0f, 40.0f, false },
        { "BottomEdge", 30.0f, 60.0f, false },
        { "BottomRightCorner", 50.0f, 60.0f, false },
        { "OutsideLeft", 5.0f, 40.0f, false },
        { "OutsideAbove", 30.0f, 10.0f, false },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        EXPECT_EQ(PointInRect(c.x, c.y, r), c.inside);
    }
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
// PaneCloseButtonRect / PaneRefreshButtonRect / PaneRevealButtonRect
// ═══════════════════════════════════════════════

namespace {

struct PaneHeaderButton {
    const char* name;
    D2D1_RECT_F (*rect)(float pane_width, float header_height) noexcept;
};

// 右端から左へ並ぶ順
constexpr PaneHeaderButton kPaneHeaderButtons[] = {
    { "Close", PaneCloseButtonRect },
    { "Refresh", PaneRefreshButtonRect },
    { "Reveal", PaneRevealButtonRect },
};

} // namespace

TEST(PaneHeaderButtonRectTest, FitsInHeader)
{
    constexpr float pane_width = 220.0f;
    constexpr float header_height = 32.0f;
    for (const auto& b : kPaneHeaderButtons) {
        SCOPED_TRACE(b.name);
        const auto r = b.rect(pane_width, header_height);
        EXPECT_GE(r.left, 0.0f);
        EXPECT_LE(r.right, pane_width);
        EXPECT_GE(r.top, 0.0f);
        EXPECT_LE(r.bottom, header_height);
    }
}

TEST(PaneHeaderButtonRectTest, IsSquare)
{
    for (const auto& b : kPaneHeaderButtons) {
        SCOPED_TRACE(b.name);
        const auto r = b.rect(220.0f, 32.0f);
        EXPECT_FLOAT_EQ(r.right - r.left, r.bottom - r.top);
    }
}

TEST(PaneHeaderButtonRectTest, IsVerticallyCentered)
{
    constexpr float header_height = 32.0f;
    for (const auto& b : kPaneHeaderButtons) {
        SCOPED_TRACE(b.name);
        const auto r = b.rect(220.0f, header_height);
        EXPECT_NEAR((r.top + r.bottom) / 2.0f, header_height / 2.0f, 0.01f);
    }
}

TEST(PaneHeaderButtonRectTest, SizeScalesWithHeaderHeight)
{
    for (const auto& b : kPaneHeaderButtons) {
        SCOPED_TRACE(b.name);
        const auto r1 = b.rect(220.0f, 32.0f);
        const auto r2 = b.rect(220.0f, 64.0f);
        EXPECT_GT(r2.right - r2.left, r1.right - r1.left);
    }
}

// 右寄せなのでサイズは同じまま位置だけが動く
TEST(PaneHeaderButtonRectTest, PositionAdaptsToWidth)
{
    for (const auto& b : kPaneHeaderButtons) {
        SCOPED_TRACE(b.name);
        const auto r1 = b.rect(200.0f, 32.0f);
        const auto r2 = b.rect(400.0f, 32.0f);
        EXPECT_FLOAT_EQ(r1.right - r1.left, r2.right - r2.left);
        EXPECT_GT(r2.left, r1.left);
    }
}

TEST(PaneHeaderButtonRectTest, SitsLeftOfNeighborWithSameSize)
{
    for (size_t i = 1; i < std::size(kPaneHeaderButtons); ++i) {
        const auto& left_btn = kPaneHeaderButtons[i];
        const auto& right_btn = kPaneHeaderButtons[i - 1];
        SCOPED_TRACE(std::string{ left_btn.name } + " vs " + right_btn.name);
        const auto l = left_btn.rect(220.0f, 32.0f);
        const auto r = right_btn.rect(220.0f, 32.0f);
        EXPECT_FLOAT_EQ(l.right - l.left, r.right - r.left);
        EXPECT_FLOAT_EQ(l.bottom - l.top, r.bottom - r.top);
        EXPECT_FLOAT_EQ(l.top, r.top);
        EXPECT_LT(l.right, r.left);
    }
}

TEST(PaneCloseButtonRectTest, ButtonIsOnRightSide)
{
    float pane_width = 220.0f;
    auto r = PaneCloseButtonRect(pane_width, 32.0f);
    float center_x = (r.left + r.right) / 2.0f;
    EXPECT_GT(center_x, pane_width / 2.0f);
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

// 右寄せ⇔フォールバックの遷移を検証する。しきい値幅は SEARCH_* 定数やボタン構成に依存して
// 動くため、560/600 のようなマジックナンバーではなく ComputeSearchBarLayout 自身を走査して
// 遷移点を発見し、その前後で挙動が切り替わることを確認する。
// std::max の選択方向ミス (min 化・不等号反転) を検出するのが狙い。
TEST(SearchBarLayoutTest, RightAlignTogglesAroundThresholdWidth)
{
    // 右寄せなら close は highlight から離れ、フォールバックなら highlight 直後に密着する
    auto right_aligned = [](float w) {
        auto l = ComputeSearchBarLayout(0.0f, w, 900.0f, true);
        return l.close_btn.left > l.highlight_btn.right + SEARCH_BAR_GAP + 0.5f;
    };
    // 十分狭ければフォールバック、十分広ければ右寄せ (遷移が存在する両端)
    ASSERT_FALSE(right_aligned(MD_PANE_MIN_WIDTH));
    ASSERT_TRUE(right_aligned(SEARCH_INPUT_MAX_WIDTH * 4.0f));
    // 遷移点を発見し、その直下がフォールバック・しきい値以上が右寄せであることを確認
    float threshold = MD_PANE_MIN_WIDTH;
    for (float w = MD_PANE_MIN_WIDTH; w <= SEARCH_INPUT_MAX_WIDTH * 4.0f; w += 1.0f) {
        if (right_aligned(w)) {
            threshold = w;
            break;
        }
    }
    EXPECT_FALSE(right_aligned(threshold - 1.0f));
    EXPECT_TRUE(right_aligned(threshold));
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
