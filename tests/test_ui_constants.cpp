#include <gtest/gtest.h>
#include <format>
#include <iterator>
#include "chrome_layout.h"
#include "pane_layout.h"
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
    // 大きさ 0 の矩形はどの点も含まない
    EXPECT_FALSE(PointInRect(10.0f, 10.0f, D2D1::RectF(10.0f, 10.0f, 10.0f, 10.0f)));
}

// ═══════════════════════════════════════════════
// SnapToPhysicalPixel
// ═══════════════════════════════════════════════

// 物理ピクセル境界 (1/scale DIP 刻み) の最寄りへ寄せる。境界上の値はそのまま。
TEST(SnapToPhysicalPixelTest, SnapsToNearestPhysicalPixel)
{
    struct Case {
        float dip;
        float scale;
        float expected;
    };
    constexpr Case kCases[] = {
        { 0.0f, 1.0f, 0.0f },
        { 10.0f, 1.0f, 10.0f },
        { 25.0f, 1.0f, 25.0f },
        { 999.0f, 1.0f, 999.0f },
        { 10.3f, 1.0f, 10.0f },
        { 10.7f, 1.0f, 11.0f },
        // std::round は 0.5 を 0 から離れる方向に丸める
        { 10.5f, 1.0f, 11.0f },
        { 12345.3f, 1.0f, 12345.0f },
        { 12345.7f, 1.0f, 12346.0f },
        // スムーススクロール補間で生成される典型的な端数値
        { 43.75f, 1.0f, 44.0f },
        { 57.8125f, 1.0f, 58.0f },
        // 10.0 * 1.25 = 12.5 → round で 13 → 13 / 1.25 = 10.4 (整数 DIP が非整数物理ピクセルになる)
        { 10.0f, 1.25f, 10.4f },
        { 0.0f, 1.5f, 0.0f },
        { 10.0f, 1.5f, 10.0f },
        { 10.2f, 1.5f, 10.0f },
        { 10.4f, 1.5f, 16.0f / 1.5f },
        { 0.0f, 2.0f, 0.0f },
        { 10.0f, 2.0f, 10.0f },
        { 10.1f, 2.0f, 10.0f },
        { 10.3f, 2.0f, 10.5f },
        { 10.5f, 2.0f, 10.5f },
        { 10.75f, 2.0f, 11.0f },
        { 11.0f, 2.0f, 11.0f },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::Message() << "dip=" << c.dip << " scale=" << c.scale);
        EXPECT_NEAR(SnapToPhysicalPixel(c.dip, c.scale), c.expected, 1e-5f);
    }
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
// PaneHeaderButtonRect
// ═══════════════════════════════════════════════

namespace {

// 右端から左へ並ぶ順
constexpr PaneHeaderButton kPaneHeaderButtons[] = { PaneHeaderButton::Close, PaneHeaderButton::Refresh, PaneHeaderButton::Reveal };

} // namespace

// 正方形でヘッダーの高さの中央に収まる。大きさはヘッダーの高さに従い、右寄せなのでペイン幅では位置だけが動く。
TEST(PaneHeaderButtonRectTest, SquareCenteredInHeaderAndRightAligned)
{
    constexpr float pane_width = 220.0f;
    constexpr float header_height = 32.0f;
    for (const auto button : kPaneHeaderButtons) {
        SCOPED_TRACE(static_cast<int>(button));
        const auto r = PaneHeaderButtonRect(pane_width, header_height, button);
        EXPECT_GE(r.left, 0.0f);
        EXPECT_LE(r.right, pane_width);
        EXPECT_FLOAT_EQ(r.right - r.left, r.bottom - r.top);
        EXPECT_NEAR(RectCenter(r).y, header_height / 2.0f, 0.01f);

        const auto taller = PaneHeaderButtonRect(pane_width, 64.0f, button);
        EXPECT_GT(taller.right - taller.left, r.right - r.left);

        const auto wider = PaneHeaderButtonRect(pane_width + 200.0f, header_height, button);
        EXPECT_FLOAT_EQ(wider.right - wider.left, r.right - r.left);
        EXPECT_FLOAT_EQ(wider.left, r.left + 200.0f);
    }
}

TEST(PaneHeaderButtonRectTest, SitsLeftOfNeighborWithSameSize)
{
    for (size_t i = 1; i < std::size(kPaneHeaderButtons); ++i) {
        SCOPED_TRACE(i);
        const auto l = PaneHeaderButtonRect(220.0f, 32.0f, kPaneHeaderButtons[i]);
        const auto r = PaneHeaderButtonRect(220.0f, 32.0f, kPaneHeaderButtons[i - 1]);
        EXPECT_FLOAT_EQ(l.right - l.left, r.right - r.left);
        EXPECT_FLOAT_EQ(l.top, r.top);
        EXPECT_LT(l.right, r.left);
    }
}

// ═══════════════════════════════════════════════
// ComputeSearchBarLayout — close ボタンの右寄せ (issue #253)
// ═══════════════════════════════════════════════

namespace {

constexpr float kWideBarWidth = 1600.0f;

SearchBarLayout SearchBarAt(float md_width, bool has_query = true)
{
    return ComputeSearchBarLayout(PaneRect{ .x = 0.0f, .width = md_width, .height = 900.0f }, has_query);
}

} // namespace

// 十分な幅では close ボタンだけバー右端へ寄せ、大きさと高さは他のボタンと揃える。
// 左側のボタンは従来どおり左詰めで重ならず、highlight と close の間が空く。
TEST(SearchBarLayoutTest, CloseButtonRightAlignedWhenWide)
{
    const auto l = SearchBarAt(kWideBarWidth);
    EXPECT_FLOAT_EQ(l.close_btn.right, kWideBarWidth - SEARCH_BAR_PADDING);
    EXPECT_FLOAT_EQ(l.close_btn.left, kWideBarWidth - SEARCH_BAR_PADDING - SEARCH_BTN_SIZE);
    EXPECT_FLOAT_EQ(l.close_btn.bottom - l.close_btn.top, SEARCH_INPUT_HEIGHT);
    EXPECT_FLOAT_EQ(l.close_btn.top, l.highlight_btn.top);
    EXPECT_FLOAT_EQ(l.close_btn.bottom, l.highlight_btn.bottom);

    EXPECT_LT(l.up_btn.right, l.down_btn.left);
    EXPECT_LT(l.down_btn.right, l.case_btn.left);
    EXPECT_LT(l.case_btn.right, l.highlight_btn.left);
    // 単に隣接 (gap 分) ではなく、明確に離れている
    EXPECT_GT(l.close_btn.left, l.highlight_btn.right + SEARCH_BAR_GAP);
}

// 狭幅では右寄せをやめ、highlight の右隣 (左詰め) にフォールバックする。
// 件数表示なし (has_query=false) では count_rect が消えて詰まるが、それでもフォールバックする。
TEST(SearchBarLayoutTest, CloseButtonFallsBackToLeftPackedWhenNarrow)
{
    constexpr float md_width = 300.0f;
    for (const bool has_query : { true, false }) {
        SCOPED_TRACE(has_query);
        const auto l = SearchBarAt(md_width, has_query);
        EXPECT_FLOAT_EQ(l.close_btn.left, l.highlight_btn.right + SEARCH_BAR_GAP);
        // 右寄せ位置よりも右側に居る = フォールバックが発動している証拠
        EXPECT_GT(l.close_btn.left, md_width - SEARCH_BAR_PADDING - SEARCH_BTN_SIZE);
    }
}

// 右寄せ⇔フォールバックの遷移を検証する。しきい値幅は SEARCH_* 定数やボタン構成に依存して
// 動くため、560/600 のようなマジックナンバーではなく ComputeSearchBarLayout 自身を走査して
// 遷移点を発見し、その前後で挙動が切り替わることを確認する。
// std::max の選択方向ミス (min 化・不等号反転) を検出するのが狙い。
TEST(SearchBarLayoutTest, RightAlignTogglesAroundThresholdWidth)
{
    // 右寄せなら close は highlight から離れ、フォールバックなら highlight 直後に密着する
    auto right_aligned = [](float w) static {
        const auto l = SearchBarAt(w);
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

// ═══════════════════════════════════════════════
// HitTestSearchBar — 右寄せ close ボタンのヒット判定
// ═══════════════════════════════════════════════

// 右寄せされた close ボタンの中心は Close、highlight との間の空白はどのゾーンにも当たらない。
TEST(SearchBarHitTestTest, RightAlignedCloseAndGap)
{
    const auto l = SearchBarAt(kWideBarWidth);
    const auto [cx, cy] = RectCenter(l.close_btn);
    EXPECT_EQ(HitTestSearchBar(l, cx, cy), SearchBarHitZone::Close);
    EXPECT_EQ(HitTestSearchBar(l, (l.highlight_btn.right + l.close_btn.left) / 2.0f, cy), SearchBarHitZone::None);
}

// ═══════════════════════════════════════════════
// PaneHeaderTextRect
// ═══════════════════════════════════════════════

// サイド幅はウィンドウ縮小で PANE_MIN_WIDTH 未満にもなる (PaneController::ComputeLayout の比例縮小)。
// どの幅でも見出し矩形は反転せず、ボタンと重ならない (bb25241 の回帰)。
TEST(PaneHeaderTextRectTest, NeverInvertedNorOverlapsButtonsAtAnyWidth)
{
    for (const float header_h : { 24.0f, 28.0f, 36.0f }) {
        for (const bool file_buttons : { false, true }) {
            for (float w = 0.0f; w <= 400.0f; w += 0.25f) {
                const auto rect = PaneHeaderTextRect(w, header_h, file_buttons);
                if (!rect) {
                    continue;
                }
                // 失敗したときだけ組み立てる (9600 通りで毎回 SCOPED_TRACE を作らない)。
                const auto where = [&] { return std::format("w={} {} h={}", w, file_buttons ? "file" : "toc", header_h); };
                ASSERT_LT(rect->left, rect->right) << where();
                ASSERT_GE(rect->left, 0.0f) << where();
                ASSERT_LE(rect->right, PaneHeaderButtonRect(w, header_h, PaneHeaderButton::Close).left) << where();
                if (file_buttons) {
                    ASSERT_LE(rect->right, PaneHeaderButtonRect(w, header_h, PaneHeaderButton::Refresh).left) << where();
                    ASSERT_LE(rect->right, PaneHeaderButtonRect(w, header_h, PaneHeaderButton::Reveal).left) << where();
                }
            }
            // 通常幅では見出しが出る (常に nullopt で上のループを素通りしないこと)。
            EXPECT_TRUE(PaneHeaderTextRect(220.0f, header_h, file_buttons).has_value());
        }
    }
}

// ═══════════════════════════════════════════════
// DPI 変換
// ═══════════════════════════════════════════════

// DPI 未初期化 (0) で 0 除算や 0 倍にならないよう等倍に倒す。
TEST(DpiScaleFromTest, NonPositiveDpiFallsBackToOne)
{
    EXPECT_FLOAT_EQ(DpiScaleFrom(0.0f), 1.0f);
    EXPECT_FLOAT_EQ(DpiScaleFrom(-96.0f), 1.0f);
    EXPECT_FLOAT_EQ(DpiScaleFrom(96.0f), 1.0f);
    EXPECT_FLOAT_EQ(DpiScaleFrom(144.0f), 1.5f);
}

TEST(DipToPixelTest, FloorAndCeilVariants)
{
    EXPECT_EQ(DipToPixel(10.0f, 1.5f), 15);
    EXPECT_EQ(DipToPixel(10.1f, 1.5f), 15);
    EXPECT_EQ(DipToPixelCeil(10.1f, 1.5f), 16);
    EXPECT_EQ(DipToPixelCeil(10.0f, 1.5f), 15);
}

TEST(PixelToDipTest, DividesByScale)
{
    const auto p = PixelToDip(300, 150, 1.5f);
    EXPECT_FLOAT_EQ(p.x, 200.0f);
    EXPECT_FLOAT_EQ(p.y, 100.0f);
}

// ═══════════════════════════════════════════════
// ブロック横スクロールバー
// ═══════════════════════════════════════════════

// つまみの幅は可視幅 × 可視の割合。最小幅で止めるが、可視幅が最小幅より狭ければ可視幅に収める。中身の幅が 0 以下なら 0。
TEST(BlockHScrollbarThumbWidthTest, ProportionalAndClamped)
{
    struct Case {
        float visible;
        float natural;
        float expected;
    };
    const Case kCases[] = {
        { 200.0f, 400.0f, 100.0f },
        { 200.0f, 100000.0f, PANE_SCROLLBAR_THUMB_MIN },
        { 200.0f, 100.0f, 200.0f },
        { PANE_SCROLLBAR_THUMB_MIN * 0.5f, 100000.0f, PANE_SCROLLBAR_THUMB_MIN * 0.5f },
        { 200.0f, 0.0f, 0.0f },
        { 200.0f, -1.0f, 0.0f },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::Message() << c.visible << " / " << c.natural);
        EXPECT_FLOAT_EQ(BlockHScrollbarThumbWidth(c.visible, c.natural), c.expected);
    }
}

// バーはブロック下端の内側に収まり、ヒット矩形はバーを上下に広げたもの。
TEST(BlockHScrollbarGeometryTest, BarInsideBlockAndHitRectContainsBar)
{
    const float bar_y = BlockHScrollbarBarY(100.0f, 50.0f, 0.0f);
    EXPECT_LT(bar_y, 150.0f);
    EXPECT_LE(bar_y + PANE_SCROLLBAR_WIDTH, 150.0f);
    EXPECT_FLOAT_EQ(BlockHScrollbarBarY(100.0f, 50.0f, 8.0f), bar_y + 8.0f);

    const auto hit = BlockHScrollbarHitRect(20.0f, 300.0f, bar_y);
    EXPECT_FLOAT_EQ(hit.left, 20.0f);
    EXPECT_FLOAT_EQ(hit.right, 320.0f);
    EXPECT_LT(hit.top, bar_y);
    EXPECT_GT(hit.bottom, bar_y + PANE_SCROLLBAR_WIDTH);
}
