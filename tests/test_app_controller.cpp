#include <gtest/gtest.h>
#include "app_controller.h"
#include "document_test_helpers.h"
#include "layout_computer.h"
#include "mermaid_util.h"
#include "reducer_harness.h"
#include "test_helpers.h"
#include "tooltip.h"
#include <optional>
#include <utility>
#include <windows.h>

namespace {

// 同じ alternative であることに加え、payload を持つものは中身まで比較する。nullopt は割り当ての無いキー。
void ExpectSameAction(const std::optional<AppAction>& actual_opt, const std::optional<AppAction>& expected_opt)
{
    ASSERT_EQ(actual_opt.has_value(), expected_opt.has_value());
    if (!expected_opt) {
        return;
    }
    const AppAction& actual = *actual_opt;
    const AppAction& expected = *expected_opt;
    ASSERT_EQ(actual.index(), expected.index());
    if (const auto* step = std::get_if<SearchStepAction>(&expected)) {
        EXPECT_EQ(std::get<SearchStepAction>(actual).forward, step->forward);
    }
    if (const auto* nav = std::get_if<NavigateHistoryAction>(&expected)) {
        EXPECT_EQ(std::get<NavigateHistoryAction>(actual).forward, nav->forward);
    }
    if (const auto* copy = std::get_if<CopyAction>(&expected)) {
        EXPECT_EQ(std::get<CopyAction>(actual).formatted, copy->formatted);
    }
    if (const auto* scroll = std::get_if<KeyScrollAction>(&expected)) {
        EXPECT_EQ(std::get<KeyScrollAction>(actual).type, scroll->type);
    }
    if (const auto* toggle = std::get_if<TogglePaneAction>(&expected)) {
        EXPECT_EQ(std::get<TogglePaneAction>(actual).target, toggle->target);
    }
    if (const auto* zoom = std::get_if<ZoomAction>(&expected)) {
        EXPECT_EQ(std::get<ZoomAction>(actual).direction, zoom->direction);
        EXPECT_EQ(std::get<ZoomAction>(actual).steps, zoom->steps);
        EXPECT_EQ(std::get<ZoomAction>(actual).pane, zoom->pane);
    }
}

struct KeyCase {
    const char* name;
    KeyDownEvent ev;
    std::optional<AppAction> expected;
};

} // namespace

TEST(AppControllerTest, HandleKeyDown)
{
    const KeyCase kCases[] = {
        { "Up", { .key = VK_UP }, KeyScrollAction{ ScrollType::LineUp } },
        { "Down", { .key = VK_DOWN }, KeyScrollAction{ ScrollType::LineDown } },
        { "PageUp", { .key = VK_PRIOR }, KeyScrollAction{ ScrollType::PageUp } },
        { "PageDown", { .key = VK_NEXT }, KeyScrollAction{ ScrollType::PageDown } },
        { "Home", { .key = VK_HOME }, KeyScrollAction{ ScrollType::Home } },
        { "End", { .key = VK_END }, KeyScrollAction{ ScrollType::End } },

        { "F1", { .key = VK_F1 }, ShowHelpAction{} },
        { "F5", { .key = VK_F5 }, ReloadFileAction{} },
        { "Escape", { .key = VK_ESCAPE }, ClearSelectionAction{} },

        { "Ctrl+C", { .key = 'C', .ctrl = true }, CopyAction{} },
        { "Ctrl+Shift+C", { .key = 'C', .ctrl = true, .shift = true }, CopyAction{ .formatted = true } },
        { "Ctrl+A", { .key = 'A', .ctrl = true }, SelectAllAction{} },
        { "Ctrl+O", { .key = 'O', .ctrl = true }, OpenFileAction{} },
        { "Ctrl+N", { .key = 'N', .ctrl = true }, NewWindowAction{} },
        { "Ctrl+N repeat", { .key = 'N', .ctrl = true, .repeat = true }, std::nullopt },
        { "N without Ctrl", { .key = 'N' }, std::nullopt },
        { "Ctrl+1", { .key = '1', .ctrl = true }, TogglePaneAction{ PaneTarget::File } },
        { "Ctrl+2", { .key = '2', .ctrl = true }, TogglePaneAction{ PaneTarget::Toc } },
        { "Ctrl+Plus", { .key = VK_OEM_PLUS, .ctrl = true }, ZoomAction{ ZoomDirection::In } },
        { "Ctrl+NumpadPlus", { .key = VK_ADD, .ctrl = true }, ZoomAction{ ZoomDirection::In } },
        { "Ctrl+Minus", { .key = VK_OEM_MINUS, .ctrl = true }, ZoomAction{ ZoomDirection::Out } },
        { "Ctrl+NumpadMinus", { .key = VK_SUBTRACT, .ctrl = true }, ZoomAction{ ZoomDirection::Out } },
        { "Ctrl+0", { .key = '0', .ctrl = true }, ZoomAction{ ZoomDirection::Reset } },
        { "Ctrl+Numpad0", { .key = VK_NUMPAD0, .ctrl = true }, ZoomAction{ ZoomDirection::Reset } },

        { "Z", { .key = 'Z' }, std::nullopt },
        { "C without Ctrl", { .key = 'C' }, std::nullopt },
        { "Ctrl+Z", { .key = 'Z', .ctrl = true }, std::nullopt },

        { "Alt+Left", { .key = VK_LEFT, .alt = true }, NavigateHistoryAction{ .forward = false } },
        { "Alt+Right", { .key = VK_RIGHT, .alt = true }, NavigateHistoryAction{ .forward = true } },
        { "Alt+Up", { .key = VK_UP, .alt = true }, std::nullopt },
        // Ctrl+Alt (AltGr) には何も割り当てない
        { "Ctrl+Alt+Left", { .key = VK_LEFT, .ctrl = true, .alt = true }, std::nullopt },
        { "AltGr+0", { .key = '0', .ctrl = true, .alt = true }, std::nullopt },
        { "AltGr+Plus", { .key = VK_OEM_PLUS, .ctrl = true, .alt = true }, std::nullopt },
        { "AltGr+F", { .key = 'F', .ctrl = true, .alt = true }, std::nullopt },

        { "F3", { .key = VK_F3 }, SearchStepAction{ .forward = true } },
        { "Shift+F3", { .key = VK_F3, .shift = true }, SearchStepAction{ .forward = false } },
        { "Ctrl+F", { .key = 'F', .ctrl = true }, OpenSearchBarAction{} },
        { "Ctrl+G", { .key = 'G', .ctrl = true }, SearchStepAction{ .forward = true } },
        { "Ctrl+Shift+G", { .key = 'G', .ctrl = true, .shift = true }, SearchStepAction{ .forward = false } },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        ExpectSameAction(app_controller::HandleKeyDown(c.ev), c.expected);
    }
}

// タイトルバーのボタンはキーと同じアクションにする。ウィンドウの操作と設定ダイアログは App が直接行う。
TEST(AppControllerTest, TitleBarClickAction)
{
    struct Case {
        TitleBarHitZone zone;
        std::optional<AppAction> expected;
    };
    const Case kCases[]{
        { TitleBarHitZone::OpenFile, OpenFileAction{} },
        { TitleBarHitZone::Help, ShowHelpAction{} },
        { TitleBarHitZone::Search, OpenSearchBarAction{} },
        { TitleBarHitZone::ThemeToggle, ToggleDarkModeAction{} },
        { TitleBarHitZone::FileToggle, TogglePaneAction{ PaneTarget::File } },
        { TitleBarHitZone::TocToggle, TogglePaneAction{ PaneTarget::Toc } },
        { TitleBarHitZone::Settings, std::nullopt },
        { TitleBarHitZone::Minimize, std::nullopt },
        { TitleBarHitZone::Maximize, std::nullopt },
        { TitleBarHitZone::Close, std::nullopt },
        { TitleBarHitZone::Caption, std::nullopt },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(static_cast<int>(c.zone));
        ExpectSameAction(app_controller::TitleBarClickAction(c.zone), c.expected);
    }
}

TEST(AppControllerTest, HandleSearchEditKeyDown)
{
    // nullopt は入力欄 (EDIT) に処理させるキー。
    const KeyCase kCases[] = {
        { "Enter", { .key = VK_RETURN }, SearchStepAction{ .forward = true } },
        { "Shift+Enter", { .key = VK_RETURN, .shift = true }, SearchStepAction{ .forward = false } },
        { "Ctrl+A", { .key = 'A', .ctrl = true }, SearchSelectAllAction{} },

        { "Left", { .key = VK_LEFT }, std::nullopt },
        { "Shift+Right", { .key = VK_RIGHT, .shift = true }, std::nullopt },
        { "Ctrl+Left", { .key = VK_LEFT, .ctrl = true }, std::nullopt },
        { "Home", { .key = VK_HOME }, std::nullopt },
        { "End", { .key = VK_END }, std::nullopt },
        { "Backspace", { .key = VK_BACK }, std::nullopt },
        { "Delete", { .key = VK_DELETE }, std::nullopt },
        { "Shift+Insert", { .key = VK_INSERT, .shift = true }, std::nullopt },
        { "Ctrl+C", { .key = 'C', .ctrl = true }, std::nullopt },
        { "Ctrl+Shift+C", { .key = 'C', .ctrl = true, .shift = true }, std::nullopt },
        { "Ctrl+V", { .key = 'V', .ctrl = true }, std::nullopt },
        { "Ctrl+X", { .key = 'X', .ctrl = true }, std::nullopt },
        { "Ctrl+Z", { .key = 'Z', .ctrl = true }, std::nullopt },
        { "Letter", { .key = 'Z' }, std::nullopt },
        { "IME", { .key = VK_PROCESSKEY }, std::nullopt },
        { "AltGr+0", { .key = '0', .ctrl = true, .alt = true }, std::nullopt },
        { "AltGr+Q", { .key = 'Q', .ctrl = true, .alt = true }, std::nullopt },

        // 本体と同じ割り当て。
        { "Escape", { .key = VK_ESCAPE }, ClearSelectionAction{} },
        { "Ctrl+F", { .key = 'F', .ctrl = true }, OpenSearchBarAction{} },
        { "F3", { .key = VK_F3 }, SearchStepAction{ .forward = true } },
        { "Ctrl+Shift+G", { .key = 'G', .ctrl = true, .shift = true }, SearchStepAction{ .forward = false } },
        { "F5", { .key = VK_F5 }, ReloadFileAction{} },
        { "Ctrl+O", { .key = 'O', .ctrl = true }, OpenFileAction{} },
        { "Ctrl+N", { .key = 'N', .ctrl = true }, NewWindowAction{} },
        { "Down", { .key = VK_DOWN }, KeyScrollAction{ ScrollType::LineDown } },
        { "PageUp", { .key = VK_PRIOR }, KeyScrollAction{ ScrollType::PageUp } },
        { "Alt+Left", { .key = VK_LEFT, .alt = true }, NavigateHistoryAction{ .forward = false } },
        { "Alt+Right", { .key = VK_RIGHT, .alt = true }, NavigateHistoryAction{ .forward = true } },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        ExpectSameAction(app_controller::HandleSearchEditKeyDown(c.ev), c.expected);
    }
}

// ============================================================
// ポインタの判定 (ResolvePointerHover / Resolve*Hit)
// ============================================================

namespace {

// 本番と同じく、タイトルバーの下にファイル・目次・本文の順で並べる。
class PointerHitTest : public ::testing::Test {
protected:
    static constexpr float kTop = TitleBar::BASE_HEIGHT;
    static constexpr float kPaneHeight = 400.0f;

    PointerHitTest() : h_({ .markdown = md_, .md_height = kPaneHeight }), layout_(ComputePaneLayout(h_.state)) {}

    app_controller::PointerHoverResult Hover(float x, float y) const
    {
        return app_controller::ResolvePointerHover(h_.state, layout_, static_cast<int>(x), static_cast<int>(y), false);
    }
    app_controller::MdPaneHit ResolveMd(float x, float y) const
    {
        const auto ctx = app_controller::MakeMdPaneHitContext(h_.state, layout_, { x, y });
        return app_controller::ResolveMdPaneHit(h_.state, ctx, layout_, x, y);
    }
    int FirstCodeBlock() const
    {
        return FindFirstNodeIndexByType(h_.state.document.doc.GetNodes(), NodeType::CodeBlock);
    }
    // 本文のノード i の中ほど (画面の DIP)。scroll_y は 0。
    float NodeMidY(int i) const
    {
        return layout_.md_rect.y + h_.state.document.layout_cache.Top(static_cast<size_t>(i)) + ReducerHarness::kNodeHeight / 2.0f;
    }

    std::string md_ = MakeHarnessMarkdown(20);
    ReducerHarness h_;
    PaneLayout layout_;
};

} // namespace

TEST_F(PointerHitTest, SplittersShowResizeCursor)
{
    EXPECT_EQ(Hover(152.0f, 200.0f).cursor, app_controller::PointerCursor::SizeWE);
    EXPECT_EQ(Hover(306.0f, 200.0f).cursor, app_controller::PointerCursor::SizeWE);
}

TEST_F(PointerHitTest, TocPaneHeaderButtonAndItem)
{
    const auto toc = std::to_underlying(PaneTarget::Toc);
    const float header_h = h_.theme.pane_header_height;
    const auto close = PaneHeaderButtonRect(layout_.toc_rect.width, header_h, PaneHeaderButton::Close);
    const auto [close_cx, close_cy] = RectCenter(close);
    const auto on_close = Hover(layout_.toc_rect.x + close_cx, kTop + close_cy);
    EXPECT_EQ(on_close.hover.side_button[toc], PaneHeaderButton::Close);
    EXPECT_EQ(on_close.cursor, app_controller::PointerCursor::Hand);
    EXPECT_EQ(on_close.hover.tooltip.target.zone, TooltipTarget::Zone::TocPaneButton);

    const auto on_item = Hover(layout_.toc_rect.x + 20.0f, kTop + header_h + h_.theme.pane_item_height * 1.5f);
    EXPECT_EQ(on_item.hover.side_index[toc], 1);
    EXPECT_EQ(on_item.hover.side_button[toc], PaneHeaderButton::None);
    EXPECT_EQ(on_item.cursor, app_controller::PointerCursor::Arrow);
    EXPECT_EQ(on_item.hover.tooltip.target, (TooltipTarget{ TooltipTarget::Zone::TocPaneItem, 1 }));
    EXPECT_FALSE(on_item.hover.tooltip.text.empty());
    // 他のペインのホバーは既定値のまま返して解除させる。
    EXPECT_EQ(on_item.hover.side_index[std::to_underlying(PaneTarget::File)], -1);
    EXPECT_EQ(on_item.hover.h_block, -1);
}

// クリックはヘッダー (ボタンの外も消費する)、スクロールバー、項目の順に見る。
TEST_F(PointerHitTest, SidePaneClickSeesScrollbarBeforeItem)
{
    const float header_h = h_.theme.pane_header_height;
    const float item_y = kTop + header_h + h_.theme.pane_item_height * 0.5f;
    const float right = layout_.toc_rect.x + layout_.toc_rect.width;
    ASSERT_TRUE(InVScrollbarHitX(right - 3.0f, right));

    const auto on_bar = app_controller::ResolveSidePaneHit(h_.state, PaneTarget::Toc, layout_, right - 3.0f, item_y);
    EXPECT_FALSE(on_bar.in_header);
    EXPECT_TRUE(on_bar.scrollbar) << "目次は 20 項目以上でペインに収まらない";
    EXPECT_EQ(on_bar.item, 0);

    const auto in_header = app_controller::ResolveSidePaneHit(h_.state, PaneTarget::Toc, layout_, layout_.toc_rect.x + 5.0f, kTop + 2.0f);
    EXPECT_TRUE(in_header.in_header);
    EXPECT_EQ(in_header.button, PaneHeaderButton::None);
    EXPECT_FALSE(in_header.scrollbar);
    EXPECT_EQ(in_header.item, -1);
}

// スクロールした一覧のヘッダーの上では、その下に隠れた項目に当たらない (ツールチップを出さない)。
TEST_F(PointerHitTest, ScrolledSidePaneHeaderHidesItems)
{
    const auto toc = std::to_underlying(PaneTarget::Toc);
    h_.state.view.panes.SidePaneScroll(PaneTarget::Toc) = h_.theme.pane_item_height * 5.0f;
    const auto on_header = Hover(layout_.toc_rect.x + 5.0f, kTop + 2.0f);
    EXPECT_EQ(on_header.hover.side_index[toc], -1);
    EXPECT_EQ(on_header.hover.side_button[toc], PaneHeaderButton::None);
    EXPECT_TRUE(on_header.hover.tooltip.text.empty());
}

// 横スクロールのドラッグ中は、ホバーで別のブロックのバーを出さない (ドラッグ中のバーだけを描く)。
TEST_F(PointerHitTest, BlockHScrollbarHiddenWhileDragging)
{
    const int code = FirstCodeBlock();
    ASSERT_GE(code, 0);
    const float x = layout_.md_rect.x + 100.0f;
    EXPECT_EQ(Hover(x, NodeMidY(code)).hover.h_block, code);
    EXPECT_EQ(Hover(x, NodeMidY(code)).cursor, app_controller::PointerCursor::IBeam);

    h_.state.view.drag.kind = LeftDrag::BlockHScroll;
    h_.state.view.drag.block_node = code;
    EXPECT_EQ(Hover(x, NodeMidY(code)).hover.h_block, -1);
}

// 縮小表示で右の余白が狭いと、コピーボタンが本文のスクロールバーのヒット余白に掛かる。
// ボタンを優先しないと、押せるように見えるボタンでスクロールバーのドラッグが始まる。
TEST_F(PointerHitTest, OverlayButtonWinsOverScrollbarHitPadding)
{
    h_.theme.margin_right = 0.0f;
    const int code = FirstCodeBlock();
    ASSERT_GE(code, 0);
    const auto& node = h_.state.document.doc.GetNodes()[code];
    const auto& theme = h_.theme;
    const float top = h_.state.document.layout_cache.Top(static_cast<size_t>(code));
    D2D1_RECT_F button{};
    layout::ForEachOverlayButton(node, h_.state.document.layout_cache, static_cast<size_t>(code), layout::NodeBlockX(node, theme),
                                 layout::NodeContentWidth(node, theme, theme.ContentWidth(layout_.md_rect.width)), top, theme,
                                 [&](const D2D1_RECT_F& r, OverlayButton) { button = r; });
    const float md_right = layout_.md_rect.x + layout_.md_rect.width;
    // ボタンの中で、スクロールバーのヒット余白にも入る x。
    std::optional<float> x;
    for (float lx = button.left + 1.0f; lx < button.right; lx += 1.0f) {
        if (InVScrollbarHitX(layout_.md_rect.x + lx, md_right)) {
            x = layout_.md_rect.x + lx;
            break;
        }
    }
    ASSERT_TRUE(x.has_value()) << "ボタンがヒット余白に掛からない配置になっている";
    const float button_y = layout_.md_rect.y + RectCenter(button).y;

    EXPECT_EQ(ResolveMd(*x, button_y).kind, app_controller::MdPaneHit::Kind::OverlayButton);
    const auto hover = Hover(*x, button_y);
    EXPECT_EQ(hover.hover.overlay_button.kind, OverlayButton::CodeCopy);
    EXPECT_EQ(hover.cursor, app_controller::PointerCursor::Hand);
    EXPECT_EQ(hover.hover.tooltip.target.zone, TooltipTarget::Zone::OverlayButton);

    // ボタンの外なら、同じ x はスクロールバー。
    EXPECT_EQ(ResolveMd(*x, NodeMidY(code + 1)).kind, app_controller::MdPaneHit::Kind::Scrollbar);
    EXPECT_EQ(Hover(*x, NodeMidY(code + 1)).cursor, app_controller::PointerCursor::Arrow);
}

// 細い図ではボタンが画像の左へはみ出す。画像の外のボタンの上でも図のボタンを出したままにしないと、
// 消えたボタンの場所で手のカーソルとツールチップが出る。
TEST(PointerHover, DiagramButtonOutsideImageKeepsButtonsShown)
{
    ReducerHarness h({ .markdown = "```mermaid\ngraph TD; A\n```\n" });
    const PaneLayout layout = ComputePaneLayout(h.state);
    const int index = FindFirstNodeIndexByType(h.state.document.doc.GetNodes(), NodeType::Diagram);
    ASSERT_GE(index, 0);
    auto& cache = h.state.document.layout_cache;
    auto& diagram = cache.EnsureDiagram(static_cast<size_t>(index));
    diagram.width = 40.0f;
    diagram.height = 40.0f;
    diagram.bitmap = &SharedStubBitmap();
    diagram.drawing = std::make_shared<const mermaid_util::DiagramDrawing>();

    const auto& node = h.state.document.doc.GetNodes()[static_cast<size_t>(index)];
    const auto& theme = h.theme;
    const float x = layout::NodeBlockX(node, theme);
    const float w = layout::NodeContentWidth(node, theme, theme.ContentWidth(layout.md_rect.width));
    const float top = cache.Top(static_cast<size_t>(index));
    D2D1_RECT_F button{};
    layout::ForEachOverlayButton(node, cache, static_cast<size_t>(index), x, w, top, theme, [&](const D2D1_RECT_F& r, OverlayButton kind) {
        if (kind == OverlayButton::DiagramView) {
            button = r;
        }
    });
    ASSERT_LT(button.left, DiagramRect(diagram.width, diagram.height, x, w, top).left);

    const auto hover = app_controller::ResolvePointerHover(h.state, layout, static_cast<int>(layout.md_rect.x + button.left + 1.0f),
                                                           static_cast<int>(layout.md_rect.y + button.top + 1.0f), false);
    EXPECT_EQ(hover.hover.overlay_button.kind, OverlayButton::DiagramView);
    EXPECT_EQ(hover.hover.diagram, index);
}

TEST_F(PointerHitTest, NavButtonsComeBeforeContent)
{
    h_.state.view.nav_history.Push({ .file_path = L"prev.md" });
    const auto [x, y] = RectCenter(NavBackButtonRect(layout_.md_rect));
    EXPECT_EQ(ResolveMd(x, y).kind, app_controller::MdPaneHit::Kind::NavButton);
    const auto hover = Hover(x, y);
    EXPECT_EQ(hover.hover.nav, NavDirection::Back);
    EXPECT_EQ(hover.cursor, app_controller::PointerCursor::Hand);
}

// 履歴が空ならナビボタンは描かないので、その位置は本文として扱う。
TEST_F(PointerHitTest, NavButtonsIgnoredWithoutHistory)
{
    const auto [x, y] = RectCenter(NavBackButtonRect(layout_.md_rect));
    EXPECT_EQ(ResolveMd(x, y).kind, app_controller::MdPaneHit::Kind::Content);
    EXPECT_EQ(Hover(x, y).hover.nav, NavDirection::None);
}

// スピナーを出している間の本文ペインは何も描かないので、リンク・ボタン・スクロールバーのどれにも当たらない。
TEST_F(PointerHitTest, LoadingMdPaneHitsNothing)
{
    h_.state.view.nav_history.Push({ .file_path = L"prev.md" });
    h_.state.view.loading_shown = true;
    const auto [nav_x, nav_y] = RectCenter(NavBackButtonRect(layout_.md_rect));
    EXPECT_EQ(ResolveMd(nav_x, nav_y).kind, app_controller::MdPaneHit::Kind::None);
    const auto hover = Hover(nav_x, nav_y);
    EXPECT_EQ(hover.hover, HoverHighlights{});
    EXPECT_TRUE(hover.hover.tooltip.target.IsEmpty());
    EXPECT_EQ(hover.cursor, app_controller::PointerCursor::Arrow);

    const float right_edge = layout_.md_rect.x + layout_.md_rect.width - 2.0f;
    EXPECT_FALSE(app_controller::IsOverMdScrollbar(h_.state, layout_, right_edge, layout_.md_rect.y + 10.0f));
}

// ---- TooltipTarget ----

// 空かどうかは zone だけで決まり、key は見ない。既定は空。
TEST(TooltipTarget, IsEmptyFollowsZoneOnly)
{
    EXPECT_EQ(TooltipTarget{}.zone, TooltipTarget::Zone::None);
    EXPECT_TRUE(TooltipTarget{}.IsEmpty());
    EXPECT_FALSE((TooltipTarget{ TooltipTarget::Zone::OverlayButton, 0 }).IsEmpty());
    EXPECT_TRUE((TooltipTarget{ TooltipTarget::Zone::None, 5 }).IsEmpty());
}

// ホバー移動ごとに表示文字列を作らないため、同じ対象なら文言を作らない (同一判定は zone と key だけ)。
TEST(TooltipTarget, MakeTooltipBuildsTextOnlyWhenTargetChanges)
{
    const TooltipHover current = MakeTooltip({}, TooltipTarget::Zone::NavButton, 1, L"back");
    EXPECT_EQ(current.text, L"back");

    int fills = 0;
    const auto same = MakeTooltip(current.target, TooltipTarget::Zone::NavButton, 1, [&](std::wstring&) { ++fills; });
    EXPECT_EQ(fills, 0);
    EXPECT_EQ(same.target, current.target);
    EXPECT_TRUE(same.text.empty());

    const auto other = MakeTooltip(current.target, TooltipTarget::Zone::NavButton, 2, L"forward");
    EXPECT_EQ(other.text, L"forward");
    // 該当ボタンが無いときは文言が空になるので、ゾーンを残さず非表示にする。
    EXPECT_TRUE(MakeTooltip({}, TooltipTarget::Zone::NavButton, 0, L"").target.IsEmpty());
}
