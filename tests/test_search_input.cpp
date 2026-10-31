// 検索欄の文字列 (MeasureSearchInput / HitTestSearchInput) と、検索バーに積む文字列。レイアウトを作るのに実 DirectWrite が要る。
#include <gtest/gtest.h>
#include "dwrite_test_base.h"
#include "command_generator.h"
#include "draw_cmd_test_util.h"
#include "render_params.h"
#include "test_helpers.h"
#include "ui_constants.h"
#include <algorithm>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

constexpr PaneRect kPane{ 100.0f, 50.0f, 600.0f, 400.0f };
constexpr float kWidth = 300.0f;

class SearchInputTest : public DWriteTestBase {
protected:
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_;
    TextLayoutCache text_layouts_;

    void SetUp() override
    {
        DWriteTestBase::SetUp();
        fmt_ = CreateThemeFormat(SEARCH_INPUT_FONT_SIZE);
        ASSERT_NE(fmt_, nullptr);
        text_layouts_.SetFactory(dwrite_factory_.Get());
    }

    SearchInputMetrics Measure(const SearchBarRenderState& sb, float width)
    {
        return MeasureSearchInput(text_layouts_, fmt_.Get(), sb, width);
    }
    int HitTest(std::wstring_view query, float local_x, float width)
    {
        return HitTestSearchInput(text_layouts_, fmt_.Get(), query, local_x, width);
    }

    // text_pos の前に置くキャレットの x。
    static float XOf(IDWriteTextLayout* layout, int text_pos)
    {
        FLOAT x = 0.0f;
        FLOAT y = 0.0f;
        DWRITE_HIT_TEST_METRICS m{};
        layout->HitTestTextPosition(static_cast<UINT32>(text_pos), FALSE, &x, &y, &m);
        return x;
    }

    // 検索バーに積んだ文字列。書式は件数と "Aa" の分だけ渡す。
    std::vector<std::wstring> BarTexts(const SearchBarRenderState& sb)
    {
        DrawCommandList cmds;
        overlay::EmitSearchBar(cmds, text_layouts_, nullptr, fmt_.Get(), ComputeSearchBarLayout(kPane, !sb.query.empty()), sb, {});
        return TextsOf(cmds);
    }

    std::vector<std::wstring> TextsOf(const DrawCommandList& cmds) const
    {
        std::vector<std::wstring> texts;
        for (const auto& t : CollectCmds<DrawTextLayoutCmd>(cmds)) {
            texts.emplace_back(text_layouts_.TextOf(t.layout));
        }
        return texts;
    }
};

} // namespace

// 文字列が無いとき・書式が無いときは測らない。
TEST_F(SearchInputTest, MeasuresNothingWithoutTextOrFormat)
{
    EXPECT_EQ(Measure({}, kWidth).layout, nullptr);
    EXPECT_EQ(MeasureSearchInput(text_layouts_, nullptr, { .query = L"abc" }, kWidth).layout, nullptr);
    EXPECT_EQ(HitTestSearchInput(text_layouts_, nullptr, L"abc", 100.0f, kWidth), 0);
}

// キャレットは caret_pos の前、-1 なら末尾。
TEST_F(SearchInputTest, CaretSitsBeforeCaretPos)
{
    const auto end = Measure({ .query = L"abc" }, kWidth);
    ASSERT_NE(end.layout, nullptr);
    EXPECT_GT(end.caret_x, 0.0f);
    EXPECT_FLOAT_EQ(end.caret_x, XOf(end.layout, 3));

    const auto mid = Measure({ .query = L"abc", .caret_pos = 1 }, kWidth);
    EXPECT_FLOAT_EQ(mid.caret_x, XOf(mid.layout, 1));
    EXPECT_FLOAT_EQ(Measure({ .query = L"abc", .caret_pos = 0 }, kWidth).caret_x, 0.0f);
}

// 選択範囲は起点とキャレットの間。向きによらない。
TEST_F(SearchInputTest, SelectionSpansAnchorToCaret)
{
    for (const auto [anchor, caret] : {
             std::pair{ 1, 3 },
             std::pair{ 3, 1 }
    }) {
        const auto m = Measure({ .query = L"abcd", .caret_pos = caret, .selection_start = anchor }, kWidth);
        ASSERT_NE(m.layout, nullptr);
        EXPECT_FLOAT_EQ(m.selection_x, XOf(m.layout, 1));
        EXPECT_NEAR(m.selection_width, XOf(m.layout, 3) - XOf(m.layout, 1), 1e-3f);
    }
    EXPECT_EQ(Measure({ .query = L"abcd", .caret_pos = 2, .selection_start = 2 }, kWidth).selection_width, 0.0f);
}

// IME の合成中テキストはキャレットの位置に挿入して下線を引き、キャレットはその末尾。選択範囲は描かない。
TEST_F(SearchInputTest, CompositionIsInsertedAtCaretWithUnderline)
{
    const auto m = Measure({ .query = L"ab", .ime_composition = L"xy", .caret_pos = 1, .selection_start = 0 }, kWidth);
    ASSERT_NE(m.layout, nullptr);
    BOOL underline = FALSE;
    m.layout->GetUnderline(0, &underline);
    EXPECT_FALSE(underline);
    m.layout->GetUnderline(1, &underline);
    EXPECT_TRUE(underline);
    m.layout->GetUnderline(3, &underline);
    EXPECT_FALSE(underline);
    EXPECT_FLOAT_EQ(m.caret_x, XOf(m.layout, 3));
    EXPECT_EQ(m.selection_width, 0.0f);
}

// 文字列と幅が同じ間はレイアウトを作り直さない。IME の合成中は下線の範囲もキーなので、同じ文字列の確定後とは別になる。
TEST_F(SearchInputTest, ReusesLayoutWhileTextAndWidthUnchanged)
{
    auto* const first = Measure({ .query = L"abc" }, kWidth).layout;
    EXPECT_EQ(Measure({ .query = L"abc", .caret_pos = 1 }, kWidth).layout, first);
    EXPECT_NE(Measure({ .query = L"abc" }, kWidth + 10.0f).layout, first);

    auto* const plain = Measure({ .query = L"ab" }, kWidth).layout;
    auto* const composing = Measure({ .query = L"a", .ime_composition = L"b", .caret_pos = 1 }, kWidth).layout;
    EXPECT_NE(composing, plain);
    EXPECT_EQ(Measure({ .query = L"a", .ime_composition = L"b", .caret_pos = 1 }, kWidth).layout, composing);
}

// クリック位置にいちばん近い文字の境界を返す。
TEST_F(SearchInputTest, HitTestFindsNearestBoundary)
{
    const auto m = Measure({ .query = L"abc" }, kWidth);
    ASSERT_NE(m.layout, nullptr);
    EXPECT_EQ(HitTest(L"abc", 0.0f, kWidth), 0);
    EXPECT_EQ(HitTest(L"abc", XOf(m.layout, 2) + 0.5f, kWidth), 2);
    EXPECT_EQ(HitTest(L"abc", 1000.0f, kWidth), 3);
    EXPECT_EQ(HitTest(L"", 10.0f, kWidth), 0);
    // 表示中のレイアウトと違う文字列でも測れる。
    EXPECT_EQ(HitTest(L"abcdef", 1000.0f, kWidth), 6);
}

// 件数は「今の番号 / 全件」、見つからなければ 0。初めての検索の途中 (-1) とクエリが空のときは出さない。
TEST_F(SearchInputTest, MatchCountShowsCurrentOfTotal)
{
    using Texts = std::vector<std::wstring>;
    EXPECT_EQ(BarTexts({ .query = L"a", .current_match = 1, .total_matches = 5, .visible = true }), (Texts{ L"2 / 5", L"Aa" }));
    EXPECT_EQ(BarTexts({ .query = L"a", .total_matches = 0, .visible = true }), (Texts{ L"0", L"Aa" }));
    EXPECT_EQ(BarTexts({ .query = L"a", .total_matches = -1, .visible = true }), (Texts{ L"Aa" }));
    EXPECT_EQ(BarTexts({ .total_matches = 5, .visible = true }), (Texts{ L"Aa" }));
}

// 目次の項目には見出しの文字列を描く。
TEST_F(SearchInputTest, TocRowsDrawHeadingText)
{
    const Theme theme = GetLightTheme();
    const auto nodes = ParseNodes("# A\n\n## B");
    const std::vector<FileEntry> files;
    const std::vector<TocEntry> toc{ { 0 }, { 1 } };
    const SidePaneState sp{
        .panes = { {}, SidePaneInstance{ .rect = kPane, .show = true, .zoom = 1.0f, .item_height = 24.0f } },
        .file_entries = files,
        .toc_entries = toc,
        .nodes = nodes,
        .active_toc_index = -1,
    };
    chrome::SidePaneFormats fmts;
    fmts.item[std::to_underlying(PaneTarget::Toc)] = fmt_.Get();
    DrawCommandList cmds;
    chrome::EmitSidePanes(cmds, text_layouts_, fmts, theme, sp);
    EXPECT_EQ(TextsOf(cmds), (std::vector<std::wstring>{ L"A", L"B" }));
}

// 測った選択範囲を、文字列より先に、文字列の左端からの位置に塗る。
TEST_F(SearchInputTest, SelectionIsPaintedBeforeText)
{
    const SearchBarRenderState sb{ .query = L"abc", .total_matches = 1, .caret_pos = 2, .selection_start = 0, .visible = true };
    const auto sbl = ComputeSearchBarLayout(kPane, true);
    const auto m = Measure(sb, sbl.text_width());
    ASSERT_GT(m.selection_width, 0.0f);

    DrawCommandList cmds;
    overlay::EmitSearchBar(cmds, text_layouts_, nullptr, nullptr, sbl, sb, m);
    const auto sel = std::ranges::find_if(cmds, [](const DrawCommand& c) {
        const auto* f = std::get_if<FillRectCmd>(&c);
        return f && f->brush_id == BrushId::Selection;
    });
    const auto text = std::ranges::find_if(cmds, [&](const DrawCommand& c) {
        const auto* t = std::get_if<DrawTextLayoutCmd>(&c);
        return t && t->layout == m.layout;
    });
    ASSERT_NE(sel, cmds.end());
    ASSERT_NE(text, cmds.end());
    EXPECT_LT(sel, text);

    const auto& sel_rect = std::get<FillRectCmd>(*sel).rect;
    EXPECT_FLOAT_EQ(sel_rect.left, sbl.text_left() + m.selection_x);
    EXPECT_FLOAT_EQ(sel_rect.right, sbl.text_left() + m.selection_x + m.selection_width);
    EXPECT_GT(sel_rect.top, sbl.input_rect.top);
    EXPECT_LT(sel_rect.bottom, sbl.input_rect.bottom);
    const auto& text_cmd = std::get<DrawTextLayoutCmd>(*text);
    EXPECT_FLOAT_EQ(text_cmd.origin.x, sbl.text_left());
    EXPECT_FLOAT_EQ(text_cmd.origin.y, sbl.input_rect.top);
}
