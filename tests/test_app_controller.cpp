#include <gtest/gtest.h>
#include "app_controller.h"
#include "document_test_helpers.h"
#include "layout_computer.h"
#include "reducer_harness.h"
#include <optional>
#include <utility>
#include <windows.h>

namespace {

// 同じ alternative であることに加え、payload を持つものは中身まで比較する。
void ExpectSameAction(const AppAction& actual, const AppAction& expected)
{
    ASSERT_EQ(actual.index(), expected.index());
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
    AppAction expected;
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

        { "Ctrl+C", { .key = 'C', .ctrl = true }, CopyClipboardAction{} },
        { "Ctrl+Shift+C", { .key = 'C', .ctrl = true, .shift = true }, CopyFormattedClipboardAction{} },
        { "Ctrl+A", { .key = 'A', .ctrl = true }, SelectAllAction{} },
        { "Ctrl+O", { .key = 'O', .ctrl = true }, OpenFileAction{} },
        { "Ctrl+1", { .key = '1', .ctrl = true }, TogglePaneAction{ PaneTarget::File } },
        { "Ctrl+2", { .key = '2', .ctrl = true }, TogglePaneAction{ PaneTarget::Toc } },
        { "Ctrl+Plus", { .key = VK_OEM_PLUS, .ctrl = true }, ZoomAction{ ZoomDirection::In } },
        { "Ctrl+NumpadPlus", { .key = VK_ADD, .ctrl = true }, ZoomAction{ ZoomDirection::In } },
        { "Ctrl+Minus", { .key = VK_OEM_MINUS, .ctrl = true }, ZoomAction{ ZoomDirection::Out } },
        { "Ctrl+NumpadMinus", { .key = VK_SUBTRACT, .ctrl = true }, ZoomAction{ ZoomDirection::Out } },
        { "Ctrl+0", { .key = '0', .ctrl = true }, ZoomAction{ ZoomDirection::Reset } },
        { "Ctrl+Numpad0", { .key = VK_NUMPAD0, .ctrl = true }, ZoomAction{ ZoomDirection::Reset } },

        { "Z", { .key = 'Z' }, NoOpAction{} },
        { "C without Ctrl", { .key = 'C' }, NoOpAction{} },
        { "Ctrl+Z", { .key = 'Z', .ctrl = true }, NoOpAction{} },

        { "Alt+Left", { .key = VK_LEFT, .alt = true }, NavigateBackAction{} },
        { "Alt+Right", { .key = VK_RIGHT, .alt = true }, NavigateForwardAction{} },
        { "Alt+Up", { .key = VK_UP, .alt = true }, NoOpAction{} },
        // Ctrl+Alt ではナビゲーションを発動しない
        { "Ctrl+Alt+Left", { .key = VK_LEFT, .ctrl = true, .alt = true }, NoOpAction{} },

        { "F3", { .key = VK_F3 }, SearchNextAction{} },
        { "Shift+F3", { .key = VK_F3, .shift = true }, SearchPrevAction{} },
        { "Ctrl+F", { .key = 'F', .ctrl = true }, OpenSearchBarAction{} },
        { "Ctrl+G", { .key = 'G', .ctrl = true }, SearchNextAction{} },
        { "Ctrl+Shift+G", { .key = 'G', .ctrl = true, .shift = true }, SearchPrevAction{} },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        ExpectSameAction(app_controller::HandleKeyDown(c.ev), c.expected);
    }
}

TEST(AppControllerTest, HandleSearchEditKeyDown)
{
    // NoOpAction は入力欄 (EDIT) に処理させるキー。
    const KeyCase kCases[] = {
        { "Enter", { .key = VK_RETURN }, SearchNextAction{} },
        { "Shift+Enter", { .key = VK_RETURN, .shift = true }, SearchPrevAction{} },
        { "Ctrl+A", { .key = 'A', .ctrl = true }, SearchSelectAllAction{} },

        { "Left", { .key = VK_LEFT }, NoOpAction{} },
        { "Shift+Right", { .key = VK_RIGHT, .shift = true }, NoOpAction{} },
        { "Ctrl+Left", { .key = VK_LEFT, .ctrl = true }, NoOpAction{} },
        { "Home", { .key = VK_HOME }, NoOpAction{} },
        { "End", { .key = VK_END }, NoOpAction{} },
        { "Backspace", { .key = VK_BACK }, NoOpAction{} },
        { "Delete", { .key = VK_DELETE }, NoOpAction{} },
        { "Shift+Insert", { .key = VK_INSERT, .shift = true }, NoOpAction{} },
        { "Ctrl+C", { .key = 'C', .ctrl = true }, NoOpAction{} },
        { "Ctrl+Shift+C", { .key = 'C', .ctrl = true, .shift = true }, NoOpAction{} },
        { "Ctrl+V", { .key = 'V', .ctrl = true }, NoOpAction{} },
        { "Ctrl+X", { .key = 'X', .ctrl = true }, NoOpAction{} },
        { "Ctrl+Z", { .key = 'Z', .ctrl = true }, NoOpAction{} },
        { "Letter", { .key = 'Z' }, NoOpAction{} },
        { "IME", { .key = VK_PROCESSKEY }, NoOpAction{} },

        // 本体と同じ割り当て。
        { "Escape", { .key = VK_ESCAPE }, ClearSelectionAction{} },
        { "Ctrl+F", { .key = 'F', .ctrl = true }, OpenSearchBarAction{} },
        { "F3", { .key = VK_F3 }, SearchNextAction{} },
        { "Ctrl+Shift+G", { .key = 'G', .ctrl = true, .shift = true }, SearchPrevAction{} },
        { "F5", { .key = VK_F5 }, ReloadFileAction{} },
        { "Ctrl+O", { .key = 'O', .ctrl = true }, OpenFileAction{} },
        { "Down", { .key = VK_DOWN }, KeyScrollAction{ ScrollType::LineDown } },
        { "PageUp", { .key = VK_PRIOR }, KeyScrollAction{ ScrollType::PageUp } },
        { "Alt+Left", { .key = VK_LEFT, .alt = true }, NavigateBackAction{} },
        { "Alt+Right", { .key = VK_RIGHT, .alt = true }, NavigateForwardAction{} },
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

    PointerHitTest() : h_({ .markdown = md_, .md_height = kPaneHeight })
    {
        layout_.file_rect = { 0.0f, kTop, 150.0f, kPaneHeight };
        layout_.toc_rect = { 154.0f, kTop, 150.0f, kPaneHeight };
        layout_.md_rect = { 308.0f, kTop, ReducerHarness::kMdWidth, kPaneHeight };
        layout_.show_file = true;
        layout_.show_toc = true;
        h_.state.pane_layout_cache.Set(layout_);
    }

    app_controller::PointerHoverResult Hover(float x, float y) const
    {
        return app_controller::ResolvePointerHover(h_.state, layout_, static_cast<int>(x), static_cast<int>(y), false);
    }
    app_controller::MdPaneHit ResolveMd(float x, float y) const
    {
        const auto ctx = app_controller::MakeMdPaneHitContext(h_.state, layout_, static_cast<int>(x), static_cast<int>(y));
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
    PaneLayout layout_{};
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
    const auto close = PaneCloseButtonRect(layout_.toc_rect.width, header_h);
    const auto on_close = Hover(layout_.toc_rect.x + (close.left + close.right) / 2.0f, kTop + (close.top + close.bottom) / 2.0f);
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
    layout::ForEachOverlayButton(node, nullptr, layout::NodeBlockX(node, theme), layout::NodeContentWidth(node, theme, theme.ContentWidth(layout_.md_rect.width)), top, theme,
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
    const float button_y = layout_.md_rect.y + (button.top + button.bottom) / 2.0f;

    EXPECT_EQ(ResolveMd(*x, button_y).kind, app_controller::MdPaneHit::Kind::OverlayButton);
    const auto hover = Hover(*x, button_y);
    EXPECT_EQ(hover.hover.overlay_button.kind, OverlayButton::CodeCopy);
    EXPECT_EQ(hover.cursor, app_controller::PointerCursor::Hand);
    EXPECT_EQ(hover.hover.tooltip.target.zone, TooltipTarget::Zone::OverlayButton);

    // ボタンの外なら、同じ x はスクロールバー。
    EXPECT_EQ(ResolveMd(*x, NodeMidY(code + 1)).kind, app_controller::MdPaneHit::Kind::Scrollbar);
    EXPECT_EQ(Hover(*x, NodeMidY(code + 1)).cursor, app_controller::PointerCursor::Arrow);
}

TEST_F(PointerHitTest, NavButtonsComeBeforeContent)
{
    const auto back = NavBackButtonRect(layout_.md_rect);
    const float x = (back.left + back.right) / 2.0f;
    const float y = (back.top + back.bottom) / 2.0f;
    EXPECT_EQ(ResolveMd(x, y).kind, app_controller::MdPaneHit::Kind::NavButton);
    const auto hover = Hover(x, y);
    EXPECT_EQ(hover.hover.nav, NavDirection::Back);
    EXPECT_EQ(hover.cursor, app_controller::PointerCursor::Hand);
}
