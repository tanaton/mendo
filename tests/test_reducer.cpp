#include <gtest/gtest.h>
#include "app_constants.h"

#include "reducer.h"
#include "document.h"
#include "app_state.h"
#include "theme.h"
#include "reducer_harness.h"
#include "test_helpers.h"
#include "document_utils.h"
#include "file_io.h"
#include "layout_computer.h"
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory_resource>
#include <optional>
#include <utility>

namespace {

// ツールチップを隠させたか (空の SetTooltip)。
bool HidesTooltip(const SideEffectList& effects)
{
    const auto* set = FindEffect<effect::SetTooltip>(effects);
    return set && set->text.empty();
}

constexpr TooltipTarget kLinkTooltip{ TooltipTarget::Zone::MdLink, 1 };

// 文書を差し替え、各ノードを node_height ずつ縦に並べたレイアウトを持たせる。
void SetDocument(AppState& state, std::string_view md, std::wstring_view path, float node_height = 100.0f)
{
    state.document.doc = Document::FromMarkdown(std::string(md), path);
    state.document.layout_cache = MakeUniformCache(static_cast<int>(state.document.doc.GetNodes().size()), node_height);
}

} // namespace

class ReducerTest : public ::testing::Test {
protected:
    AppState state;
    // value-init ({}) で全 POD フィールドを 0 にする。Theme は zoom 以外に default member
    // initializer を持たない集約のため、`Theme theme;` (default-init) だと heading_spacing_above
    // 等が未初期化のまま残り、テスト順序により MakeHeadingTopTarget の offset 計算が非決定的になる
    // (TocItemClicked_TailSection の flaky 原因)。本番では ThemeService が全フィールドを設定する。
    Theme theme{};

    void SetUp() override
    {
        // PaneLayout のモック（ページサイズ用）
        PaneLayout pl{};
        pl.md_rect.height = 500.0f;
        state.pane_layout_cache.Set(pl);
        InitReducerState(state, theme);
        // 内容高 1000・表示高 500 で、500 までスクロールできる。パスは空 (未ロード扱い) にする。
        SetDocument(state, "Hello", L"", 1000.0f);
    }

    // MD スクロールバー系テスト用: reducer が ComputeTotalContentHeight で導出する
    // 総コンテンツ高が total になるよう doc と layout_cache を構築する (margin_bottom=0 前提)。
    void SetupMdContentHeight(float total)
    {
        SetDocument(state, "Hello", L"test.md", total);
    }
};

// ---- KeyScrollAction テスト ----

TEST_F(ReducerTest, KeyScrollLineDown)
{
    const float old_scroll = state.view.viewport.GetScrollY();
    auto effects = Reduce(state, KeyScrollAction{ ScrollType::LineDown });

    EXPECT_GT(state.view.viewport.GetScrollY(), old_scroll);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
    EXPECT_TRUE(HasEffect<effect::BitmapManage>(effects));
}

TEST_F(ReducerTest, KeyScrollLineUp_AtTop_NoEffect)
{
    // スクロール位置0で上にスクロール → 変化なし → 副作用なし
    state.view.viewport.ScrollTo(0.0f, MdMaxScroll(state));
    auto effects = Reduce(state, KeyScrollAction{ ScrollType::LineUp });

    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), 0.0f);
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, KeyScrollPageDown)
{
    const float old_scroll = state.view.viewport.GetScrollY();
    auto effects = Reduce(state, KeyScrollAction{ ScrollType::PageDown });

    // ページスクロールはラインスクロールより大きい
    EXPECT_GT(state.view.viewport.GetScrollY() - old_scroll, 40.0f);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, KeyScrollHome)
{
    state.view.viewport.ScrollTo(500.0f, MdMaxScroll(state));
    auto effects = Reduce(state, KeyScrollAction{ ScrollType::Home });

    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), 0.0f);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, KeyScrollEnd)
{
    auto effects = Reduce(state, KeyScrollAction{ ScrollType::End });

    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), MdMaxScroll(state));
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

// App::Dispatch はスタックの領域を渡し、副作用の一覧をそこから取らせる (上流は使わない大きさに収まる)。
TEST_F(ReducerTest, Reduce_TakesListFromGivenResource)
{
    alignas(std::max_align_t) std::array<std::byte, 1024> buffer;
    std::pmr::monotonic_buffer_resource arena(buffer.data(), buffer.size(), std::pmr::null_memory_resource());

    const auto effects = Reduce(state, KeyScrollAction{ ScrollType::LineDown }, &arena);

    ASSERT_FALSE(effects.empty());
    EXPECT_EQ(effects.get_allocator().resource(), &arena);
}

// ---- MouseWheelAction テスト ----

constexpr float kWheelNotchScroll = WHEEL_DELTA * MOUSE_WHEEL_SCROLL_MULTIPLIER;

// 下へ回すと (delta < 0) 進む。スプリッタの上は本文をスクロールする。
TEST_F(ReducerTest, MouseWheel_ScrollsMdPane)
{
    auto effects = Reduce(state, MouseWheelAction{ .delta = -WHEEL_DELTA });

    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), kWheelNotchScroll);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
    EXPECT_TRUE(HasEffect<effect::BitmapManage>(effects));

    Reduce(state, MouseWheelAction{ .delta = WHEEL_DELTA, .zone = PaneZone::Splitter1 });
    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), 0.0f);
}

TEST_F(ReducerTest, MouseWheel_ClampedAtMax)
{
    auto effects = Reduce(state, MouseWheelAction{ .delta = -WHEEL_DELTA * 1000 });

    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), MdMaxScroll(state));
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

// サイドペインの上ではそのペインだけをスクロールする。
TEST(ReducerWheelTest, ScrollsSidePaneUnderPointer)
{
    ReducerHarness h({ .markdown = MakeHarnessMarkdown(30) });

    auto effects = h.Dispatch(MouseWheelAction{ .delta = -WHEEL_DELTA, .zone = PaneZone::TocPane });

    EXPECT_FLOAT_EQ(h.state.view.panes.SidePaneScroll(PaneTarget::Toc).scroll_y, kWheelNotchScroll);
    EXPECT_FLOAT_EQ(h.state.view.panes.SidePaneScroll(PaneTarget::File).scroll_y, 0.0f);
    EXPECT_FLOAT_EQ(h.state.view.viewport.GetScrollY(), 0.0f);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

// Ctrl+ホイールはポインタの下の領域だけをズームする。スプリッタの上は本文。
TEST_F(ReducerTest, CtrlWheel_ZoomsPaneUnderPointer)
{
    const float md_before = state.view.viewport.Zoom().Value();
    const float file_before = state.view.panes.SidePaneZoom(PaneTarget::File).Value();

    auto effects = Reduce(state, MouseWheelAction{ .delta = WHEEL_DELTA, .ctrl = true, .zone = PaneZone::FilePane });
    EXPECT_GT(state.view.panes.SidePaneZoom(PaneTarget::File).Value(), file_before);
    EXPECT_FLOAT_EQ(state.view.viewport.Zoom().Value(), md_before);
    EXPECT_FALSE(HasEffect<effect::ApplyZoom>(effects));

    effects = Reduce(state, MouseWheelAction{ .delta = -WHEEL_DELTA, .ctrl = true, .zone = PaneZone::Splitter1 });
    EXPECT_LT(state.view.viewport.Zoom().Value(), md_before);
    EXPECT_TRUE(HasEffect<effect::ApplyZoom>(effects));
}

// ピンチや高分解能ホイールの細かい delta は 1 ノッチ分たまるごとに 1 段だけズームする。
TEST_F(ReducerTest, CtrlWheel_ZoomsOncePerNotch)
{
    const auto wheel = [this](int delta) { return Reduce(state, MouseWheelAction{ .delta = delta, .ctrl = true }); };
    const auto& zoom = state.view.viewport.Zoom();
    ZoomLevel expected = zoom;

    EXPECT_FALSE(HasEffect<effect::ApplyZoom>(wheel(40)));
    EXPECT_FALSE(HasEffect<effect::ApplyZoom>(wheel(40)));
    EXPECT_TRUE(HasEffect<effect::ApplyZoom>(wheel(40)));
    expected.Step(ZoomDirection::In);
    EXPECT_EQ(zoom.Index(), expected.Index());
    EXPECT_EQ(state.interaction.zoom_wheel_accum, 0);

    wheel(-WHEEL_DELTA);
    expected.Step(ZoomDirection::Out);
    EXPECT_EQ(zoom.Index(), expected.Index());

    // まとめて届いたノッチは段数として 1 回で組み直す。
    const auto effects = wheel(2 * WHEEL_DELTA);
    expected.Step(ZoomDirection::In);
    expected.Step(ZoomDirection::In);
    EXPECT_EQ(zoom.Index(), expected.Index());
    EXPECT_EQ(std::ranges::count_if(effects, [](const SideEffect& e) static { return std::holds_alternative<effect::ApplyZoom>(e); }), 1);
}

// 向きが反転したら逆向きの端数は持ち越さない。
TEST_F(ReducerTest, CtrlWheel_DropsRemainderOnReverse)
{
    const auto wheel = [this](int delta) { return Reduce(state, MouseWheelAction{ .delta = delta, .ctrl = true }); };
    const int before = state.view.viewport.Zoom().Index();

    EXPECT_FALSE(HasEffect<effect::ApplyZoom>(wheel(100)));
    EXPECT_FALSE(HasEffect<effect::ApplyZoom>(wheel(-100)));
    EXPECT_EQ(state.interaction.zoom_wheel_accum, -100);
    EXPECT_TRUE(HasEffect<effect::ApplyZoom>(wheel(-20)));
    EXPECT_EQ(state.view.viewport.Zoom().Index(), before - 1);
}

// ---- SelectAllAction テスト ----

TEST_F(ReducerTest, SelectAll_WithNodes)
{
    state.document.doc = Document::FromMarkdown(std::string("Hello world\n\nSecond paragraph"), L"test.md");

    auto effects = Reduce(state, SelectAllAction{});

    EXPECT_TRUE(state.view.viewport.GetSelection().active());
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

// ---- SelectWordAction テスト ----

TEST_F(ReducerTest, SelectWord_SelectsWordAndSetsAnchor)
{
    state.document.doc = Document::FromMarkdown(std::string("Hello world"), L"test.md");

    auto effects = Reduce(state, SelectWordAction{ 0, 7 });

    const auto& sel = state.view.viewport.GetSelection();
    ASSERT_TRUE(sel.active());
    EXPECT_EQ(sel.start_node, 0);
    EXPECT_EQ(sel.start_pos, 6u);
    EXPECT_EQ(sel.end_pos, 11u);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, SelectWord_InvalidNode_NoOp)
{
    state.document.doc = Document::FromMarkdown(std::string("Hello"), L"test.md");

    auto effects = Reduce(state, SelectWordAction{ -1, 0 });

    EXPECT_FALSE(state.view.viewport.GetSelection().active());
    EXPECT_TRUE(effects.empty());
}

// ---- ClearSelectionAction テスト ----

TEST_F(ReducerTest, ClearSelection_WhenNotVisible)
{
    // 検索バーが非表示の場合、選択をクリアする
    state.view.viewport.SetSelection(TextSelection::MakeOrdered(0, 0, 0, 3));
    auto effects = Reduce(state, ClearSelectionAction{});

    EXPECT_FALSE(state.view.viewport.GetSelection().active());
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, ClearSelection_NothingToClearSkipsRedraw)
{
    auto effects = Reduce(state, ClearSelectionAction{});

    EXPECT_FALSE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, ClearSelection_ClosesSearchBar)
{
    state.search.search_state.Show();

    auto effects = Reduce(state, ClearSelectionAction{});

    // 検索バーが閉じられる
    EXPECT_FALSE(state.search.search_state.IsVisible());
    EXPECT_TRUE(HasEffect<effect::SearchUnfocus>(effects));
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

// 検索バーの開閉はフォーカス移動と再描画を effect で返す。
TEST_F(ReducerTest, OpenAndCloseSearchBar_EmitFocusEffects)
{
    auto opened = Reduce(state, OpenSearchBarAction{});
    EXPECT_TRUE(state.search.search_state.IsVisible());
    EXPECT_TRUE(HasEffect<effect::SearchFocus>(opened));
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(opened));

    auto closed = Reduce(state, CloseSearchBarAction{});
    EXPECT_FALSE(state.search.search_state.IsVisible());
    EXPECT_TRUE(HasEffect<effect::SearchUnfocus>(closed));
}

// ---- 検索ジャンプ ----

// 検索ジャンプのスクロールも通常スクロールと同じ副作用 (ツールチップ消去・ビットマップ管理・TOC 追従) を積む。
TEST_F(ReducerTest, SearchJumpEmitsScrollEffects)
{
    SetDocument(state, "a\n\ntarget", L"test.md", 1000.0f);
    state.search.search_state.Show();
    state.interaction.tooltip = kLinkTooltip;

    auto effects = Reduce(state, SearchTextChangedAction{ std::wstring(L"target") });

    EXPECT_GT(state.view.viewport.GetScrollY(), 0.0f);
    EXPECT_TRUE(HidesTooltip(effects));
    EXPECT_TRUE(state.interaction.tooltip.IsEmpty());
    EXPECT_TRUE(HasEffect<effect::BitmapManage>(effects));
}

TEST_F(ReducerTest, SearchWithoutScrollEmitsNoScrollEffects)
{
    SetDocument(state, "target", L"test.md", 10.0f);
    state.search.search_state.Show();

    auto effects = Reduce(state, SearchTextChangedAction{ std::wstring(L"target") });

    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), 0.0f);
    EXPECT_FALSE(HasEffect<effect::BitmapManage>(effects));
}

// ---- NoOpAction テスト ----

TEST_F(ReducerTest, NoOp_NoStateChange)
{
    const float scroll = state.view.viewport.GetScrollY();
    auto effects = Reduce(state, NoOpAction{});

    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), scroll);
    EXPECT_TRUE(effects.empty());
}

// ---- ActivateAction テスト ----

TEST_F(ReducerTest, Activate_ChangesWindowActive)
{
    state.window.window_active = true;
    state.interaction.tooltip = kLinkTooltip;
    auto effects = Reduce(state, ActivateAction{ false });

    EXPECT_FALSE(state.window.window_active);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
    EXPECT_TRUE(HidesTooltip(effects));
}

TEST_F(ReducerTest, Activate_NoChangeWhenSame)
{
    state.window.window_active = true;
    auto effects = Reduce(state, ActivateAction{ true });

    // 状態変化なし → 再描画なし
    EXPECT_FALSE(HasEffect<effect::InvalidateWindow>(effects));
}

// ---- EnterSizeMoveAction テスト ----

TEST_F(ReducerTest, EnterSizeMove_SetsFlag)
{
    state.window.is_sizing = false;
    Reduce(state, EnterSizeMoveAction{});

    EXPECT_TRUE(state.window.is_sizing);
}

TEST_F(ReducerTest, ExitSizeMove_MoveOnly_SkipsSettleViewport)
{
    Reduce(state, EnterSizeMoveAction{});
    auto effects = Reduce(state, ExitSizeMoveAction{});

    EXPECT_FALSE(state.window.is_sizing);
    EXPECT_FALSE(HasEffect<effect::SettleViewport>(effects));
}

TEST_F(ReducerTest, ExitSizeMove_AfterResize_SettlesViewport)
{
    Reduce(state, EnterSizeMoveAction{});
    Reduce(state, ResizeAction{ 800, 600 });
    auto effects = Reduce(state, ExitSizeMoveAction{});

    EXPECT_TRUE(HasEffect<effect::SettleViewport>(effects));
}

// ライブリサイズ中は描画面を大きめに使い回し、終わったら実際の大きさに合わせ直させる。
TEST_F(ReducerTest, ExitSizeMove_AfterResize_FitsRendererToClient)
{
    Reduce(state, EnterSizeMoveAction{});
    const auto during = Reduce(state, ResizeAction{ 800, 600 });
    const auto* live = FindEffect<effect::RendererResize>(during);
    ASSERT_NE(live, nullptr);
    EXPECT_TRUE(live->live);

    const auto effects = Reduce(state, ExitSizeMoveAction{});
    const auto* fit = FindEffect<effect::RendererResize>(effects);
    ASSERT_NE(fit, nullptr);
    EXPECT_FALSE(fit->live);
    EXPECT_EQ(fit->width, 800u);
    EXPECT_EQ(fit->height, 600u);
    EXPECT_TRUE((HasEffectInOrder<effect::RendererResize, effect::SettleViewport>(effects)));
}

TEST_F(ReducerTest, ExitSizeMove_AfterDpiChange_SettlesViewport)
{
    Reduce(state, EnterSizeMoveAction{});
    Reduce(state, DpiChangedAction{ 144, RECT{ 0, 0, 800, 600 } });
    auto effects = Reduce(state, ExitSizeMoveAction{});

    EXPECT_TRUE(HasEffect<effect::SettleViewport>(effects));
}

TEST_F(ReducerTest, TogglePane_SettlesViewport)
{
    auto effects = Reduce(state, TogglePaneAction{ PaneTarget::Toc });

    EXPECT_TRUE(HasEffect<effect::SettleViewport>(effects));
}

// ---- MouseLeaveAction テスト ----

TEST_F(ReducerTest, MouseLeave_ClearsTooltip)
{
    state.interaction.tooltip = kLinkTooltip;
    auto effects = Reduce(state, MouseLeaveAction{});
    EXPECT_TRUE(HidesTooltip(effects));
    EXPECT_TRUE(state.interaction.tooltip.IsEmpty());
}

// 対象が無ければツールチップは隠れているので、スクロールのたびに消させない。
TEST_F(ReducerTest, MouseLeave_NoTooltip_NoEffect)
{
    EXPECT_TRUE(Reduce(state, MouseLeaveAction{}).empty());
}

TEST_F(ReducerTest, MouseLeave_ClearsHoverHighlights)
{
    Reduce(state, PointerHoverAction{ PointerHover{ { .titlebar = TitleBarHitZone::Help, .h_block = 4 } } });
    auto effects = Reduce(state, MouseLeaveAction{});
    EXPECT_EQ(state.view.hover.titlebar, TitleBarHitZone::None);
    EXPECT_EQ(state.view.hover.h_block, -1);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

// ---- PointerHoverAction テスト ----

// 状態には対象だけを持ち、文言は副作用で App (Tooltip) に渡す。
TEST_F(ReducerTest, PointerHover_NewTooltipEmitsSetTooltipWithText)
{
    const TooltipHover hover{ kLinkTooltip, L"https://example.com" };
    auto effects = Reduce(state, PointerHoverAction{ PointerHover{ {}, hover } });

    ASSERT_EQ(effects.size(), 1u);
    const auto* set = GetEffect<effect::SetTooltip>(effects[0]);
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(set->text, hover.text);
    EXPECT_EQ(state.interaction.tooltip, kLinkTooltip);

    // 同じ対象の上を動いている間は出し直さない (文言は作られずに空で来る)。
    EXPECT_TRUE(Reduce(state, PointerHoverAction{ PointerHover{ {}, { kLinkTooltip } } }).empty());
}

TEST_F(ReducerTest, PointerHover_NothingChanged_NoEffect)
{
    // 初期状態はどこにもホバーしていないので、空のホバーは no-op
    auto effects = Reduce(state, PointerHoverAction{});
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, PointerHover_EmptyTooltipAfterPrevTarget_HidesTooltip)
{
    // 既にターゲットが設定済みなら、空ターゲットへの遷移で隠させる（App::Execute でタイマー停止）
    state.interaction.tooltip = kLinkTooltip;

    auto effects = Reduce(state, PointerHoverAction{});
    ASSERT_EQ(effects.size(), 1u);
    EXPECT_TRUE(HidesTooltip(effects));
    EXPECT_TRUE(state.interaction.tooltip.IsEmpty());
}

// ホバーはマウス位置の全体を表すので、別ゾーンへ移ると前のゾーンのハイライトは消える。
TEST_F(ReducerTest, PointerHover_MovingToAnotherZoneClearsPreviousHighlights)
{
    Reduce(state, PointerHoverAction{ PointerHover{ { .titlebar = TitleBarHitZone::Close } } });
    Reduce(state, PointerHoverAction{ PointerHover{ { .side_index = { 2, -1 } } } });
    EXPECT_EQ(state.view.hover.titlebar, TitleBarHitZone::None);
    EXPECT_EQ(state.view.hover.side_index[0], 2);

    auto effects = Reduce(state, PointerHoverAction{ PointerHover{ { .nav = NavDirection::Back } } });
    EXPECT_EQ(state.view.hover.side_index[0], -1);
    EXPECT_EQ(state.view.hover.nav, NavDirection::Back);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, PointerHover_SearchBarInputHasNoHighlight)
{
    auto effects = Reduce(state, PointerHoverAction{ PointerHover{ { .search_bar = SearchBarHitZone::Input } } });
    EXPECT_EQ(state.view.hover.search_bar, SearchBarHitZone::None);
    EXPECT_TRUE(effects.empty());
}

// ---- コマンド系アクションテスト ----

TEST_F(ReducerTest, ReloadFileAction_EmitsReloadEffect)
{
    auto effects = Reduce(state, ReloadFileAction{});
    EXPECT_EQ(effects.size(), 1u);
    EXPECT_TRUE(HasEffect<effect::ReloadFile>(effects));
}

TEST_F(ReducerTest, OpenFileAction_EmitsOpenFileDialog)
{
    auto effects = Reduce(state, OpenFileAction{});
    EXPECT_EQ(effects.size(), 1u);
    EXPECT_TRUE(HasEffect<effect::OpenFileDialog>(effects));
}

// ---- ブロック単位の横スクロール ----

TEST_F(ReducerTest, BlockHHoverChanged_UpdatesAndInvalidates)
{
    auto effects = Reduce(state, PointerHoverAction{ PointerHover{ { .h_block = 5 } } });
    EXPECT_EQ(state.view.hover.h_block, 5);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, BlockHHoverChanged_SameValue_NoEffect)
{
    state.view.hover.h_block = 5;
    auto effects = Reduce(state, PointerHoverAction{ PointerHover{ { .h_block = 5 } } });
    EXPECT_TRUE(effects.empty());
}

// 隠したペインを再び表示したとき、隠す前のホバー強調が残らない。
TEST_F(ReducerTest, TogglePane_ClearsThatPaneHover)
{
    Reduce(state, PointerHoverAction{ PointerHover{ { .side_index = { 3, 5 }, .side_button = { PaneHeaderButton::Refresh, PaneHeaderButton::Close } } } });
    Reduce(state, TogglePaneAction{ PaneTarget::File });
    EXPECT_EQ(state.view.hover.side_index[0], -1);
    EXPECT_EQ(state.view.hover.side_button[0], PaneHeaderButton::None);
    EXPECT_EQ(state.view.hover.side_index[1], 5);
    EXPECT_EQ(state.view.hover.side_button[1], PaneHeaderButton::Close);
}

// ロード・リロード・ヘルプ表示のどれもこのリセットを通る。旧文書の横スクロール量が
// 残ると、同じ index の段落でヒットテストだけがずれる。
TEST_F(ReducerTest, DocumentReloaded_ClearsNodeBoundState)
{
    state.view.block_scroll_x[3] = 100.0f;
    state.view.block_scroll_x[7] = 250.0f;
    state.view.hover.h_block = 3;
    state.view.hover.side_index[std::to_underlying(PaneTarget::Toc)] = 4;
    state.view.active_toc_index = 2;
    state.view.viewport.SetSelection(TextSelection{ .start_node = 1, .end_node = 2, .end_pos = 3 });
    state.interaction.tooltip = kLinkTooltip;

    const auto effects = Reduce(state, DocumentReloadedAction{});

    EXPECT_TRUE(state.view.block_scroll_x.empty());
    EXPECT_EQ(state.view.hover, HoverHighlights{});
    EXPECT_EQ(state.view.active_toc_index, -1);
    EXPECT_FALSE(state.view.viewport.GetSelection().active());
    // ツールチップの対象も index で決まるので忘れ、出ていれば隠させる。
    EXPECT_TRUE(state.interaction.tooltip.IsEmpty());
    EXPECT_TRUE(HidesTooltip(effects));
}

// 文書を対象にするドラッグはキャプチャごと終え、待たせていたリロードは旧文書のものなので予約しない。
TEST_F(ReducerTest, DocumentReloaded_EndsDocumentBoundDrag)
{
    state.view.drag = { .kind = LeftDrag::TextSelect, .select_node = 1 };
    state.view.reload_after_drag = true;

    const auto effects = Reduce(state, DocumentReloadedAction{});

    EXPECT_EQ(state.view.drag.kind, LeftDrag::None);
    EXPECT_FALSE(state.view.reload_after_drag);
    EXPECT_TRUE(HasEffect<effect::ReleaseCapture>(effects));
    EXPECT_FALSE(HasEffect<effect::SetTimer>(effects));
}

// スプリッタのドラッグは文書と無関係なので続ける。
TEST_F(ReducerTest, DocumentReloaded_KeepsSplitterDrag)
{
    state.view.drag = { .kind = LeftDrag::Splitter, .pane = PaneTarget::Toc };

    const auto effects = Reduce(state, DocumentReloadedAction{});

    EXPECT_EQ(state.view.drag.kind, LeftDrag::Splitter);
    EXPECT_FALSE(HasEffect<effect::ReleaseCapture>(effects));
}

// 別の文書を開いたら、番号に結び付いた状態に加えてサイドペインのスクロールも先頭へ戻す。
TEST_F(ReducerTest, DocumentOpened_ResetsViewForNewDocument)
{
    state.document.doc = Document::FromMarkdown(std::string("# A"), L"C:\\docs\\a.md");
    state.view.hover.h_block = 3;
    state.view.panes.SidePaneScroll(PaneTarget::Toc).scroll_y = 100.0f;
    state.view.panes.SidePaneScroll(PaneTarget::File).scroll_y = 50.0f;

    Reduce(state, DocumentOpenedAction{});

    EXPECT_EQ(state.view.hover, HoverHighlights{});
    EXPECT_FLOAT_EQ(state.view.panes.SidePaneScroll(PaneTarget::Toc).scroll_y, 0.0f);
    EXPECT_FLOAT_EQ(state.view.panes.SidePaneScroll(PaneTarget::File).scroll_y, 0.0f);
}

TEST_F(ReducerTest, BlockHScrollDragEnded_ReleasesCapture)
{
    state.view.drag = { .kind = LeftDrag::BlockHScroll, .block_node = 4 };
    auto effects = Reduce(state, LeftDragEndedAction{});
    EXPECT_EQ(state.view.HDragNode(), -1);
    EXPECT_TRUE(HasEffect<effect::ReleaseCapture>(effects));
}

// テキスト選択の終了は TextSelectionEndedAction が担う。
TEST_F(ReducerTest, LeftDragEnded_NoLeftDragOrTextSelect_NoEffect)
{
    for (const LeftDrag kind : { LeftDrag::None, LeftDrag::TextSelect }) {
        SCOPED_TRACE(static_cast<int>(kind));
        state.view.drag = { .kind = kind };
        auto effects = Reduce(state, LeftDragEndedAction{});
        EXPECT_EQ(state.view.drag.kind, kind);
        EXPECT_TRUE(effects.empty());
    }
}

// ---- ナビゲーションテスト ----

TEST_F(ReducerTest, NavigateBack_NoHistory_NoEffects)
{
    auto effects = Reduce(state, NavigateBackAction{});
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, NavigateBack_DifferentFile_EmitsLoadFile)
{
    state.document.doc = Document::FromMarkdown(std::string("test"), L"C:\\current.md");
    state.view.nav_history.Push(NavEntry{ L"C:\\prev.md", 0, 50.0f });

    auto effects = Reduce(state, NavigateBackAction{});
    EXPECT_TRUE(HasEffect<effect::LoadFile>(effects));
}

TEST_F(ReducerTest, NavigateBack_SameFile_ScrollsAndInvalidates)
{
    // ノード 0 に対してオフセット 50 → y_position(0) + 50 = 50.0f
    SetDocument(state, "test", L"C:\\file.md", 1000.0f);
    state.view.nav_history.Push(NavEntry{ L"C:\\file.md", 0, 50.0f });
    state.view.viewport.ScrollTo(200.0f, MdMaxScroll(state));

    auto effects = Reduce(state, NavigateBackAction{});
    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), 50.0f);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
    EXPECT_TRUE(HasEffect<effect::BitmapManage>(effects));
}

// 戻る (別ファイルをロード中) → 進むで表示中の文書へ戻ったら、ロード中のファイルを取り消させる。
TEST_F(ReducerTest, NavigateBackThenForwardWhileLoading_CancelsOtherFileLoad)
{
    SetDocument(state, "test", L"C:\\current.md");
    state.view.nav_history.Push(NavEntry{ L"C:\\prev.md", 0, 0.0f });

    auto back = Reduce(state, NavigateBackAction{});
    ASSERT_TRUE(HasEffect<effect::LoadFile>(back));
    EXPECT_FALSE(HasEffect<effect::CancelOtherFileLoad>(back));

    auto forward = Reduce(state, NavigateForwardAction{});
    EXPECT_FALSE(HasEffect<effect::LoadFile>(forward));
    EXPECT_TRUE(HasEffect<effect::CancelOtherFileLoad>(forward));
    EXPECT_FALSE(state.view.pending_restore.IsValid());
    EXPECT_TRUE(state.view.nav_history.CanGoBack());
    EXPECT_FALSE(state.view.nav_history.CanGoForward());
}

TEST_F(ReducerTest, NavigateForward_NoHistory_NoEffects)
{
    auto effects = Reduce(state, NavigateForwardAction{});
    EXPECT_TRUE(effects.empty());
}

// ---- OpenPath / ShowHelp テスト ----

TEST_F(ReducerTest, OpenPath_PushesHistoryAndLoads)
{
    state.document.doc = Document::FromMarkdown(std::string("test"), L"C:\\current.md");

    auto effects = Reduce(state, OpenPathAction{ std::wstring(L"C:\\dropped.md") });
    EXPECT_TRUE(HasEffect<effect::LoadFile>(effects));
    EXPECT_TRUE(state.view.nav_history.CanGoBack());
}

TEST_F(ReducerTest, OpenPath_EmptyDoc_NoPush)
{
    // ドキュメントが空ならナビゲーション履歴にプッシュしない
    auto effects = Reduce(state, OpenPathAction{ std::wstring(L"C:\\dropped.md") });
    EXPECT_TRUE(HasEffect<effect::LoadFile>(effects));
    EXPECT_FALSE(state.view.nav_history.CanGoBack());
}

TEST_F(ReducerTest, ShowHelp_EmitsLoadFile)
{
    state.document.doc = Document::FromMarkdown(std::string("test"), L"C:\\file.md");

    auto effects = Reduce(state, ShowHelpAction{});
    EXPECT_TRUE(HasEffect<effect::LoadFile>(effects));
    EXPECT_TRUE(state.view.nav_history.CanGoBack());
}

// ---- リサイズ / DPI テスト ----

TEST_F(ReducerTest, Resize_ZeroSize_NoEffects)
{
    auto effects = Reduce(state, ResizeAction{ 0, 0 });
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, Resize_Normal_SettlesViewport)
{
    state.window.cached_dpi_scale = 1.0f;
    auto effects = Reduce(state, ResizeAction{ 800, 600 });

    EXPECT_TRUE(HasEffect<effect::RendererResize>(effects));
    EXPECT_TRUE(HasEffect<effect::SettleViewport>(effects));
    EXPECT_FALSE(state.pane_layout_cache.IsValid());
}

TEST_F(ReducerTest, Resize_Sizing_EmitsSizingUpdate)
{
    state.window.is_sizing = true;
    state.window.cached_dpi_scale = 1.0f;
    auto effects = Reduce(state, ResizeAction{ 800, 600 });

    EXPECT_TRUE(HasEffect<effect::RendererResize>(effects));
    EXPECT_TRUE(HasEffect<effect::PerformSizingUpdate>(effects));
    EXPECT_FALSE(HasEffect<effect::SettleViewport>(effects));
}

// 最小化からの復元などで同じ大きさの WM_SIZE が来ても、描画面とレイアウトは作り直さない。
TEST_F(ReducerTest, Resize_SameSize_NoEffects)
{
    Reduce(state, ResizeAction{ 800, 600 });
    auto effects = Reduce(state, ResizeAction{ 800, 600 });
    EXPECT_TRUE(effects.empty());
}

// DPI が変わった直後は、ピクセルサイズが同じでも DIP サイズが変わるので処理する。
TEST_F(ReducerTest, Resize_SameSizeAfterDpiChange_EmitsResize)
{
    Reduce(state, ResizeAction{ 800, 600 });
    Reduce(state, DpiChangedAction{ 192, RECT{ 0, 0, 800, 600 } });
    auto effects = Reduce(state, ResizeAction{ 800, 600 });
    EXPECT_TRUE(HasEffect<effect::RendererResize>(effects));
}

TEST_F(ReducerTest, DpiChanged_UpdatesScale)
{
    auto effects = Reduce(state, DpiChangedAction{
                                     192, RECT{ 0, 0, 800, 600 }
    });

    EXPECT_FLOAT_EQ(state.window.cached_dpi_scale, 2.0f);
    EXPECT_TRUE(HasEffect<effect::RendererSetDpi>(effects));
    const auto* pos = FindEffect<effect::SetWindowPosition>(effects);
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->rect.right, 800);
    EXPECT_EQ(pos->rect.bottom, 600);
}

// ---- テーマ / ズームテスト ----

// 色だけの変更なのでペイン配置は作り直さない。
TEST_F(ReducerTest, ToggleDarkMode_EmitsToggleDarkMode)
{
    state.pane_layout_cache.Set(PaneLayout{});
    auto effects = Reduce(state, ToggleDarkModeAction{});

    EXPECT_TRUE(HasEffect<effect::ToggleDarkMode>(effects));
    EXPECT_TRUE(state.pane_layout_cache.IsValid());
}

// ズームは本文だけに掛かる。サイドペインの幅・スプリッタ幅は変わらないので、
// ペイン配置 (md_rect の位置と幅) も作り直さない。
TEST_F(ReducerTest, ZoomIn_EmitsApplyZoom)
{
    state.pane_layout_cache.Set(PaneLayout{});
    auto effects = Reduce(state, ZoomAction{ ZoomDirection::In });

    EXPECT_TRUE(HasEffect<effect::ApplyZoom>(effects));
    EXPECT_TRUE(state.pane_layout_cache.IsValid());
}

// まとめて届いたノッチは段数ぶん進めるが、組み直しの要求は 1 回だけ出す。
TEST_F(ReducerTest, ZoomInMultipleStepsEmitsSingleThemeChange)
{
    const int before = state.view.viewport.Zoom().Index();
    auto effects = Reduce(state, ZoomAction{ ZoomDirection::In, 3 });

    EXPECT_EQ(state.view.viewport.Zoom().Index(), before + 3);
    EXPECT_EQ(std::ranges::count_if(effects, [](const SideEffect& e) static { return std::holds_alternative<effect::ApplyZoom>(e); }), 1);
}

// サイドペインのズームはそのペインの倍率だけを変え、本文は組み直さない。
TEST_F(ReducerTest, SidePaneZoomChangesOnlyThatPane)
{
    const int md_before = state.view.viewport.Zoom().Index();
    auto effects = Reduce(state, ZoomAction{ ZoomDirection::In, 2, PaneTarget::File });

    EXPECT_EQ(state.view.panes.SidePaneZoom(PaneTarget::File).Index(), ZOOM_DEFAULT_INDEX + 2);
    EXPECT_TRUE(state.view.panes.SidePaneZoom(PaneTarget::Toc).IsDefault());
    EXPECT_EQ(state.view.viewport.Zoom().Index(), md_before);
    EXPECT_FALSE(HasEffect<effect::ApplyZoom>(effects));
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

// 先頭に見えていた項目が先頭に残るよう、スクロール位置を倍率の比で動かす。
TEST_F(ReducerTest, SidePaneZoomScalesScrollPosition)
{
    // 配置が無ければ上限で切らない (項目が無いので切ると 0 になる)。
    state.pane_layout_cache.Invalidate();
    state.view.panes.SidePaneScroll(PaneTarget::Toc).scroll_y = 100.0f;
    Reduce(state, ZoomAction{ ZoomDirection::In, 1, PaneTarget::Toc });

    EXPECT_FLOAT_EQ(state.view.panes.SidePaneScroll(PaneTarget::Toc).scroll_y, 100.0f * ZOOM_STEPS[ZOOM_DEFAULT_INDEX + 1]);
}

// 端で止まったら何もしない。
TEST_F(ReducerTest, SidePaneZoomAtLimitIsNoOp)
{
    state.view.panes.SidePaneZoom(PaneTarget::File).SetIndex(ZOOM_STEP_COUNT - 1);
    auto effects = Reduce(state, ZoomAction{ ZoomDirection::In, 1, PaneTarget::File });

    EXPECT_FALSE(HasEffect<effect::InvalidateWindow>(effects));
}

// ---- HWheel テスト ----

TEST_F(ReducerTest, HWheel_EmitsSetTimer)
{
    auto effects = Reduce(state, HWheelAction{ 120, 1000 });
    EXPECT_TRUE(HasEffect<effect::SetTimer>(effects));
}

// 縦スクロールでスワイプの蓄積を捨て、出ていたオーバーレイを消す。
TEST_F(ReducerTest, VWheel_CancelsSwipeOverlay)
{
    for (uint64_t i = 0; i < 4; ++i) {
        Reduce(state, HWheelAction{ 120, 1000 + i * 10 });
    }
    ASSERT_TRUE(state.interaction.swipe_detector.IsOverlayVisible());

    auto effects = Reduce(state, MouseWheelAction{ .delta = 0, .tick = 1100 });

    EXPECT_FALSE(state.interaction.swipe_detector.IsOverlayVisible());
    const auto* kill = FindEffect<effect::KillTimer>(effects);
    ASSERT_NE(kill, nullptr);
    EXPECT_EQ(kill->id, app_timer::Id::SWIPE_OVERLAY);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

// 直後の水平ホイールは軸ロックで無視する。
TEST_F(ReducerTest, VWheel_LocksHorizontalSwipe)
{
    Reduce(state, MouseWheelAction{ .delta = 0, .tick = 1000 });
    for (uint64_t i = 0; i < 4; ++i) {
        Reduce(state, HWheelAction{ 120, 1010 + i * 10 });
    }
    EXPECT_FALSE(state.interaction.swipe_detector.IsOverlayVisible());
}

TEST_F(ReducerTest, VWheel_NoOverlay_NoEffect)
{
    EXPECT_TRUE(Reduce(state, MouseWheelAction{ .delta = 0, .tick = 1000 }).empty());
}

// ---- CaptureChanged テスト ----

TEST_F(ReducerTest, CaptureChanged_ResetsGesture)
{
    // ジェスチャーがアクティブでない場合、InvalidateWindow は出ない
    auto effects = Reduce(state, CaptureChangedAction{});
    EXPECT_FALSE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, CaptureChanged_ResetsTextSelectionDrag)
{
    // キャプチャ喪失 (Alt+Tab 等) では WM_LBUTTONUP が来ないため、ここで解除しないと
    // テキスト選択ドラッグが残ってファイル自動リロードが永久延期される
    state.view.drag = { .kind = LeftDrag::TextSelect };
    auto effects = Reduce(state, CaptureChangedAction{});
    EXPECT_NE(state.view.drag.kind, LeftDrag::TextSelect);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

namespace {
bool HasReloadTimer(const SideEffectList& effects)
{
    const auto* t = FindEffect<effect::SetTimer>(effects);
    return t && t->id == app_timer::Id::FILE_RELOAD_DEBOUNCE;
}
} // namespace

// ドラッグ中に待たせたリロードは、ドラッグの終わり方によらず 1 回だけ予約する。
TEST_F(ReducerTest, TextSelectionDragEnd_SchedulesDeferredReload)
{
    state.view.drag = { .kind = LeftDrag::TextSelect };
    state.view.reload_after_drag = true;
    auto effects = Reduce(state, TextSelectionEndedAction{ -1, 0 });
    EXPECT_TRUE(HasReloadTimer(effects));
    EXPECT_FALSE(state.view.reload_after_drag);

    state.view.drag = { .kind = LeftDrag::TextSelect };
    EXPECT_FALSE(HasReloadTimer(Reduce(state, TextSelectionEndedAction{ -1, 0 })));
}

TEST_F(ReducerTest, CaptureChanged_SchedulesDeferredReload)
{
    state.view.drag = { .kind = LeftDrag::TextSelect };
    state.view.reload_after_drag = true;
    EXPECT_TRUE(HasReloadTimer(Reduce(state, CaptureChangedAction{})));
    EXPECT_FALSE(state.view.reload_after_drag);
}

TEST_F(ReducerTest, CaptureChanged_ResetsScrollbarDrag)
{
    state.view.drag = { .kind = LeftDrag::MdScrollbar };
    auto effects = Reduce(state, CaptureChangedAction{});
    EXPECT_EQ(state.view.drag.kind, LeftDrag::None);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, CaptureChanged_ResetsBlockHScrollDrag)
{
    state.view.drag = { .kind = LeftDrag::BlockHScroll, .block_node = 3 };
    auto effects = Reduce(state, CaptureChangedAction{});
    EXPECT_EQ(state.view.HDragNode(), -1);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

// ---- タイマーテスト（代表ケース） ----


// ---- ナビ / コピーボタンのホバー ----

TEST_F(ReducerTest, NavHover_NoneToBack_UpdatesStateAndInvalidates)
{
    using Hover = NavDirection;
    state.view.hover.overlay_button = { 3, OverlayButton::CodeCopy };

    // ナビボタン上ではコピー/保存ボタンはホバーしていない
    auto effects = Reduce(state, PointerHoverAction{ PointerHover{ { .nav = Hover::Back } } });

    EXPECT_EQ(state.view.hover.nav, Hover::Back);
    EXPECT_FALSE(state.view.hover.overlay_button.Any());
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, NavHover_Unchanged_NoEffect)
{
    using Hover = NavDirection;
    state.view.hover.nav = Hover::Forward;

    auto effects = Reduce(state, PointerHoverAction{ PointerHover{ { .nav = Hover::Forward } } });

    EXPECT_EQ(state.view.hover.nav, Hover::Forward);
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, ButtonHoverChanged_CopyUpdated_Invalidates)
{
    auto effects = Reduce(state, PointerHoverAction{ PointerHover{ { .overlay_button = OverlayButtonHit{ 3, OverlayButton::CodeCopy } } } });

    EXPECT_EQ(state.view.hover.overlay_button, (OverlayButtonHit{ 3, OverlayButton::CodeCopy }));
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, ButtonHoverChanged_Unchanged_NoEffect)
{
    state.view.hover.overlay_button = { 4, OverlayButton::DiagramSave };

    auto effects = Reduce(state, PointerHoverAction{ PointerHover{ { .overlay_button = OverlayButtonHit{ 4, OverlayButton::DiagramSave } } } });

    EXPECT_EQ(state.view.hover.overlay_button, (OverlayButtonHit{ 4, OverlayButton::DiagramSave }));
    EXPECT_TRUE(effects.empty());
}

// ---- SplitterDrag* テスト ----

TEST_F(ReducerTest, SplitterDragStarted_SetsTargetAndCaptures)
{
    for (const auto edge : { PaneTarget::File, PaneTarget::Toc }) {
        SCOPED_TRACE(static_cast<int>(edge));
        auto effects = Reduce(state, SplitterDragStartedAction{ edge });
        EXPECT_EQ(state.view.drag.kind, LeftDrag::Splitter);
        EXPECT_EQ(state.view.drag.pane, edge);
        EXPECT_TRUE(HasEffect<effect::SetCapture>(effects));
    }
}

TEST_F(ReducerTest, SplitterDragMoved_Splitter1_UpdatesWidthAndInvalidates)
{
    theme.splitter_width = 4.0f;
    state.window.client_width = 1000;
    state.pane_layout_cache.Set(PaneLayout{});
    state.view.drag = { .kind = LeftDrag::Splitter, .pane = PaneTarget::File };
    const float old_width = state.view.panes.GetSidePaneWidth(PaneTarget::File);

    auto effects = Reduce(state, SplitterDragMovedAction{ 300.0f });

    EXPECT_NE(state.view.panes.GetSidePaneWidth(PaneTarget::File), old_width);
    EXPECT_FALSE(state.pane_layout_cache.IsValid());
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, SplitterDragMoved_Splitter2_UpdatesTocWidthAndInvalidates)
{
    theme.splitter_width = 4.0f;
    state.window.client_width = 1000;
    state.pane_layout_cache.Set(PaneLayout{});
    state.view.drag = { .kind = LeftDrag::Splitter, .pane = PaneTarget::Toc };

    // 目次ペインのスプリッタは toc_left 基準で toc_width を変更する
    auto effects = Reduce(state, SplitterDragMovedAction{ 600.0f });

    EXPECT_FALSE(state.pane_layout_cache.IsValid());
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, SplitterDragMoved_InvalidTarget_NoOp)
{
    state.pane_layout_cache.Set(PaneLayout{});
    state.view.drag = { .kind = LeftDrag::PaneScrollbar };
    auto effects = Reduce(state, SplitterDragMovedAction{ 300.0f });

    EXPECT_TRUE(state.pane_layout_cache.IsValid());
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, SplitterDragEnded_ReleasesAndRefreshes)
{
    state.view.drag = { .kind = LeftDrag::Splitter, .pane = PaneTarget::File };
    state.pane_layout_cache.Set(PaneLayout{});

    auto effects = Reduce(state, LeftDragEndedAction{});

    // 配置はドラッグ中の移動ごとに作り直し済みなので、終了では捨てない。
    EXPECT_EQ(state.view.drag.kind, LeftDrag::None);
    EXPECT_TRUE(state.pane_layout_cache.IsValid());
    EXPECT_TRUE(HasEffect<effect::ReleaseCapture>(effects));
    EXPECT_TRUE(HasEffect<effect::SettleViewport>(effects));
}

// ---- SearchInputDrag* テスト ----

TEST_F(ReducerTest, SearchInputDragStarted_BeginsDragAndCaptures)
{
    auto effects = Reduce(state, SearchInputDragStartedAction{ 5 });

    EXPECT_EQ(state.view.drag.kind, LeftDrag::SearchInput);
    EXPECT_EQ(state.view.drag.search_anchor, 5);
    EXPECT_TRUE(HasEffect<effect::SetCapture>(effects));
    EXPECT_TRUE(HasEffect<effect::SearchFocus>(effects));
}

// 右ボタンジェスチャの開始可否は drag.kind で決まる (検索入力ドラッグも含めること)。
TEST_F(ReducerTest, SearchInputDrag_IsLeftDrag)
{
    Reduce(state, SearchInputDragStartedAction{ 0 });
    EXPECT_EQ(state.view.drag.kind, LeftDrag::SearchInput);
    Reduce(state, CaptureChangedAction{});
    EXPECT_EQ(state.view.drag.kind, LeftDrag::None);
}

TEST_F(ReducerTest, LeftDragEnded_ClearsAndReleases)
{
    state.view.drag = { .kind = LeftDrag::BlockHScroll, .block_node = 2, .start_x = 10.0f };
    const auto effects = Reduce(state, LeftDragEndedAction{});
    EXPECT_EQ(state.view.drag.kind, LeftDrag::None);
    EXPECT_EQ(state.view.HDragNode(), -1);
    EXPECT_TRUE(HasEffect<effect::ReleaseCapture>(effects));
}

// 右ボタンのジェスチャーがキャプチャを使っている間は解放させない。
TEST_F(ReducerTest, LeftDragEnded_KeepsCaptureDuringGesture)
{
    Reduce(state, RightClickGestureStartedAction{ 100.0f, 100.0f });
    ASSERT_NE(state.interaction.gesture.GetPhase(), GesturePhase::Idle);
    state.view.drag = { .kind = LeftDrag::Splitter, .pane = PaneTarget::File };
    const auto effects = Reduce(state, LeftDragEndedAction{});
    EXPECT_EQ(state.view.drag.kind, LeftDrag::None);
    EXPECT_FALSE(HasEffect<effect::ReleaseCapture>(effects));
}

TEST_F(ReducerTest, SearchInputDragMoved_NotDragging_NoOp)
{
    auto effects = Reduce(state, SearchInputDragMovedAction{ 3 });
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, SearchInputDragMoved_DraggingAndChanged_EmitsSearchFocus)
{
    state.view.drag = { .kind = LeftDrag::SearchInput, .search_anchor = 3 };
    auto effects = Reduce(state, SearchInputDragMovedAction{ 7 });
    EXPECT_TRUE(HasEffect<effect::SearchFocus>(effects));
}

TEST_F(ReducerTest, SearchInputDragMoved_Unchanged_NoEffect)
{
    state.view.drag = { .kind = LeftDrag::SearchInput, .search_anchor = 5 };
    // caret と selection_start を drag_anchor に合わせると変化なしで no-op
    Reduce(state, SearchSelectionAction{ 5, 5 });
    auto effects = Reduce(state, SearchInputDragMovedAction{ 5 });
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, SearchInputDragEnded_EndsDragAndReleases)
{
    state.view.drag = { .kind = LeftDrag::SearchInput, .search_anchor = 3 };

    auto effects = Reduce(state, LeftDragEndedAction{});

    EXPECT_NE(state.view.drag.kind, LeftDrag::SearchInput);
    EXPECT_TRUE(HasEffect<effect::ReleaseCapture>(effects));
}

// ---- MdScrollbarDrag* テスト ----

TEST_F(ReducerTest, MdScrollbarDragStarted_ThumbHit_StoresOffsetOnly)
{
    // fixture で md_rect.height = 500, total_height = 1000 → content_height=500, max_scroll=500
    // thumb_height = max(24, 500 * 500/1000) = 250, scroll_y=0 → thumb_y = 0
    SetupMdContentHeight(1000.0f);
    state.view.viewport.ScrollTo(0.0f, MdMaxScroll(state));
    const float dip_y = 100.0f; // thumb 内（0〜250）
    const float old_scroll = state.view.viewport.GetScrollY();

    auto effects = Reduce(state, MdScrollbarDragStartedAction{ dip_y });

    EXPECT_EQ(state.view.drag.kind, LeftDrag::MdScrollbar);
    EXPECT_TRUE(HasEffect<effect::SetCapture>(effects));
    EXPECT_FLOAT_EQ(state.view.drag.grip_offset, dip_y);
    // つまみ上クリックではスクロール位置は変化しない
    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), old_scroll);
    EXPECT_FALSE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, MdScrollbarDragStarted_ThumbMiss_JumpsAndEmitsInvalidate)
{
    // つまみ外クリック (thumb 領域外の dip_y=400)
    SetupMdContentHeight(1000.0f);
    state.view.viewport.ScrollTo(0.0f, MdMaxScroll(state));
    const float dip_y = 400.0f;

    auto effects = Reduce(state, MdScrollbarDragStartedAction{ dip_y });

    EXPECT_EQ(state.view.drag.kind, LeftDrag::MdScrollbar);
    EXPECT_TRUE(HasEffect<effect::SetCapture>(effects));
    EXPECT_GT(state.view.viewport.GetScrollY(), 0.0f);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
    EXPECT_TRUE(HasEffect<effect::BitmapManage>(effects));
}

TEST_F(ReducerTest, MdScrollbarDragMoved_WhileDragging_UpdatesScroll)
{
    SetupMdContentHeight(1000.0f);
    state.view.drag = { .kind = LeftDrag::MdScrollbar };
    state.view.viewport.ScrollTo(0.0f, MdMaxScroll(state));

    auto effects = Reduce(state, MdScrollbarDragMovedAction{ 200.0f });

    EXPECT_GT(state.view.viewport.GetScrollY(), 0.0f);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, MdScrollbarDragMoved_NotDragging_NoOp)
{
    // drag_target が MdScrollbar でない → 何もしない
    auto effects = Reduce(state, MdScrollbarDragMovedAction{ 200.0f });
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, MdScrollbarDragEnded_ReleasesAndSchedulesResize)
{
    state.view.drag = { .kind = LeftDrag::MdScrollbar };

    auto effects = Reduce(state, LeftDragEndedAction{});

    EXPECT_EQ(state.view.drag.kind, LeftDrag::None);
    EXPECT_TRUE(HasEffect<effect::ReleaseCapture>(effects));
    EXPECT_TRUE(HasEffect<effect::SettleViewport>(effects));
}

// ---- PaneScrollbarDrag* テスト（Toc ペイン経由）----

namespace {

// 目次 30 件を持つドキュメントをセットアップし、Toc ペインが縦にあふれる状態を作る。
void SetupScrollableToc(AppState& state)
{
    std::string md;
    for (int i = 0; i < 30; ++i) {
        md += "# Heading ";
        md += std::to_string(i);
        md += "\n\n";
    }
    state.document.doc = Document::FromMarkdown(std::string(md), L"test.md");
    PaneLayout pl{};
    pl.toc_rect = { 0.0f, 0.0f, 200.0f, 300.0f };
    state.pane_layout_cache.Set(pl);
    // item_height=28, header_height=32 → content_height=268, total=30*28=840 → scrollable
}

} // namespace

TEST_F(ReducerTest, PaneScrollbarDragStarted_NotScrollable_NoOp)
{
    // エントリが少なく total_content <= content_height → スクロール不要で no-op
    PaneLayout pl{};
    pl.toc_rect = { 0.0f, 0.0f, 200.0f, 300.0f };
    state.pane_layout_cache.Set(pl);
    // 既定の Toc (エントリ 0) で総コンテンツ 0 → スクロール不要
    auto effects = Reduce(state, PaneScrollbarDragStartedAction{ PaneTarget::Toc, 100.0f });
    EXPECT_EQ(state.view.drag.kind, LeftDrag::None);
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, PaneScrollbarDragStarted_TocScrollable_ThumbHit_StoresOffset)
{
    SetupScrollableToc(state);
    // header_height=32 → content_top=32。scroll=0 の時 thumb_y = content_top = 32
    const float dip_y = 40.0f; // thumb 内を想定
    auto& scroll = state.view.panes.SidePaneScroll(PaneTarget::Toc);
    scroll.scroll_y = 0.0f;

    auto effects = Reduce(state, PaneScrollbarDragStartedAction{ PaneTarget::Toc, dip_y });

    EXPECT_EQ(state.view.drag.kind, LeftDrag::PaneScrollbar);
    EXPECT_EQ(state.view.drag.pane, PaneTarget::Toc);
    EXPECT_TRUE(HasEffect<effect::SetCapture>(effects));
    // thumb 上クリックではスクロール位置は不変
    EXPECT_FLOAT_EQ(state.view.panes.SidePaneScroll(PaneTarget::Toc).scroll_y, 0.0f);
    EXPECT_FALSE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, PaneScrollbarDragStarted_TocScrollable_ThumbMiss_Jumps)
{
    SetupScrollableToc(state);
    const float dip_y = 250.0f; // thumb 外（下側）
    state.view.panes.SidePaneScroll(PaneTarget::Toc).scroll_y = 0.0f;

    auto effects = Reduce(state, PaneScrollbarDragStartedAction{ PaneTarget::Toc, dip_y });

    EXPECT_EQ(state.view.drag.kind, LeftDrag::PaneScrollbar);
    EXPECT_TRUE(HasEffect<effect::SetCapture>(effects));
    EXPECT_GT(state.view.panes.SidePaneScroll(PaneTarget::Toc).scroll_y, 0.0f);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, PaneScrollbarDragMoved_WhileDragging_UpdatesScroll)
{
    SetupScrollableToc(state);
    state.view.drag = { .kind = LeftDrag::PaneScrollbar, .pane = PaneTarget::Toc };
    state.view.panes.SidePaneScroll(PaneTarget::Toc).scroll_y = 0.0f;

    auto effects = Reduce(state, PaneScrollbarDragMovedAction{ 200.0f });

    EXPECT_GT(state.view.panes.SidePaneScroll(PaneTarget::Toc).scroll_y, 0.0f);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, PaneScrollbarDragMoved_NotDragging_NoOp)
{
    state.view.drag = { .kind = LeftDrag::Splitter };
    auto effects = Reduce(state, PaneScrollbarDragMovedAction{ 200.0f });
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, PaneScrollbarDragEnded_TocOrFile_Releases)
{
    state.view.drag = { .kind = LeftDrag::PaneScrollbar, .pane = PaneTarget::Toc };

    auto effects = Reduce(state, LeftDragEndedAction{});

    EXPECT_EQ(state.view.drag.kind, LeftDrag::None);
    EXPECT_TRUE(HasEffect<effect::ReleaseCapture>(effects));
}

// ---- TextSelection* テスト ----

TEST_F(ReducerTest, TextSelectionStarted_WithHit_SetsAnchorAndCaptures)
{
    state.document.doc = Document::FromMarkdown(std::string("Hello world"), L"test.md");
    // 既存の選択を残しておき、クリア動作を検証
    state.view.viewport.SetSelection(TextSelection::MakeOrdered(0, 0, 0, 5));

    auto effects = Reduce(state, TextSelectionStartedAction{ 0, 3u, 10, 20 });

    EXPECT_EQ(state.view.drag.kind, LeftDrag::TextSelect);
    EXPECT_EQ(state.view.drag.click_x, 10);
    EXPECT_EQ(state.view.drag.click_y, 20);
    EXPECT_EQ(state.view.drag.select_node, 0);
    EXPECT_EQ(state.view.drag.select_pos, 3u);
    EXPECT_FALSE(state.view.viewport.GetSelection().active());
    EXPECT_TRUE(HasEffect<effect::SetCapture>(effects));
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, TextSelectionStarted_NoHit_DoesNotStartDrag)
{
    // ヒットなし（空白領域のクリック）: SetCapture もドラッグも始めない。
    // SetCapture をスキップすることで、対応する ReleaseCapture を省略できる。
    auto effects = Reduce(state, TextSelectionStartedAction{ -1, 0u, 5, 7 });

    EXPECT_NE(state.view.drag.kind, LeftDrag::TextSelect);
    EXPECT_FALSE(HasEffect<effect::SetCapture>(effects));
    EXPECT_FALSE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, TextSelectionMoved_NotDragging_NoOp)
{
    state.document.doc = Document::FromMarkdown(std::string("Hello"), L"test.md");
    auto effects = Reduce(state, TextSelectionMovedAction{ 0, 3u });
    EXPECT_FALSE(state.view.viewport.GetSelection().active());
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, TextSelectionMoved_DraggingWithHit_UpdatesSelection)
{
    state.document.doc = Document::FromMarkdown(std::string("Hello world"), L"test.md");
    state.view.drag = { .kind = LeftDrag::TextSelect, .select_node = 0, .select_pos = 2u };

    auto effects = Reduce(state, TextSelectionMovedAction{ 0, 7u });

    const auto& sel = state.view.viewport.GetSelection();
    EXPECT_TRUE(sel.active());
    EXPECT_EQ(sel.start_node, 0);
    EXPECT_EQ(sel.end_node, 0);
    EXPECT_EQ(sel.start_pos, 2u);
    EXPECT_EQ(sel.end_pos, 7u);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, TextSelectionMoved_NoHit_NoUpdate)
{
    state.view.drag = { .kind = LeftDrag::TextSelect, .select_node = 0, .select_pos = 2u };

    auto effects = Reduce(state, TextSelectionMovedAction{ -1, 0u });

    EXPECT_FALSE(state.view.viewport.GetSelection().active());
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, TextSelectionEnded_NotDragging_NoOp)
{
    auto effects = Reduce(state, TextSelectionEndedAction{ 0, 3u });
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, TextSelectionEnded_WithHit_FinalizesSelection)
{
    state.document.doc = Document::FromMarkdown(std::string("Hello world"), L"test.md");
    state.view.drag = { .kind = LeftDrag::TextSelect, .select_node = 0, .select_pos = 2u };

    auto effects = Reduce(state, TextSelectionEndedAction{ 0, 8u });

    const auto& sel = state.view.viewport.GetSelection();
    EXPECT_TRUE(sel.active());
    EXPECT_EQ(sel.start_pos, 2u);
    EXPECT_EQ(sel.end_pos, 8u);
    EXPECT_NE(state.view.drag.kind, LeftDrag::TextSelect);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, TextSelectionEnded_NoHit_ClearsDraggingOnly)
{
    state.view.drag = { .kind = LeftDrag::TextSelect, .select_node = 0, .select_pos = 2u };
    // 事前選択なし
    auto effects = Reduce(state, TextSelectionEndedAction{ -1, 0u });

    EXPECT_NE(state.view.drag.kind, LeftDrag::TextSelect);
    EXPECT_FALSE(state.view.viewport.GetSelection().active());
    EXPECT_TRUE(HasEffect<effect::ReleaseCapture>(effects));
    // 見た目が変わらないので再描画しない
    EXPECT_FALSE(HasEffect<effect::InvalidateWindow>(effects));
}

// 選択の無い状態でのクリックは、押下でも再描画しない。
TEST_F(ReducerTest, TextSelectionStarted_WithoutSelection_DoesNotInvalidate)
{
    auto effects = Reduce(state, TextSelectionStartedAction{ 0, 2u, 10, 20 });
    EXPECT_EQ(state.view.drag.kind, LeftDrag::TextSelect);
    EXPECT_TRUE(HasEffect<effect::SetCapture>(effects));
    EXPECT_FALSE(HasEffect<effect::InvalidateWindow>(effects));
}

// ---- RightClickGesture* テスト ----

TEST_F(ReducerTest, RightClickGestureStarted_EntersPressedAndCaptures)
{
    auto effects = Reduce(state, RightClickGestureStartedAction{ 100.0f, 200.0f });

    EXPECT_EQ(state.interaction.gesture.GetPhase(), GesturePhase::Pressed);
    EXPECT_TRUE(HasEffect<effect::SetCapture>(effects));
}

TEST_F(ReducerTest, RightClickGestureMoved_BelowThreshold_StaysPressed)
{
    state.interaction.gesture.OnRButtonDown(100.0f, 200.0f);

    auto effects = Reduce(state, RightClickGestureMovedAction{ 105.0f, 205.0f });

    EXPECT_EQ(state.interaction.gesture.GetPhase(), GesturePhase::Pressed);
    // Tracking でないので InvalidateWindow は出ない
    EXPECT_FALSE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, RightClickGestureMoved_AboveThreshold_EntersTracking)
{
    state.interaction.gesture.OnRButtonDown(100.0f, 200.0f);

    // 閾値 (30.0f) を超える水平方向移動
    auto effects = Reduce(state, RightClickGestureMovedAction{ 200.0f, 200.0f });

    EXPECT_EQ(state.interaction.gesture.GetPhase(), GesturePhase::Tracking);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, RightClickGestureCompleted_Idle_NoOp)
{
    auto effects = Reduce(state, RightClickGestureCompletedAction{ 0, 0 });
    EXPECT_TRUE(effects.empty());
}

TEST_F(ReducerTest, RightClickGestureCompleted_Pressed_ShowsContextMenu)
{
    state.interaction.gesture.OnRButtonDown(100.0f, 200.0f);

    auto effects = Reduce(state, RightClickGestureCompletedAction{ 400, 500 });

    EXPECT_EQ(state.interaction.gesture.GetPhase(), GesturePhase::Idle);
    EXPECT_TRUE(HasEffect<effect::ReleaseCapture>(effects));
    const auto* menu = FindEffect<effect::ShowContextMenu>(effects);
    ASSERT_NE(menu, nullptr);
    EXPECT_EQ(menu->screen_x, 400);
    EXPECT_EQ(menu->screen_y, 500);
}

TEST_F(ReducerTest, RightClickGestureCompleted_TrackingLeft_NavigatesBack)
{
    // 戻り先履歴を用意
    state.document.doc = Document::FromMarkdown(std::string("test"), L"C:\\current.md");
    state.view.nav_history.Push(NavEntry{ L"C:\\prev.md", 0, 0.0f });

    state.interaction.gesture.OnRButtonDown(200.0f, 200.0f);
    state.interaction.gesture.OnMouseMove(100.0f, 200.0f); // 左に 100px = 閾値越え

    auto effects = Reduce(state, RightClickGestureCompletedAction{ 0, 0 });

    EXPECT_EQ(state.interaction.gesture.GetPhase(), GesturePhase::Idle);
    EXPECT_TRUE(HasEffect<effect::ReleaseCapture>(effects));
    // ReduceNavigateBack が走るので LoadFile が出る
    EXPECT_TRUE(HasEffect<effect::LoadFile>(effects));
}

TEST_F(ReducerTest, RightClickGestureCompleted_TrackingRight_NavigatesForward)
{
    // 前進履歴を用意: page2 からスタート → Push(page1) → GoBack → 前進スタックに page2
    state.view.nav_history.Push(NavEntry{ L"C:\\page1.md", 0, 0.0f });
    NavEntry tmp;
    state.view.nav_history.GoBack(NavEntry{ L"C:\\page2.md", 0, 0.0f }, tmp);
    // 現在ドキュメントを page1（後進結果）に合わせる
    state.document.doc = Document::FromMarkdown(std::string("test"), L"C:\\page1.md");
    ASSERT_TRUE(state.view.nav_history.CanGoForward());

    state.interaction.gesture.OnRButtonDown(100.0f, 200.0f);
    state.interaction.gesture.OnMouseMove(200.0f, 200.0f); // 右に 100px

    auto effects = Reduce(state, RightClickGestureCompletedAction{ 0, 0 });

    EXPECT_EQ(state.interaction.gesture.GetPhase(), GesturePhase::Idle);
    EXPECT_TRUE(HasEffect<effect::ReleaseCapture>(effects));
    // GoForward で page2.md に進むので LoadFile 発火
    EXPECT_TRUE(HasEffect<effect::LoadFile>(effects));
}

// ---- FilePane* テスト ----

TEST_F(ReducerTest, FilePaneDirectoryClicked_UpdatesDirAndInvalidates)
{
    // スクロール位置を設定してリセットされることを確認
    state.view.panes.SidePaneScroll(PaneTarget::File).scroll_y = 100.0f;

    auto effects = Reduce(state, FilePaneDirectoryClickedAction{ std::wstring(L"C:\\nonexistent_dir_for_test") });

    EXPECT_EQ(state.view.panes.SidePaneScroll(PaneTarget::File).scroll_y, 0.0f);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

// ---- FilePaneRevealCurrentFile テスト ----

class FilePaneRevealTest : public ReducerTest {
protected:
    ScopedTempDir temp_dir_;
    const std::filesystem::path dir_ = temp_dir_.path();

    void SetUp() override
    {
        ReducerTest::SetUp();
        // ".." + f00.md〜f29.md の 31 項目
        for (int i = 0; i < 30; ++i) {
            temp_dir_.Write(std::format(L"f{:02}.md", i), "x");
        }
        // ファイルペインの内容領域は 10 項目分 (280 DIP)
        PaneLayout pl{};
        pl.md_rect.height = 500.0f;
        pl.file_rect.height = theme.pane_header_height + 280.0f;
        state.pane_layout_cache.Set(pl);
    }

    // App::FinishLoadMarkdownFile と同じく現在ファイルも登録する。
    void OpenDocument(const wchar_t* name)
    {
        state.document.doc = Document::FromMarkdown(std::string("x"), (dir_ / name).wstring());
        state.file_explorer.SetCurrentFile(state.document.doc.GetFilePath());
    }

    float FileScroll() const
    {
        return state.view.panes.SidePaneScroll(PaneTarget::File).scroll_y;
    }
};

TEST_F(FilePaneRevealTest, MovesToDocumentDirectoryAndCentersCurrentFile)
{
    OpenDocument(L"f15.md");
    state.file_explorer.SetDirectory(dir_.parent_path().wstring());

    auto effects = Reduce(state, FilePaneRevealCurrentFileAction{});

    EXPECT_TRUE(path_util::iequal(state.file_explorer.GetDirectory(), dir_.wstring()));
    const auto& entries = state.file_explorer.GetEntries();
    ASSERT_EQ(entries.size(), 31u);
    EXPECT_TRUE(entries[16].is_current);
    // index 16 の上端 448 から (280 - 28) / 2 を引いて中央に置く
    EXPECT_FLOAT_EQ(FileScroll(), 322.0f);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

// 列挙を別スレッドに任せていると一覧がまだ無いので、届いてからやり直させる。
TEST_F(FilePaneRevealTest, DefersRevealWhileListingAsync)
{
    OpenDocument(L"f15.md");
    std::optional<uint32_t> request;
    state.file_explorer.SetAsyncLister([&request](std::wstring, uint32_t r) { request = r; });

    Reduce(state, FilePaneRevealCurrentFileAction{});
    ASSERT_TRUE(request.has_value());
    EXPECT_TRUE(state.file_explorer.IsListing());

    ASSERT_TRUE(state.file_explorer.ApplyListing(*request, FileExplorer::List(dir_.wstring())));
    ASSERT_TRUE(state.file_explorer.TakeDeferredReveal());
    Reduce(state, FilePaneRevealCurrentFileAction{});
    EXPECT_FLOAT_EQ(FileScroll(), 322.0f);
}

TEST_F(FilePaneRevealTest, ClampsScrollNearEnd)
{
    OpenDocument(L"f29.md");

    Reduce(state, FilePaneRevealCurrentFileAction{});

    // max_scroll = 31 * 28 - 280
    EXPECT_FLOAT_EQ(FileScroll(), 588.0f);
}

TEST_F(FilePaneRevealTest, SameDirectoryStillScrollsToCurrentFile)
{
    OpenDocument(L"f15.md");
    state.file_explorer.SetDirectory(dir_.wstring());

    Reduce(state, FilePaneRevealCurrentFileAction{});

    EXPECT_FLOAT_EQ(FileScroll(), 322.0f);
}

TEST_F(FilePaneRevealTest, HelpDocumentIsNoOp)
{
    state.document.doc = Document::FromMarkdown(std::string("help"), HELP_PATH);
    state.file_explorer.SetDirectory(dir_.wstring());
    state.view.panes.SidePaneScroll(PaneTarget::File).scroll_y = 100.0f;

    auto effects = Reduce(state, FilePaneRevealCurrentFileAction{});

    EXPECT_TRUE(effects.empty());
    EXPECT_FLOAT_EQ(FileScroll(), 100.0f);
}

TEST_F(FilePaneRevealTest, NoDocumentIsNoOp)
{
    auto effects = Reduce(state, FilePaneRevealCurrentFileAction{});

    EXPECT_TRUE(effects.empty());
    EXPECT_TRUE(state.file_explorer.GetDirectory().empty());
}

// ---- TocItemClicked テスト ----

TEST_F(ReducerTest, TocItemClicked_InvalidIndex_PushesHistoryOnly)
{
    state.document.doc = Document::FromMarkdown(std::string("test"), L"C:\\file.md");

    auto effects = Reduce(state, TocItemClickedAction{ -1 });

    EXPECT_TRUE(state.view.nav_history.CanGoBack());
    EXPECT_FALSE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(ReducerTest, TocItemClicked_ValidAnchor_ScrollsAndInvalidates)
{
    SetDocument(state, "# First\n\n# Second\n\n# Third", L"C:\\file.md", 400.0f);

    auto effects = Reduce(state, TocItemClickedAction{ 1 });

    EXPECT_TRUE(state.view.nav_history.CanGoBack());
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
    EXPECT_TRUE(HasEffect<effect::BitmapManage>(effects));
}

// issue#224: 末尾セクションへのジャンプで scroll_y が max_scroll を超えると、
// その後のホイールスクロールで位置が一気に補正されて「飛ぶ」体感になる。
// ApplyScrollTarget が事前クランプする契約を担保する。
TEST_F(ReducerTest, TocItemClicked_TailSection_ClampsToMaxScroll)
{
    // 末尾見出しを max_scroll (= 500) より遥か下に配置 → ペイン上端へ持ってくると要求 scroll_y > max_scroll
    SetDocument(state, "# First\n\n# Second\n\n# Tail", L"C:\\file.md", 450.0f);
    const int tail = static_cast<int>(state.document.doc.GetNodes().size()) - 1;
    ASSERT_FLOAT_EQ(state.document.layout_cache.Top(tail), 900.0f);

    Reduce(state, TocItemClickedAction{ tail });

    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), MdMaxScroll(state));
    EXPECT_FALSE(state.view.viewport.HasScrollTarget());
}

namespace {

// 見出し 30 件の文書を、目次ペインがあふれる大きさで表示する。最初の描画の同期を済ませた状態を返す。
void SetupTocFollow(AppState& state)
{
    std::string md;
    for (int i = 0; i < 30; ++i) {
        md += std::format("# Heading {}\n\n", i);
        for (int k = 0; k < 20; ++k) {
            md += "line\n\n";
        }
    }
    state.view.panes.SetSidePaneVisible(PaneTarget::Toc, true);
    PaneLayout pl{};
    pl.md_rect = { 400.0f, 0.0f, 800.0f, 400.0f };
    pl.toc_rect = { 0.0f, 0.0f, 200.0f, 300.0f };
    state.pane_layout_cache.Set(pl);
    SetDocument(state, md, L"C:\\file.md", 400.0f);
    SyncActiveTocIndex(state);
}

float TocPaneScroll(AppState& state)
{
    return state.view.panes.SidePaneScroll(PaneTarget::Toc).scroll_y;
}

} // namespace

// MD ペインのスクロール由来では、描画時の同期でアクティブ見出しが変わり、目次ペインが追従する。
TEST_F(ReducerTest, SyncActiveToc_FollowsMdScroll)
{
    SetupTocFollow(state);
    ASSERT_EQ(state.view.active_toc_index, 0);
    ASSERT_FLOAT_EQ(TocPaneScroll(state), 0.0f);

    Reduce(state, NavigateAnchorAction{ std::string("heading-20") });
    SyncActiveTocIndex(state);

    EXPECT_EQ(state.view.active_toc_index, 20);
    EXPECT_GT(TocPaneScroll(state), 0.0f) << "アンカーリンク由来でも目次は追従する (抑制は目次クリックのみ)";
}

// issue#259: 目次クリック由来のジャンプでは、直後の同期でハイライトだけ更新し、目次ペインはスクロールさせない。
// クリック先の項目がペイン内で動いて見失う混乱を防ぐ契約。抑制はその 1 回だけで、以後のスクロールでは追従する。
TEST_F(ReducerTest, SyncActiveToc_TocClickKeepsTocPaneOnce)
{
    SetupTocFollow(state);

    Reduce(state, TocItemClickedAction{ state.document.doc.GetToc()[20].node_index });
    SyncActiveTocIndex(state);
    EXPECT_EQ(state.view.active_toc_index, 20);
    EXPECT_FLOAT_EQ(TocPaneScroll(state), 0.0f);

    Reduce(state, NavigateAnchorAction{ std::string("heading-25") });
    SyncActiveTocIndex(state);
    EXPECT_EQ(state.view.active_toc_index, 25);
    EXPECT_GT(TocPaneScroll(state), 0.0f);
}

// スクロールしなくても、計測などで見出しの Y が動けば次の描画で決め直す。
TEST_F(ReducerTest, SyncActiveToc_FollowsLayoutChangeWithoutScroll)
{
    SetupTocFollow(state);
    const float scroll_y = state.view.viewport.GetScrollY();
    auto& cache = state.document.layout_cache;
    const auto& toc = state.document.doc.GetToc();
    ASSERT_EQ(state.view.active_toc_index, 0);

    // 見出し 1 より前の高さを縮めて、見出し 1 を画面上端より上へ動かす。
    cache[0].height = 0.0f;
    for (int i = 1; i < toc[1].node_index; ++i) {
        cache[i].height = 0.0f;
    }
    layout::RecomputeYPositions(state.document.doc.GetNodes(), cache, *state.theme);
    ASSERT_FLOAT_EQ(state.view.viewport.GetScrollY(), scroll_y);
    ASSERT_LE(cache.Top(static_cast<size_t>(toc[1].node_index)), scroll_y + state.theme->heading_spacing_above);

    SyncActiveTocIndex(state);
    EXPECT_GE(state.view.active_toc_index, 1);
}

// ---- 副作用順序 (test_helpers.h::HasEffectInOrder) ----
// PerformSizingUpdate はリサイズ済みの render target を読むため、順序そのものを検証する。
TEST_F(ReducerTest, Resize_EmitsRendererResizeBeforeSizingUpdate)
{
    state.window.is_sizing = true;
    auto effects = Reduce(state, ResizeAction{ 1024, 768 });
    EXPECT_TRUE((HasEffectInOrder<effect::RendererResize, effect::PerformSizingUpdate>(effects)));
}

// ペイン配置はクライアント寸法と DPI から組む。DPI が変わると同じピクセル寸法でも DIP の幅が変わり、
// 続く同じ大きさの WM_SIZE も描画面の作り直しのために通す。
TEST_F(ReducerTest, PaneLayoutFollowsClientSizeAndDpi)
{
    Reduce(state, ResizeAction{ 1000, 800 });
    EXPECT_FLOAT_EQ(EnsurePaneLayout(state).md_rect.width + EnsurePaneLayout(state).md_rect.x, 1000.0f);

    Reduce(state, DpiChangedAction{ 192, RECT{ 0, 0, 1000, 800 } });
    EXPECT_FLOAT_EQ(state.window.WidthDip(), 500.0f);
    EXPECT_FLOAT_EQ(EnsurePaneLayout(state).md_rect.width + EnsurePaneLayout(state).md_rect.x, 500.0f);

    EXPECT_TRUE(HasEffect<effect::RendererResize>(Reduce(state, ResizeAction{ 1000, 800 })));
    EXPECT_FALSE(HasEffect<effect::RendererResize>(Reduce(state, ResizeAction{ 1000, 800 })));
}

// ---- DocumentOpenedAction のスクロール復元: 2 分岐 ----
// 仕様 (reducer.cpp の RestoreScrollAfterLoad):
//   1. pending_restore 有効 -> SetScrollTarget(node, offset)
//   2. (else) -> SetScrollY(0)

TEST_F(ReducerTest, DocumentOpened_HasPendingRestore_SetsScrollTarget)
{
    SetDocument(state, "# A\n\n# B\n\n# C", L"test.md");
    state.view.pending_restore = { 1, 7.0f };

    Reduce(state, DocumentOpenedAction{});

    // ApplyScrollTarget 後は scroll_target が消費される実装になり得るため
    // ScrollY 側で検証する (cache.Top(1) + offset)。
    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), 107.0f);
    EXPECT_FALSE(state.view.pending_restore.IsValid()); // 消費される
}

TEST_F(ReducerTest, DocumentOpened_NoRestoreInfo_ScrollsToTop)
{
    state.view.viewport.ScrollTo(300.0f, MdMaxScroll(state));

    Reduce(state, DocumentOpenedAction{});

    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), 0.0f);
}

// 戻る先のファイルが削除済み等でロードに失敗した場合、その復元情報が次に開く別ファイルへ
// 持ち越されると、無関係なノード位置へスクロールする。
// 履歴も戻る前に戻す (残すと、進むで今いるファイルへ「移動」するだけになる)。
TEST_F(ReducerTest, LoadFailed_DropsPendingRestoreForNextDocument)
{
    state.document.doc = Document::FromMarkdown(std::string("# A\n\n# B\n\n# C"), L"C:\\current.md");
    state.view.nav_history.Push(NavEntry{ L"C:\\deleted.md", 2, 30.0f });
    auto effects = Reduce(state, NavigateBackAction{});
    ASSERT_TRUE(HasEffect<effect::LoadFile>(effects));
    ASSERT_TRUE(state.view.pending_restore.IsValid());

    Reduce(state, LoadFailedAction{});
    EXPECT_EQ(state.view.nav_history.BackSize(), 1u);
    EXPECT_FALSE(state.view.nav_history.CanGoForward());

    // 続けてユーザーが別ファイルを開き、ロードに成功する。
    Reduce(state, OpenPathAction{ std::wstring(L"C:\\other.md") });
    SetDocument(state, "# X\n\n# Y\n\n# Z", L"C:\\other.md");
    Reduce(state, DocumentOpenedAction{});

    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), 0.0f);
}

// 開けなかったファイルの分の履歴は取り消す。残すと戻る履歴に現在位置が重複し、進む履歴も消える。
TEST_F(ReducerTest, LoadFailed_RollsBackHistoryPushedForOpen)
{
    state.document.doc = Document::FromMarkdown(std::string("test"), L"C:\\current.md");
    state.view.nav_history.Push(NavEntry{ L"C:\\next.md", 0, 0.0f });
    NavEntry tmp;
    ASSERT_TRUE(state.view.nav_history.GoBack(NavEntry{ L"C:\\later.md", 0, 0.0f }, tmp));
    state.view.nav_history.CommitPending();
    const size_t back = state.view.nav_history.BackSize();
    ASSERT_TRUE(state.view.nav_history.CanGoForward());

    Reduce(state, OpenPathAction{ std::wstring(L"C:\\too_big.md") });
    Reduce(state, LoadFailedAction{});
    EXPECT_EQ(state.view.nav_history.BackSize(), back);
    EXPECT_TRUE(state.view.nav_history.CanGoForward());
}

// 今のファイルの開き直しはリロードとして終わり確定が来ないので、仮の履歴にしない。
// 仮のままだと、後の無関係なリロード失敗で取り消される。
TEST_F(ReducerTest, LoadFailed_KeepsHistoryPushedForReopeningCurrentFile)
{
    state.document.doc = Document::FromMarkdown(std::string("test"), L"C:\\Current.md");
    Reduce(state, OpenPathAction{ std::wstring(L"C:\\current.md") });
    Reduce(state, LoadFailedAction{});
    EXPECT_TRUE(state.view.nav_history.CanGoBack());
}

// 同じファイル内の戻るはロードを伴わないので即確定し、後の無関係なロード失敗で取り消さない。
TEST_F(ReducerTest, LoadFailed_KeepsSameFileNavigateBack)
{
    state.document.doc = Document::FromMarkdown(std::string("# A\n\n# B"), L"C:\\current.md");
    state.view.nav_history.Push(NavEntry{ L"C:\\current.md", 0, 0.0f });

    auto effects = Reduce(state, NavigateBackAction{});
    ASSERT_FALSE(HasEffect<effect::LoadFile>(effects));
    Reduce(state, LoadFailedAction{});

    EXPECT_FALSE(state.view.nav_history.CanGoBack());
    EXPECT_TRUE(state.view.nav_history.CanGoForward());
}

// ロード成功後に別のロード (リロード等) が失敗しても、確定した履歴は取り消さない。
TEST_F(ReducerTest, LoadFailed_KeepsHistoryAfterSuccessfulLoad)
{
    state.document.doc = Document::FromMarkdown(std::string("test"), L"C:\\current.md");
    Reduce(state, OpenPathAction{ std::wstring(L"C:\\next.md") });
    state.document.doc = Document::FromMarkdown(std::string("next"), L"C:\\next.md");
    Reduce(state, DocumentOpenedAction{});
    Reduce(state, LoadFailedAction{});
    EXPECT_TRUE(state.view.nav_history.CanGoBack());
}

// リンク切れのアンカーでは履歴を積まない (進む履歴が消えない)。
TEST_F(ReducerTest, NavigateAnchor_MissingAnchorKeepsHistory)
{
    state.document.doc = Document::FromMarkdown(std::string("# First\n\n# Second"), L"C:\\file.md");
    state.view.nav_history.Push(NavEntry{ L"C:\\file.md", 0, 0.0f });
    NavEntry tmp;
    ASSERT_TRUE(state.view.nav_history.GoBack(NavEntry{ L"C:\\file.md", 1, 0.0f }, tmp));

    auto effects = Reduce(state, NavigateAnchorAction{ std::string("typo") });

    EXPECT_TRUE(effects.empty());
    EXPECT_FALSE(state.view.nav_history.CanGoBack());
    EXPECT_TRUE(state.view.nav_history.CanGoForward());
}

// ---- ロード中に次の移動が重なる連続操作 ----

namespace {

// 見出し 4 つ (高さ 100 ずつ) の文書に差し替える。
void ReplaceDocument(AppState& state, std::wstring_view path)
{
    // 表示高 500 に対して内容高 1000 で、500 までスクロールできる。
    SetDocument(state, "# A\n\n# B\n\n# C\n\n# D", path, 250.0f);
}

void ResetToDocument(AppState& state, std::wstring_view path)
{
    state.view.nav_history = NavHistory{};
    ReplaceDocument(state, path);
}

// App のロード成功相当: 文書を差し替えて DocumentOpenedAction で確定する。
void CompleteLoad(AppState& state, std::wstring_view path)
{
    ReplaceDocument(state, path);
    Reduce(state, DocumentOpenedAction{});
}

// 副作用の LoadFile のパス。LoadFile が無ければ nullopt。
std::optional<std::wstring> LoadedPath(const SideEffectList& effects)
{
    const auto* load = FindEffect<effect::LoadFile>(effects);
    return load ? std::optional<std::wstring>(load->path) : std::nullopt;
}

} // namespace

// 別ファイルへの「戻る」のロード中に別のファイルを開くと、戻る先は取り消される。
// その復元位置を残すと、開いたファイルが戻る先のノード位置へスクロールする。
TEST_F(ReducerTest, OpenWhileNavigateBackLoading_DoesNotCarryRestoreOver)
{
    ResetToDocument(state, L"C:\\current.md");
    state.view.nav_history.Push(NavEntry{ L"C:\\b.md", 2, 30.0f });
    ASSERT_TRUE(HasEffect<effect::LoadFile>(Reduce(state, NavigateBackAction{})));
    ASSERT_TRUE(state.view.pending_restore.IsValid());

    Reduce(state, OpenPathAction{ std::wstring(L"C:\\other.md") });
    CompleteLoad(state, L"C:\\other.md");

    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), 0.0f);
    EXPECT_FALSE(state.view.pending_restore.IsValid());
    EXPECT_EQ(state.view.nav_history.BackSize(), 2u);
    EXPECT_EQ(state.view.nav_history.ForwardSize(), 0u);
}

// ヘルプ表示中の F1 はヘルプに留まり、ロード中の別ファイルを取り消すだけなので確定も失敗も来ない。
// その「戻る」の仮の履歴と復元位置を残すと、戻るボタンが無効なまま進むボタンだけ有効になる。
TEST_F(ReducerTest, ShowHelpWhileOnHelp_RollsBackPendingNavigation)
{
    ResetToDocument(state, HELP_PATH);
    state.view.nav_history.Push(NavEntry{ L"C:\\b.md", 3, 0.0f });
    ASSERT_TRUE(HasEffect<effect::LoadFile>(Reduce(state, NavigateBackAction{})));

    const auto effects = Reduce(state, ShowHelpAction{});

    EXPECT_FALSE(HasEffect<effect::LoadFile>(effects));
    EXPECT_TRUE(HasEffect<effect::CancelOtherFileLoad>(effects));
    EXPECT_EQ(state.view.nav_history.BackSize(), 1u);
    EXPECT_EQ(state.view.nav_history.ForwardSize(), 0u);
    EXPECT_FALSE(state.view.pending_restore.IsValid());
}

// 別ファイルのロード中に今のファイルを開き直すと、ロード中の分は取り消される。
// 仮の履歴を確定させてから積むと、戻る履歴に今の位置が 2 件並ぶ。
TEST_F(ReducerTest, ReopenCurrentWhileOpenLoading_DoesNotDuplicateHistory)
{
    ResetToDocument(state, L"C:\\docs\\a.md");
    Reduce(state, OpenPathAction{ std::wstring(L"C:\\docs\\big.md") });
    Reduce(state, OpenPathAction{ std::wstring(L"c:\\docs\\A.md") });
    EXPECT_EQ(state.view.nav_history.BackSize(), 1u);
}

// 開く (ロード中) → 同じファイル内の項目へ戻る: 開く分を取り消してから移動し、ロードも取り消す。
// その後に古いロードの失敗が届いても、確定した戻るは取り消さない。
TEST_F(ReducerTest, NavigateBackWithinFileWhileOpenLoading_CommitsAndCancelsLoad)
{
    ResetToDocument(state, L"C:\\current.md");
    state.view.nav_history.Push(NavEntry{ L"C:\\current.md", 0, 0.0f });
    state.view.viewport.ScrollTo(250.0f, MdMaxScroll(state));
    Reduce(state, OpenPathAction{ std::wstring(L"C:\\big.md") });

    const auto effects = Reduce(state, NavigateBackAction{});
    EXPECT_FALSE(HasEffect<effect::LoadFile>(effects));
    EXPECT_TRUE(HasEffect<effect::CancelOtherFileLoad>(effects));
    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), 0.0f);

    Reduce(state, LoadFailedAction{});
    EXPECT_EQ(state.view.nav_history.BackSize(), 0u);
    EXPECT_EQ(state.view.nav_history.ForwardSize(), 1u);
    Reduce(state, NavigateForwardAction{});
    EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), 250.0f);
}

// 開く (ロード中) → 別ファイルへ戻る: 開く分を取り消して戻る先をロードする。
// 失敗なら戻る前の履歴に戻り、成功なら戻る先の位置へ復元して進む履歴に元の位置が残る。
TEST_F(ReducerTest, NavigateBackToOtherFileWhileOpenLoading)
{
    for (const bool succeed : { false, true }) {
        SCOPED_TRACE(succeed);
        ResetToDocument(state, L"C:\\current.md");
        state.view.nav_history.Push(NavEntry{ L"C:\\b.md", 2, 10.0f });
        state.view.viewport.ScrollTo(250.0f, MdMaxScroll(state));
        Reduce(state, OpenPathAction{ std::wstring(L"C:\\big.md") });

        const auto effects = Reduce(state, NavigateBackAction{});
        EXPECT_EQ(LoadedPath(effects), L"C:\\b.md");
        EXPECT_FALSE(HasEffect<effect::CancelOtherFileLoad>(effects));

        if (!succeed) {
            Reduce(state, LoadFailedAction{});
            EXPECT_EQ(state.view.nav_history.BackSize(), 1u);
            EXPECT_EQ(state.view.nav_history.ForwardSize(), 0u);
            EXPECT_FALSE(state.view.pending_restore.IsValid());
            continue;
        }
        CompleteLoad(state, L"C:\\b.md");
        EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), state.document.layout_cache.Top(2) + 10.0f);
        EXPECT_EQ(state.view.nav_history.BackSize(), 0u);
        EXPECT_EQ(state.view.nav_history.ForwardSize(), 1u);
        EXPECT_EQ(LoadedPath(Reduce(state, NavigateForwardAction{})), L"C:\\current.md");
    }
}

// 戻り先の無い状態で開く (ロード中) → 戻る: 開く分を取り消すと戻り先が無いので何もしない。
// 戻る履歴は開く分のまま残り、ロードに成功すれば元のファイルへ戻れる。
TEST_F(ReducerTest, NavigateBackWithoutHistoryWhileOpenLoading_KeepsOpenEntry)
{
    for (const bool succeed : { false, true }) {
        SCOPED_TRACE(succeed);
        ResetToDocument(state, L"C:\\current.md");
        Reduce(state, OpenPathAction{ std::wstring(L"C:\\big.md") });

        EXPECT_TRUE(Reduce(state, NavigateBackAction{}).empty());
        EXPECT_TRUE(state.view.nav_history.CanGoBack());

        if (!succeed) {
            Reduce(state, LoadFailedAction{});
            EXPECT_FALSE(state.view.nav_history.CanGoBack());
            continue;
        }
        CompleteLoad(state, L"C:\\big.md");
        EXPECT_EQ(LoadedPath(Reduce(state, NavigateBackAction{})), L"C:\\current.md");
    }
}

// ファイルをまたいで戻るを続けると、ロード中の移動先を起点にもう 1 段戻る。
// 失敗すると連続した戻るはまとめて取り消す。
TEST_F(ReducerTest, ConsecutiveNavigateBackAcrossFiles)
{
    for (const bool succeed : { false, true }) {
        SCOPED_TRACE(succeed);
        ResetToDocument(state, L"C:\\a.md");
        state.view.nav_history.Push(NavEntry{ L"C:\\c.md", 2, 0.0f });
        state.view.nav_history.Push(NavEntry{ L"C:\\b.md", 3, 10.0f });

        ASSERT_TRUE(HasEffect<effect::LoadFile>(Reduce(state, NavigateBackAction{})));
        EXPECT_EQ(LoadedPath(Reduce(state, NavigateBackAction{})), L"C:\\c.md");
        EXPECT_EQ(LoadedPath(Reduce(state, NavigateForwardAction{})), L"C:\\b.md");

        if (!succeed) {
            Reduce(state, LoadFailedAction{});
            EXPECT_EQ(state.view.nav_history.BackSize(), 2u);
            EXPECT_EQ(state.view.nav_history.ForwardSize(), 0u);
            continue;
        }
        CompleteLoad(state, L"C:\\b.md");
        EXPECT_FLOAT_EQ(state.view.viewport.GetScrollY(), state.document.layout_cache.Top(3) + 10.0f);
        EXPECT_EQ(state.view.nav_history.BackSize(), 1u);
        EXPECT_EQ(state.view.nav_history.ForwardSize(), 1u);
    }
}

// ---- Zoom / ToggleDarkMode: 可視位置の anchor 保持 ----
// 期待動作: 可視先頭ノードを SnapshotVisibleTarget で取り、ズーム/モード切替後に
// SetScrollTarget で同じノードへ戻れるようにする。これが落ちると、ズーム後に
// 全く別の場所までスクロールが飛ぶという可視的な不具合になる。

TEST_F(ReducerTest, ZoomIn_PreservesScrollAnchorOnVisibleNode)
{
    SetDocument(state, "# A\n\n# B\n\n# C", L"test.md");
    state.view.viewport.ScrollTo(120.0f, MdMaxScroll(state)); // node 1 が可視先頭

    const auto pre = SnapshotVisibleTarget(state);
    ASSERT_TRUE(pre.IsValid());

    Reduce(state, ZoomAction{ ZoomDirection::In });

    // ZoomIn 後、可視先頭ノードが scroll_target として保存されている
    ASSERT_TRUE(state.view.viewport.HasScrollTarget());
    EXPECT_EQ(state.view.viewport.GetScrollTarget().node, pre.node);
}

TEST_F(ReducerTest, ZoomReset_AtDefaultIndex_NoEffect)
{
    // ZoomReset は既にデフォルト位置なら 0 を返し、early return する。
    auto effects = Reduce(state, ZoomAction{ ZoomDirection::Reset });
    EXPECT_FALSE(HasEffect<effect::ApplyZoom>(effects));
}

TEST_F(ReducerTest, ToggleDarkMode_PreservesScrollAnchorOnVisibleNode)
{
    SetDocument(state, "# A\n\n# B\n\n# C", L"test.md");
    state.view.viewport.ScrollTo(120.0f, MdMaxScroll(state));

    const auto pre = SnapshotVisibleTarget(state);
    ASSERT_TRUE(pre.IsValid());

    Reduce(state, ToggleDarkModeAction{});

    ASSERT_TRUE(state.view.viewport.HasScrollTarget());
    EXPECT_EQ(state.view.viewport.GetScrollTarget().node, pre.node);
}
