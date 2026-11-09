// 本文のリンクを指している間、本文ペインの下端に出す行き先 (issue #433)。
// リンクに当てるのと文字列の幅を測るのに実 DirectWrite が要る。
#include <gtest/gtest.h>
#include "dwrite_test_base.h"
#include "app_controller.h"
#include "command_generator.h"
#include "draw_cmd_test_util.h"
#include "reducer_harness.h"
#include "tooltip.h"
#include "ui_constants.h"
#include <string>
#include <string_view>

namespace {

using app_controller::PointerCursor;

class LinkHoverTest : public DWriteTestBase {
protected:
    // 既定の幅のサイドペイン 2 枚の右に、幅 600 の本文。
    static constexpr float kWindowWidth = 1048.0f;

    void Load(std::string_view md)
    {
        state_.theme = &theme_;
        state_.document.doc = Document::FromMarkdown(std::string(md), L"C:\\docs\\a.md");
        SetPaneHeight(400.0f);
        const auto& nodes = state_.document.doc.GetNodes();
        state_.document.layout_cache = LayoutCache(nodes.size());
        ComputeLayout(engine_, nodes, state_.document.layout_cache, layout_.md_rect.width);
    }

    void SetPaneHeight(float height)
    {
        SetWindowSize(state_, kWindowWidth, height);
        layout_ = ComputePaneLayout(state_);
    }

    // 先頭の段落の UTF-16 位置 pos の文字の中ほど (窓の DIP)。スクロールしていない。
    DipPoint CharCenter(uint32_t pos) const
    {
        const auto [x, y] = PointInChar(state_.document.doc.GetNodes(), state_.document.layout_cache, 0, pos, 0.5f);
        return { layout_.md_rect.x + x, layout_.md_rect.y + y };
    }

    app_controller::PointerHoverResult HoverAt(DipPoint p) const
    {
        return app_controller::ResolvePointerHover(state_, layout_, static_cast<int>(p.x), static_cast<int>(p.y), false);
    }

    AppState state_;
    PaneLayout layout_;
};

} // namespace

// 押したら開くものを出し、ツールチップ (書いたままの URL) も出す。押しても何も起きないリンクは手の形のカーソルにしない。
TEST_F(LinkHoverTest, ShowsDestinationAndCursorPerLink)
{
    // 本文の文字列は "ext bad doc"。
    Load("[ext](https://example.com) [bad](run.exe) [doc](sub/b.md#x)");
    struct Case {
        uint32_t pos;
        uint32_t index;
        std::wstring_view status;
        PointerCursor cursor;
    };
    const Case kCases[] = {
        { 1, 0, L"https://example.com", PointerCursor::Hand },
        { 5, 1, L"run.exe", PointerCursor::IBeam },
        { 9, 2, L"C:\\docs\\sub\\b.md#x", PointerCursor::Hand },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.pos);
        const auto r = HoverAt(CharCenter(c.pos));
        EXPECT_EQ(r.hover.link.node, 0);
        EXPECT_EQ(r.hover.link.index, c.index);
        EXPECT_EQ(r.hover.link.dock, LinkStatusDock::Left);
        EXPECT_EQ(r.hover.link.StatusText(), c.status);
        EXPECT_EQ(r.cursor, c.cursor);
        EXPECT_EQ(r.hover.tooltip.target.zone, TooltipTarget::Zone::MdLink);
    }

    // リンクの外では出さない。
    const auto off = HoverAt(CharCenter(3));
    EXPECT_FALSE(off.hover.link.Any());
    EXPECT_EQ(off.cursor, PointerCursor::IBeam);
}

// ツールチップの URL も、見えない文字を %XX にして出す。
TEST_F(LinkHoverTest, TooltipEncodesInvisibleChars)
{
    Load("[ext](https://evil.example/\xE2\x80\xAEgpj.moc)");
    EXPECT_EQ(HoverAt(CharCenter(1)).hover.tooltip.text, L"https://evil.example/%E2%80%AEgpj.moc");
}

// 同じリンクの上を動いている間は行き先を求め直さず、前のものを共有する。
TEST_F(LinkHoverTest, SameLinkReusesDestination)
{
    Load("[ext](https://example.com) [bad](run.exe)");
    for (const uint32_t pos : { 1u, 5u }) {
        SCOPED_TRACE(pos);
        const auto first = HoverAt(CharCenter(pos));
        Reduce(state_, first.hover);
        const auto again = HoverAt(CharCenter(pos));
        EXPECT_EQ(again.hover.link, first.hover.link);
        EXPECT_EQ(again.cursor, first.cursor);
    }
}

// ポインタが表示と同じ高さにあるときは、ポインタと反対の半分に寄せて、指しているリンクを隠さない。
TEST_F(LinkHoverTest, DocksAwayFromPointerInBand)
{
    // 本文の文字列は "l" + 空白 + a×40 + 空白 + "r"。"r" は本文の右半分にある。
    Load("[l](https://l.example) " + std::string(40, 'a') + " [r](https://r.example)");
    const float line_y = CharCenter(0).y - layout_.md_rect.y;
    SetPaneHeight(line_y + LINK_STATUS_HEIGHT * 0.5f);
    const auto band = LinkStatusBand(layout_.md_rect, false);
    const DipPoint left = CharCenter(0);
    const DipPoint right = CharCenter(43);
    ASSERT_GE(left.y, band.top);
    ASSERT_GT(right.x, RectCenter(band).x);

    EXPECT_EQ(HoverAt(left).hover.link.dock, LinkStatusDock::RightHalf);
    EXPECT_EQ(HoverAt(right).hover.link.dock, LinkStatusDock::LeftHalf);
    EXPECT_EQ(HoverAt(right).hover.link.index, 1u);

    // 帯より上なら左に寄せる。
    SetPaneHeight(400.0f);
    EXPECT_EQ(HoverAt(CharCenter(0)).hover.link.dock, LinkStatusDock::Left);
}

// ============================================================
// 描画 (overlay::EmitLinkStatus)
// ============================================================

namespace {

constexpr PaneRect kPane{ 100.0f, 50.0f, 600.0f, 400.0f };

class LinkStatusDrawTest : public DWriteTestBase {
protected:
    void SetUp() override
    {
        DWriteTestBase::SetUp();
        fmt_ = CreateTextFormat(dwrite_factory_.Get(), { .family = theme_.font_family.c_str(),
                                                         .size = theme_.pane_font_size,
                                                         .para_align = DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                                                         .no_wrap = true,
                                                         .ellipsis = true,
                                                         .ellipsis_keep_after = L'\\' });
        ASSERT_NE(fmt_, nullptr);
        text_layouts_.SetFactory(dwrite_factory_.Get());
    }

    DrawCommandList Emit(LinkStatusDock dock, std::wstring_view text)
    {
        DrawCommandList cmds;
        overlay::EmitLinkStatus({ cmds, text_layouts_ }, fmt_.Get(), band_, dock, text);
        return cmds;
    }

    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_;
    TextLayoutCache text_layouts_;
    D2D1_RECT_F band_ = LinkStatusBand(kPane, false);
};

} // namespace

// 帯は本文ペインの下端 (検索バーを出していればその上) で、ナビボタンに掛からない。
TEST(LinkStatusBandTest, SitsAtPaneBottomLeftOfNavButtons)
{
    const auto band = LinkStatusBand(kPane, false);
    EXPECT_FLOAT_EQ(band.left, kPane.x);
    EXPECT_FLOAT_EQ(band.bottom, kPane.y + kPane.height);
    EXPECT_FLOAT_EQ(band.bottom - band.top, LINK_STATUS_HEIGHT);
    EXPECT_LT(band.right, NavBackButtonRect(kPane).left);

    const auto above_search = LinkStatusBand(kPane, true);
    EXPECT_FLOAT_EQ(above_search.bottom, ComputeSearchBarLayout(kPane, false).bar_rect.top);
}

TEST_F(LinkStatusDrawTest, EmptyTextDrawsNothing)
{
    EXPECT_TRUE(Emit(LinkStatusDock::Left, L"").empty());
}

// 左に寄せたときは左端と下端をペインの縁に付け、幅は文字列に合わせる。
TEST_F(LinkStatusDrawTest, LeftDockHugsPaneCorner)
{
    const auto cmds = Emit(LinkStatusDock::Left, L"https://example.com");
    const auto clip = FindFirst<PushClipCmd>(cmds);
    const auto bg = FindFirst<FillRoundedRectCmd>(cmds);
    const auto text = FindFirst<DrawTextLayoutCmd>(cmds);
    ASSERT_TRUE(clip && bg && text);
    EXPECT_FLOAT_EQ(clip->rect.left, band_.left);
    EXPECT_FLOAT_EQ(clip->rect.top, band_.top);
    EXPECT_FLOAT_EQ(clip->rect.bottom, band_.bottom);
    EXPECT_LT(bg->rect.left, clip->rect.left) << "左の角は丸めずに切り落とす";
    EXPECT_GT(bg->rect.bottom, clip->rect.bottom) << "下の角は丸めずに切り落とす";
    EXPECT_EQ(bg->brush_id, BrushId::PaneBg);
    EXPECT_FLOAT_EQ(text->origin.x, band_.left + LINK_STATUS_PADDING_X);

    DWRITE_TEXT_METRICS m{};
    text->layout->GetMetrics(&m);
    EXPECT_NEAR(clip->rect.right - clip->rect.left, m.width + LINK_STATUS_PADDING_X * 2.0f, 1e-3f);
}

// 半分に寄せたときは、反対の半分に掛からない。右の半分は帯の右端に付ける。
TEST_F(LinkStatusDrawTest, HalfDocksStayInTheirHalf)
{
    const float mid = RectCenter(band_).x;
    const std::wstring long_url = L"https://example.com/" + std::wstring(200, L'x');

    const auto left = FindFirst<PushClipCmd>(Emit(LinkStatusDock::LeftHalf, long_url));
    ASSERT_TRUE(left);
    EXPECT_FLOAT_EQ(left->rect.left, band_.left);
    EXPECT_LE(left->rect.right, mid);

    const auto right = FindFirst<PushClipCmd>(Emit(LinkStatusDock::RightHalf, long_url));
    ASSERT_TRUE(right);
    EXPECT_FLOAT_EQ(right->rect.right, band_.right);
    EXPECT_GE(right->rect.left, mid);

    const auto short_right = FindFirst<PushClipCmd>(Emit(LinkStatusDock::RightHalf, L"#a"));
    ASSERT_TRUE(short_right);
    EXPECT_FLOAT_EQ(short_right->rect.right, band_.right);
    EXPECT_GT(short_right->rect.left, mid) << "短い文字列は幅を合わせて右に寄る";
}

// 左右の半分と、位置だけが違うペインでは、同じ文字レイアウトを使い回す (寄せる側が変わるたびに作り直さない)。
TEST_F(LinkStatusDrawTest, SharesLayoutAcrossHalvesAndPanePosition)
{
    const std::wstring long_url = L"https://example.com/" + std::wstring(200, L'x');
    const auto layout_of = [&](LinkStatusDock dock) -> IDWriteTextLayout* {
        const auto text = FindFirst<DrawTextLayoutCmd>(Emit(dock, long_url));
        return text ? text->layout : nullptr;
    };
    IDWriteTextLayout* const left = layout_of(LinkStatusDock::LeftHalf);
    ASSERT_NE(left, nullptr);
    EXPECT_EQ(layout_of(LinkStatusDock::RightHalf), left);
    band_ = LinkStatusBand({ kPane.x + 33.3f, kPane.y, kPane.width, kPane.height }, false);
    EXPECT_EQ(layout_of(LinkStatusDock::LeftHalf), left);
}

// 収まらない長いパスは、ファイル名を残して手前を省く。
TEST_F(LinkStatusDrawTest, LongPathKeepsFileName)
{
    std::wstring path = L"C:\\";
    for (int i = 0; i < 40; ++i) {
        path += L"folder\\";
    }
    path += L"file.md";
    const auto cmds = Emit(LinkStatusDock::LeftHalf, path);
    const auto text = FindFirst<DrawTextLayoutCmd>(cmds);
    ASSERT_TRUE(text);
    const auto trimmed = [&](uint32_t pos) {
        FLOAT x = 0.0f;
        FLOAT y = 0.0f;
        DWRITE_HIT_TEST_METRICS m{};
        text->layout->HitTestTextPosition(pos, FALSE, &x, &y, &m);
        return m.isTrimmed != FALSE;
    };
    EXPECT_FALSE(trimmed(0));
    EXPECT_TRUE(trimmed(static_cast<uint32_t>(path.size() / 2)));
    EXPECT_FALSE(trimmed(static_cast<uint32_t>(path.size() - 1))) << "ファイル名は見える";
}
