#include <gtest/gtest.h>
#include "cmd_gen_mock_test_base.h"
#include "d2d_util.h"
#include "file_explorer.h"
#include "hit_test_service.h"
#include "pane_layout.h"
#include "search_state.h"
#include "test_nodes.h"
#include "titlebar.h"
#include <algorithm>
#include <optional>
#include <variant>

namespace {

class CmdGenFrameTest : public CmdGenMockTestBase {};

} // namespace

// ═══════════════════════════════════════════════
// クリップ矩形とペイン原点
// ═══════════════════════════════════════════════

TEST_F(CmdGenFrameTest, PushClipMatchesPaneRect)
{
    Parse("Hello");
    const PaneRect pane{ 100.0f, 50.0f, 600.0f, 400.0f };
    auto& cmds = gen_.GenerateMdPane({ .nodes = nodes_, .cache = cache_, .selection = TextSelection{}, .md_pane_rect = pane }, 1.0f);

    const auto clip = FindFirst<PushClipCmd>(cmds);
    ASSERT_TRUE(clip.has_value());
    EXPECT_FLOAT_EQ(clip->rect.left, 100.0f);
    EXPECT_FLOAT_EQ(clip->rect.top, 50.0f);
    EXPECT_FLOAT_EQ(clip->rect.right, 700.0f);
    EXPECT_FLOAT_EQ(clip->rect.bottom, 450.0f);
}

TEST_F(CmdGenFrameTest, TransformTranslatesByPaneOriginOnly)
{
    // Transform の Y は常に 0、スクロール量は描画コマンドの Y に直接合算される。
    Parse("---");
    constexpr float origin_x = 120.0f;
    constexpr float scroll_y = 40.0f;
    const PaneRect pane{ origin_x, 0.0f, 600.0f, 400.0f };
    auto& cmds = gen_.GenerateMdPane({ .nodes = nodes_, .cache = cache_, .selection = TextSelection{}, .md_pane_rect = pane, .scroll_y = scroll_y }, 1.0f);

    const auto first_xform = FindFirst<SetTransformCmd>(cmds);
    ASSERT_TRUE(first_xform.has_value());
    EXPECT_FLOAT_EQ(first_xform->transform._31, origin_x);
    EXPECT_FLOAT_EQ(first_xform->transform._32, 0.0f);

    // スクロール量は HR の Y 座標に CPU 側で直接合算される。
    const auto line = FindFirst<DrawLineCmd>(cmds);
    ASSERT_TRUE(line.has_value());
    const float expected_y = cache_.Top(0) - scroll_y + theme_.paragraph_spacing * 0.5f;
    EXPECT_FLOAT_EQ(line->p0.y, expected_y);
}

TEST_F(CmdGenFrameTest, ScrollSnapsToPixelWithDpi)
{
    // DPI=2.0 → round(scroll_y * 2) / 2 で 0.5px 境界にスナップ。
    // スナップは Transform でなく描画コマンドの Y 計算に反映される。
    Parse("---");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 400.0f };
    const float scroll_y = 0.3f;
    const float dpi = 2.0f;
    auto& cmds = gen_.GenerateMdPane({ .nodes = nodes_, .cache = cache_, .selection = TextSelection{}, .md_pane_rect = pane, .scroll_y = scroll_y }, dpi);

    const auto xform = FindFirst<SetTransformCmd>(cmds);
    ASSERT_TRUE(xform.has_value());
    EXPECT_FLOAT_EQ(xform->transform._32, 0.0f);

    const auto line = FindFirst<DrawLineCmd>(cmds);
    ASSERT_TRUE(line.has_value());
    const float snapped_scroll = 0.5f; // round(0.3 * 2) / 2
    const float expected_y = cache_.Top(0) - snapped_scroll + theme_.paragraph_spacing * 0.5f;
    EXPECT_FLOAT_EQ(line->p0.y, expected_y);
}

// ═══════════════════════════════════════════════
// ビューポートカリング
// ═══════════════════════════════════════════════

TEST_F(CmdGenFrameTest, ScrolledBeyondBottomProducesOnlyFrameCommands)
{
    // 数個の水平線を生成し、ビューポートより大きく下にスクロールする
    Parse("---\n\n---\n\n---");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 100.0f };
    // 全ノードより下にスクロール → DrawLineCmd なし
    auto& cmds = gen_.GenerateMdPane({ .nodes = nodes_, .cache = cache_, .selection = TextSelection{}, .md_pane_rect = pane, .scroll_y = 10000.0f }, 1.0f);

    EXPECT_EQ(CountCmd<DrawLineCmd>(cmds), 0) << "すべてのノードがビューポート上ならDrawLineCmdは生成されない";
    // PushClip + SetTransform×2 + PopClip のみ
    EXPECT_EQ(cmds.size(), 4u);
}

TEST_F(CmdGenFrameTest, EmptyNodesListEmitsOnlyFrameScaffolding)
{
    nodes_.clear();
    cache_ = LayoutCache(0);
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    auto& cmds = gen_.GenerateMdPane({ .nodes = nodes_, .cache = cache_, .selection = TextSelection{}, .md_pane_rect = pane }, 1.0f);

    ASSERT_EQ(cmds.size(), 4u);
    EXPECT_TRUE(std::holds_alternative<PushClipCmd>(cmds[0]));
    EXPECT_TRUE(std::holds_alternative<SetTransformCmd>(cmds[1]));
    EXPECT_TRUE(std::holds_alternative<SetTransformCmd>(cmds[2]));
    EXPECT_TRUE(std::holds_alternative<PopClipCmd>(cmds[3]));
}

// ═══════════════════════════════════════════════
// 検索マッチ
// ═══════════════════════════════════════════════

TEST_F(CmdGenFrameTest, EmptySearchMatchesProduceNoHighlight)
{
    Parse("Hello world");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 400.0f };
    auto& cmds = gen_.GenerateMdPane({ .nodes = nodes_, .cache = cache_, .selection = TextSelection{}, .md_pane_rect = pane, .search_generation = 1 }, 1.0f);
    // マッチ空のため FillRectCmd は選択ハイライト経路以外は生成されない
    // (paragraph text_layout=null なので選択経路も走らない)
    EXPECT_EQ(CountCmd<FillRectCmd>(cmds), 0);
}

// ═══════════════════════════════════════════════
// テーマ変更の反映
// ═══════════════════════════════════════════════

TEST_F(CmdGenFrameTest, TableStripeUsesStripeBrush)
{
    Parse("| A | B |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 400.0f };
    const auto& cmds = gen_.GenerateMdPane({ .nodes = nodes_, .cache = cache_, .selection = TextSelection{}, .md_pane_rect = pane }, 1.0f);
    EXPECT_TRUE(std::ranges::any_of(cmds, [](const DrawCommand& c) static {
        const auto* fr = std::get_if<FillRectCmd>(&c);
        return fr && fr->brush_id == BrushId::TableStripe;
    }));
}

// ストライプはライトで黒、ダークで白の半透明。
TEST(TableStripeColorTest, LightBlackDarkWhiteTranslucent)
{
    const auto light = GetLightTheme().Color(BrushId::TableStripe);
    const auto dark = GetDarkTheme().Color(BrushId::TableStripe);
    EXPECT_FLOAT_EQ(light.r, 0.0f);
    EXPECT_FLOAT_EQ(dark.r, 1.0f);
    for (const auto& c : { light, dark }) {
        EXPECT_GT(c.a, 0.0f);
        EXPECT_LT(c.a, 1.0f);
    }
}

// ═══════════════════════════════════════════════
// 本文ペインに重ねるもの
// ═══════════════════════════════════════════════

namespace {

constexpr PaneRect kOverlayPane{ 100.0f, 50.0f, 600.0f, 400.0f };

} // namespace

// 点はペインの中央を回り、先頭が不透明で後ろほど薄い。
TEST(OverlayCommandsTest, LoadingSpinnerDotsFadeAroundCenter)
{
    DrawCommandList cmds;
    overlay::EmitLoadingSpinner(cmds, 0.0f, kOverlayPane);
    const auto dots = CollectCmds<FillEllipseCmd>(cmds);
    ASSERT_EQ(dots.size(), static_cast<size_t>(spinner::DOT_COUNT));
    EXPECT_EQ(cmds.size(), dots.size());
    EXPECT_FLOAT_EQ(dots[0].opacity, 1.0f);
    EXPECT_NEAR(dots[0].center.x, 400.0f + spinner::RADIUS, 1e-3f);
    EXPECT_NEAR(dots[0].center.y, 250.0f, 1e-3f);
    for (size_t i = 0; i < dots.size(); ++i) {
        EXPECT_EQ(dots[i].brush_id, BrushId::Text);
        EXPECT_NEAR(std::hypot(dots[i].center.x - 400.0f, dots[i].center.y - 250.0f), spinner::RADIUS, 1e-3f);
        if (i > 0) {
            EXPECT_LT(dots[i].opacity, dots[i - 1].opacity);
        }
    }
}

// ボタンはヒットテストと同じ矩形に、押せるか・ホバー中かで決まる不透明度の白黒で塗る。
TEST(OverlayCommandsTest, NavButtonsUseHitTestRectsAndStateOpacity)
{
    TextLayoutCache text_layouts;
    DrawCommandList cmds;
    overlay::EmitNavButtons(cmds, text_layouts, nullptr, kOverlayPane, /*dark=*/false, /*can_back=*/true, /*can_forward=*/false, NavDirection::Back);
    const auto bgs = CollectCmds<FillRoundedRectCmd>(cmds);
    ASSERT_EQ(bgs.size(), 2u);
    EXPECT_EQ(bgs[0].brush_id, BrushId::OverlayBlack);
    EXPECT_FLOAT_EQ(bgs[0].rect.left, NavBackButtonRect(kOverlayPane).left);
    EXPECT_FLOAT_EQ(bgs[1].rect.left, NavForwardButtonRect(kOverlayPane).left);
    EXPECT_FLOAT_EQ(bgs[0].opacity, 0.25f) << "ホバー中";
    EXPECT_FLOAT_EQ(bgs[1].opacity, 0.05f) << "押せない";

    cmds.clear();
    overlay::EmitNavButtons(cmds, text_layouts, nullptr, kOverlayPane, /*dark=*/true, true, true, NavDirection::None);
    const auto dark_bgs = CollectCmds<FillRoundedRectCmd>(cmds);
    ASSERT_EQ(dark_bgs.size(), 2u);
    EXPECT_EQ(dark_bgs[0].brush_id, BrushId::OverlayWhite);
    EXPECT_FLOAT_EQ(dark_bgs[0].opacity, 0.15f);
    EXPECT_FLOAT_EQ(dark_bgs[1].opacity, 0.15f);
}

// ジェスチャーの方向が無ければ何も積まない。パネルはダークで灰色、ライトで黒。
TEST(OverlayCommandsTest, GestureOverlayPanelFollowsTheme)
{
    TextLayoutCache text_layouts;
    DrawCommandList cmds;
    overlay::EmitGestureOverlay(cmds, text_layouts, nullptr, kOverlayPane, false, NavDirection::None);
    EXPECT_TRUE(cmds.empty());

    overlay::EmitGestureOverlay(cmds, text_layouts, nullptr, kOverlayPane, false, NavDirection::Back);
    auto panels = CollectCmds<FillRoundedRectCmd>(cmds);
    ASSERT_EQ(panels.size(), 1u);
    EXPECT_EQ(panels[0].brush_id, BrushId::OverlayBlack);
    EXPECT_FLOAT_EQ(panels[0].opacity, 0.6f);
    EXPECT_FLOAT_EQ((panels[0].rect.left + panels[0].rect.right) * 0.5f, 400.0f);
    EXPECT_FLOAT_EQ(panels[0].rect.right - panels[0].rect.left, GESTURE_OVERLAY_WIDTH);

    cmds.clear();
    overlay::EmitGestureOverlay(cmds, text_layouts, nullptr, kOverlayPane, true, NavDirection::Forward);
    panels = CollectCmds<FillRoundedRectCmd>(cmds);
    ASSERT_EQ(panels.size(), 1u);
    EXPECT_EQ(panels[0].brush_id, BrushId::OverlayGestureBg);
    EXPECT_FLOAT_EQ(panels[0].opacity, 0.8f);
}

// トーストはナビボタンの上に置く。空なら何も積まない。
TEST(OverlayCommandsTest, ToastSitsAboveNavButtons)
{
    TextLayoutCache text_layouts;
    DrawCommandList cmds;
    overlay::EmitToast(cmds, text_layouts, nullptr, kOverlayPane, false, L"");
    EXPECT_TRUE(cmds.empty());

    overlay::EmitToast(cmds, text_layouts, nullptr, kOverlayPane, false, L"message");
    const auto panels = CollectCmds<FillRoundedRectCmd>(cmds);
    ASSERT_EQ(panels.size(), 1u);
    EXPECT_FLOAT_EQ(panels[0].opacity, 0.7f);
    EXPECT_LT(panels[0].rect.bottom, NavBackButtonRect(kOverlayPane).top);
    EXPECT_FLOAT_EQ(panels[0].rect.bottom - panels[0].rect.top, TOAST_OVERLAY_HEIGHT);
}

// ═══════════════════════════════════════════════
// スクロールバーのつまみ
// ═══════════════════════════════════════════════

// 出すかどうかは ShowsVScrollbar に任せる (計測待ちの may_grow もそのまま渡す)。
TEST(ScrollThumbTest, VisibilityFollowsShowsVScrollbar)
{
    EXPECT_FALSE(VScrollThumb(kOverlayPane, 0.0f, 0.0f, kOverlayPane.height));
    EXPECT_TRUE(VScrollThumb(kOverlayPane, 0.0f, 0.0f, kOverlayPane.height, /*may_grow=*/true));
}

// つまみはヘッダの下から右端に寄せて置き、ドラッグ (ScrollFromThumbY) で同じスクロール位置に戻る。
TEST(ScrollThumbTest, TracksScrollWithinContentArea)
{
    constexpr float header = 30.0f;
    constexpr float total = 1480.0f;
    const PaneScrollInfo info = ComputeScrollInfo(kOverlayPane, header, total);
    const float right = kOverlayPane.x + kOverlayPane.width;

    const auto top = VScrollThumb(kOverlayPane, header, 0.0f, total);
    ASSERT_TRUE(top);
    EXPECT_EQ(top->brush_id, BrushId::ScrollbarThumb);
    EXPECT_FLOAT_EQ(top->rect.left, VScrollbarLeftX(right));
    EXPECT_FLOAT_EQ(top->rect.right - top->rect.left, PANE_SCROLLBAR_WIDTH);
    EXPECT_FLOAT_EQ(top->rect.top, kOverlayPane.y + header);
    EXPECT_FLOAT_EQ(top->rect.bottom - top->rect.top, info.thumb_height);

    const auto bottom = VScrollThumb(kOverlayPane, header, info.max_scroll, total);
    ASSERT_TRUE(bottom);
    EXPECT_FLOAT_EQ(bottom->rect.bottom, kOverlayPane.y + kOverlayPane.height);

    const float mid_scroll = info.max_scroll * 0.4f;
    const auto mid = VScrollThumb(kOverlayPane, header, mid_scroll, total);
    ASSERT_TRUE(mid);
    EXPECT_NEAR(ScrollFromThumbY(info, mid->rect.top), mid_scroll, 1e-2f);
}

// ═══════════════════════════════════════════════
// 検索バー (文字列は test_search_input.cpp)
// ═══════════════════════════════════════════════

namespace {

// rect と同じ位置に塗った角丸矩形のブラシ。無ければ nullopt。
std::optional<BrushId> FillBrushAt(const DrawCommandList& cmds, const D2D1_RECT_F& rect)
{
    const auto fill = FindFirst<FillRoundedRectCmd>(cmds, [&](const FillRoundedRectCmd& c) {
        return c.rect.left == rect.left && c.rect.top == rect.top && c.rect.right == rect.right && c.rect.bottom == rect.bottom;
    });
    return fill ? std::optional(fill->brush_id) : std::nullopt;
}

} // namespace

// バーはペインの下端に幅いっぱいに敷き、上端に境界線を引く。入力欄はフォーカス枠で描く。
TEST(OverlayCommandsTest, SearchBarDocksAtPaneBottom)
{
    TextLayoutCache text_layouts;
    DrawCommandList cmds;
    overlay::EmitSearchBar(cmds, text_layouts, nullptr, nullptr, kOverlayPane, { .visible = true }, {});
    const auto sbl = ComputeSearchBarLayout(kOverlayPane, false);

    const auto bar = FindFirst<FillRectCmd>(cmds);
    ASSERT_TRUE(bar);
    EXPECT_EQ(bar->brush_id, BrushId::SearchBarBg);
    EXPECT_FLOAT_EQ(bar->rect.left, kOverlayPane.x);
    EXPECT_FLOAT_EQ(bar->rect.right, kOverlayPane.x + kOverlayPane.width);
    EXPECT_FLOAT_EQ(bar->rect.bottom, kOverlayPane.y + kOverlayPane.height);
    EXPECT_FLOAT_EQ(bar->rect.bottom - bar->rect.top, SEARCH_BAR_HEIGHT);

    const auto border = FindFirst<DrawLineCmd>(cmds);
    ASSERT_TRUE(border);
    EXPECT_EQ(border->brush_id, BrushId::SearchBarBorder);
    EXPECT_FLOAT_EQ(border->p0.y, sbl.bar_top);

    EXPECT_EQ(FillBrushAt(cmds, sbl.input_rect), BrushId::SearchInputBg);
    const auto frame = FindFirst<DrawRoundedRectCmd>(cmds);
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->brush_id, BrushId::Link);
    EXPECT_FLOAT_EQ(frame->rect.left, sbl.input_rect.left);

    // 文字列が無ければ選択範囲も文字も積まない。
    EXPECT_EQ(CountCmd<DrawTextLayoutCmd>(cmds), 0);
    EXPECT_FALSE(FindFirst<FillRectCmd>(cmds, [](const FillRectCmd& c) { return c.brush_id == BrushId::Selection; }));
}

// 1 件も見つからなければ入力欄の背景を変える。
TEST(OverlayCommandsTest, SearchBarInputTurnsRedOnNoMatch)
{
    const auto input_rect = ComputeSearchBarLayout(kOverlayPane, true).input_rect;
    TextLayoutCache text_layouts;
    DrawCommandList cmds;
    overlay::EmitSearchBar(cmds, text_layouts, nullptr, nullptr, kOverlayPane, { .query = L"abc", .total_matches = 0, .visible = true }, {});
    EXPECT_EQ(FillBrushAt(cmds, input_rect), BrushId::SearchNoMatchBg);

    cmds.clear();
    overlay::EmitSearchBar(cmds, text_layouts, nullptr, nullptr, kOverlayPane, { .query = L"abc", .total_matches = 3, .visible = true }, {});
    EXPECT_EQ(FillBrushAt(cmds, input_rect), BrushId::SearchInputBg);
}

// キャレットは文字列の左端から caret_x の位置に引き、入力欄の右端を越えない。IME の合成中と非表示のときは引かない。
TEST(OverlayCommandsTest, SearchBarCaretFollowsMeasuredPosition)
{
    const auto sbl = ComputeSearchBarLayout(kOverlayPane, true);
    const auto caret_x = [&](const SearchBarRenderState& sb, float measured) -> std::optional<float> {
        TextLayoutCache text_layouts;
        DrawCommandList cmds;
        overlay::EmitSearchBar(cmds, text_layouts, nullptr, nullptr, kOverlayPane, sb, { .caret_x = measured });
        const auto lines = CollectCmds<DrawLineCmd>(cmds);
        if (lines.size() < 2) {
            return std::nullopt;
        }
        EXPECT_EQ(lines[1].brush_id, BrushId::SearchInputText);
        EXPECT_FLOAT_EQ(lines[1].p0.x, lines[1].p1.x);
        return lines[1].p0.x;
    };
    const SearchBarRenderState sb{ .query = L"abc", .total_matches = 1, .visible = true, .caret_visible = true };
    EXPECT_EQ(caret_x(sb, 20.0f), sbl.text_left() + 21.0f);
    EXPECT_EQ(caret_x(sb, 10000.0f), sbl.text_right());

    SearchBarRenderState composing = sb;
    composing.ime_composition = L"か";
    EXPECT_FALSE(caret_x(composing, 20.0f));
    SearchBarRenderState hidden = sb;
    hidden.caret_visible = false;
    EXPECT_FALSE(caret_x(hidden, 20.0f));
}

// ホバー中のボタンと、オンのトグル (大文字小文字・ハイライト) だけ背景を塗る。
TEST(OverlayCommandsTest, SearchBarButtonsShowHoverAndToggleState)
{
    const auto sbl = ComputeSearchBarLayout(kOverlayPane, true);
    TextLayoutCache text_layouts;
    DrawCommandList cmds;
    overlay::EmitSearchBar(cmds, text_layouts, nullptr, nullptr, kOverlayPane,
                           { .query = L"a", .total_matches = 1, .visible = true, .case_sensitive = true, .highlight_enabled = false, .hovered = SearchBarHitZone::Up }, {});
    EXPECT_EQ(FillBrushAt(cmds, sbl.up_btn), BrushId::TitleBarButtonHover);
    EXPECT_EQ(FillBrushAt(cmds, sbl.case_btn), BrushId::TitleBarButtonActive);
    EXPECT_EQ(FillBrushAt(cmds, sbl.down_btn), std::nullopt);
    EXPECT_EQ(FillBrushAt(cmds, sbl.highlight_btn), std::nullopt);
    EXPECT_EQ(FillBrushAt(cmds, sbl.close_btn), std::nullopt);

    cmds.clear();
    overlay::EmitSearchBar(cmds, text_layouts, nullptr, nullptr, kOverlayPane,
                           { .query = L"a", .total_matches = 1, .visible = true, .hovered = SearchBarHitZone::CaseSensitive }, {});
    EXPECT_EQ(FillBrushAt(cmds, sbl.case_btn), BrushId::TitleBarButtonHover);
    EXPECT_EQ(FillBrushAt(cmds, sbl.highlight_btn), BrushId::TitleBarButtonActive);
}

// ═══════════════════════════════════════════════
// タイトルバーとサイドペイン
// ═══════════════════════════════════════════════

namespace {

std::optional<BrushId> FillRectBrushAt(const DrawCommandList& cmds, const D2D1_RECT_F& rect)
{
    const auto fill = FindFirst<FillRectCmd>(cmds, [&](const FillRectCmd& c) {
        return c.rect.left == rect.left && c.rect.top == rect.top && c.rect.right == rect.right && c.rect.bottom == rect.bottom;
    });
    return fill ? std::optional(fill->brush_id) : std::nullopt;
}

// 一覧の行 (ペインの幅いっぱいで高さ item_h) を塗ったコマンドの上端と色。
std::vector<std::pair<float, BrushId>> RowFills(const DrawCommandList& cmds, const PaneRect& rect, float item_h)
{
    std::vector<std::pair<float, BrushId>> rows;
    for (const auto& c : CollectCmds<FillRectCmd>(cmds)) {
        if (c.rect.left == rect.x && c.rect.right == rect.x + rect.width && c.rect.bottom - c.rect.top == item_h) {
            rows.emplace_back(c.rect.top, c.brush_id);
        }
    }
    return rows;
}

constexpr PaneRect kSidePane{ 0.0f, 32.0f, 200.0f, 400.0f };
constexpr float kItemHeight = 20.0f;

} // namespace

// オンのトグルはホバーより優先して塗る。閉じるボタンのホバーだけ赤い。
TEST(ChromeCommandsTest, TitleBarButtonBackgrounds)
{
    TitleBar bar;
    bar.UpdateLayout(800.0f);
    TextLayoutCache text_layouts;
    DrawCommandList cmds;
    TitleBarRenderState tb{ .bar = bar, .window_width = 800.0f, .hovered_zone = TitleBarHitZone::Search, .search_active = true };
    chrome::EmitTitleBar(cmds, text_layouts, nullptr, nullptr, nullptr, false, tb);
    EXPECT_EQ(FillRectBrushAt(cmds, ToD2DRect(bar.GetButton(TitleBarHitZone::Search))), BrushId::TitleBarButtonActive);
    EXPECT_EQ(FillRectBrushAt(cmds, ToD2DRect(bar.GetButton(TitleBarHitZone::FileToggle))), std::nullopt);

    cmds.clear();
    tb.hovered_zone = TitleBarHitZone::FileToggle;
    chrome::EmitTitleBar(cmds, text_layouts, nullptr, nullptr, nullptr, false, tb);
    EXPECT_EQ(FillRectBrushAt(cmds, ToD2DRect(bar.GetButton(TitleBarHitZone::FileToggle))), BrushId::TitleBarButtonHover);

    cmds.clear();
    tb.hovered_zone = TitleBarHitZone::Close;
    chrome::EmitTitleBar(cmds, text_layouts, nullptr, nullptr, nullptr, false, tb);
    EXPECT_EQ(FillRectBrushAt(cmds, ToD2DRect(bar.GetButton(TitleBarHitZone::Close))), BrushId::TitleBarCloseRed);
}

// 見えている行だけを積む。現在のファイルとホバーも、見えている行の分だけ塗る。
TEST(ChromeCommandsTest, FilePaneEmitsOnlyVisibleRows)
{
    const Theme theme = GetLightTheme();
    std::pmr::vector<FileEntry> files(100);
    files[11].is_current = true;
    files[80].is_current = true;
    const std::pmr::vector<TocEntry> toc;
    const std::pmr::vector<Node> nodes;
    constexpr float kScroll = 200.0f;
    const SidePaneState sp{
        .panes = { SidePaneInstance{ .rect = kSidePane, .scroll = { kScroll }, .hovered_index = 12, .show = true, .item_height = kItemHeight }, {} },
        .file_entries = files,
        .toc_entries = toc,
        .nodes = nodes,
        .active_toc_index = -1,
    };
    TextLayoutCache text_layouts;
    DrawCommandList cmds;
    std::pmr::vector<std::pmr::wstring> toc_titles;
    chrome::EmitSidePanes(cmds, text_layouts, {}, theme, sp, toc_titles);

    const float content_top = PaneContentTop(kSidePane, theme.pane_header_height);
    const auto row_top = [&](int i) { return content_top + i * kItemHeight - kScroll; };
    EXPECT_EQ(RowFills(cmds, kSidePane, kItemHeight),
              (std::vector<std::pair<float, BrushId>>{ { row_top(11), BrushId::PaneItemActive }, { row_top(12), BrushId::PaneItemHover } }));
}

// 無効な「現在のファイルを表示」ボタンは、ホバーが残っていても塗らない。
TEST(ChromeCommandsTest, DisabledPaneButtonIgnoresHover)
{
    const Theme theme = GetLightTheme();
    const std::pmr::vector<FileEntry> files;
    const std::pmr::vector<TocEntry> toc;
    const std::pmr::vector<Node> nodes;
    const auto reveal = OffsetRectF(PaneRevealButtonRect(kSidePane.width, theme.pane_header_height), kSidePane.x, kSidePane.y);
    for (const bool enabled : { false, true }) {
        const SidePaneState sp{
            .panes = { SidePaneInstance{ .rect = kSidePane, .show = true, .hovered_button = PaneHeaderButton::Reveal, .reveal_enabled = enabled, .item_height = kItemHeight }, {} },
            .file_entries = files,
            .toc_entries = toc,
            .nodes = nodes,
            .active_toc_index = -1,
        };
        TextLayoutCache text_layouts;
        DrawCommandList cmds;
        std::pmr::vector<std::pmr::wstring> toc_titles;
        chrome::EmitSidePanes(cmds, text_layouts, {}, theme, sp, toc_titles);
        EXPECT_EQ(FillRectBrushAt(cmds, reveal), enabled ? std::optional(BrushId::PaneItemHover) : std::nullopt) << enabled;
    }
}

// 目次は見出しの段で字下げし、アクティブな項目はホバーより優先して塗って下線を引く。見出しの文字は描いた項目だけ UTF-16 にする。
TEST(ChromeCommandsTest, TocIndentsByLevelAndUnderlinesActive)
{
    const Theme theme = GetLightTheme();
    const auto nodes = ParseNodes("# A\n\n## B\n\n### C");
    const std::pmr::vector<FileEntry> files;
    const std::pmr::vector<TocEntry> toc{ { 0 }, { 1 }, { 2 } };
    constexpr float kZoom = 1.5f;
    const SidePaneState sp{
        .panes = { {}, SidePaneInstance{ .rect = kSidePane, .hovered_index = 2, .show = true, .zoom = kZoom, .item_height = kItemHeight } },
        .file_entries = files,
        .toc_entries = toc,
        .nodes = nodes,
        .active_toc_index = 2,
    };
    TextLayoutCache text_layouts;
    DrawCommandList cmds;
    std::pmr::vector<std::pmr::wstring> toc_titles;
    chrome::EmitSidePanes(cmds, text_layouts, {}, theme, sp, toc_titles);

    const float content_top = PaneContentTop(kSidePane, theme.pane_header_height);
    EXPECT_EQ(RowFills(cmds, kSidePane, kItemHeight), (std::vector<std::pair<float, BrushId>>{ { content_top + 2 * kItemHeight, BrushId::PaneItemActive } }));
    const auto underline = FindFirst<DrawLineCmd>(cmds);
    ASSERT_TRUE(underline);
    EXPECT_FLOAT_EQ(underline->p0.x, kSidePane.x + (8.0f + 2 * TOC_INDENT_PER_LEVEL) * kZoom);
    EXPECT_FLOAT_EQ(underline->p0.y, content_top + 3 * kItemHeight - 1.0f);
    ASSERT_EQ(toc_titles.size(), 3u);
    EXPECT_EQ(toc_titles[0], L"A");
    EXPECT_EQ(toc_titles[2], L"C");
}
