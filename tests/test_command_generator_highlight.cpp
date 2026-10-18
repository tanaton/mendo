// CommandGenerator の DirectWrite 必須経路（ハイライト系）のテスト。
// MockTextMeasurer では entry.text_layout が nullptr になるため、検索/選択
// ハイライトの矩形発行と「検索→選択→本文」の順序は検証できない。本ファイルは
// 実 IDWriteFactory を使う DWriteTestBase の上で、その経路を踏む。
#include <gtest/gtest.h>
#include "cmd_gen_mock_test_base.h"
#include "command_generator.h"
#include "document_test_helpers.h"
#include "draw_command.h"
#include "dwrite_test_base.h"
#include "search_state.h"
#include "text_types.h"
#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

// DrawCommand の variant index を順に並べたものを返す。
// 失敗時のメッセージで「コマンド型の並び」が読みやすくなることを目的とする。
std::vector<size_t> ExtractCmdKinds(const DrawCommandList& cmds)
{
    std::vector<size_t> kinds;
    kinds.reserve(cmds.size());
    for (const auto& c : cmds) {
        kinds.push_back(c.index());
    }
    return kinds;
}

std::vector<D2D1_RECT_F> CollectRectsByBrush(const DrawCommandList& cmds, std::initializer_list<BrushId> brushes)
{
    std::vector<D2D1_RECT_F> rects;
    for (const auto& c : cmds) {
        if (auto* fr = std::get_if<FillRectCmd>(&c)) {
            if (std::ranges::contains(brushes, fr->brush_id)) {
                rects.push_back(fr->rect);
            }
        }
    }
    return rects;
}

std::optional<D2D1_RECT_F> FindFirstFillRectByBrush(const DrawCommandList& cmds,
                                                    std::initializer_list<BrushId> brushes)
{
    const auto rects = CollectRectsByBrush(cmds, brushes);
    return rects.empty() ? std::nullopt : std::optional{ rects.front() };
}

class HighlightOrderTest : public DWriteTestBase {
protected:
    CommandGenerator gen_;

    void SetUp() override
    {
        DWriteTestBase::SetUp();
        gen_.SetTheme(&theme_);
        gen_.SetFormats({ nullptr, nullptr, nullptr, nullptr });
    }
};

} // namespace

// 検索ハイライト → 選択ハイライト → 本文（DrawTextLayoutCmd）の順序を検証する。
// この順序が崩れると、選択中のテキストが検索ハイライトに塗り潰される、または
// 本文が両ハイライトの下に隠れる、という視覚的不具合が発生する。
TEST_F(HighlightOrderTest, SearchThenSelectionThenText)
{
    auto pl = ParseAndLayout("Hello World");
    ASSERT_FALSE(pl.nodes.empty());

    // node 0 の "Hello" を検索マッチに、"World" を選択範囲に設定する。
    // 範囲が重ならないことで、ハイライト矩形が独立に発行されることを保証する。
    // ASCII のみの本文なので start_w == start / length_w == length を直接渡せる。
    std::pmr::vector<SearchMatch> matches;
    matches.push_back({ 0, 0, 5, -1, -1, 0, 5 });
    gen_.SetSearchMatches(&matches, 0, 1);

    TextSelection sel = TextSelection::MakeOrdered(0, 6, 0, 11);
    sel.active = true;

    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    const auto& cmds = gen_.GenerateMdPane(pl.nodes, pl.cache, pane, 0.0f, sel);

    // 検索色・選択色・本文 の各最小 index を見つける。
    std::optional<size_t> first_search;
    std::optional<size_t> first_selection;
    std::optional<size_t> first_text_layout;
    for (size_t i = 0; i < cmds.size(); ++i) {
        const DrawCommand cmd = cmds[i];
        if (auto* fr = std::get_if<FillRectCmd>(&cmd)) {
            if (!first_search && (fr->brush_id == BrushId::SearchHighlight || fr->brush_id == BrushId::SearchHighlightCurrent)) {
                first_search = i;
            }
            else if (!first_selection && fr->brush_id == BrushId::Selection) {
                first_selection = i;
            }
        }
        else if (std::holds_alternative<DrawTextLayoutCmd>(cmd)) {
            if (!first_text_layout) {
                first_text_layout = i;
            }
        }
    }

    ASSERT_TRUE(first_search.has_value()) << "検索ハイライトの FillRectCmd が見つからない";
    ASSERT_TRUE(first_selection.has_value()) << "選択ハイライトの FillRectCmd が見つからない";
    ASSERT_TRUE(first_text_layout.has_value()) << "本文 DrawTextLayoutCmd が見つからない";
    EXPECT_LT(*first_search, *first_selection)
        << "検索ハイライトは選択ハイライトより先に発行されるべき。kinds=[..]";
    EXPECT_LT(*first_selection, *first_text_layout)
        << "選択ハイライトは本文 DrawTextLayoutCmd より先に発行されるべき";
}

// 検索マッチが空・選択も無い場合は、ハイライト系 FillRectCmd は発行されないこと。
// 失敗時に「ノイズ FillRect が出ている」=どの装飾が漏れているかが分かる。
TEST_F(HighlightOrderTest, NoHighlightsWhenNeitherActive)
{
    auto pl = ParseAndLayout("Hello World");
    gen_.SetSearchMatches(nullptr, -1, 0);
    const TextSelection sel{};
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    const auto& cmds = gen_.GenerateMdPane(pl.nodes, pl.cache, pane, 0.0f, sel);

    for (const auto& c : cmds) {
        if (auto* fr = std::get_if<FillRectCmd>(&c)) {
            EXPECT_FALSE(fr->brush_id == BrushId::SearchHighlight);
            EXPECT_FALSE(fr->brush_id == BrushId::SearchHighlightCurrent);
            EXPECT_FALSE(fr->brush_id == BrushId::Selection);
        }
    }
}

// 現在マッチは search_highlight_current_color、それ以外は search_highlight_color
// で塗られる。current_match_index がインデックス指定 → そのマッチだけ別色。
// 失敗するとユーザー視点で「現在マッチがどこにあるか分からない」状態になる。
TEST_F(HighlightOrderTest, CurrentMatchUsesCurrentColor)
{
    auto pl = ParseAndLayout("Hello Hello");
    std::pmr::vector<SearchMatch> matches;
    matches.push_back({ 0, 0, 5, -1, -1, 0, 5 });
    matches.push_back({ 0, 6, 5, -1, -1, 6, 5 });
    gen_.SetSearchMatches(&matches, 1, 1); // 2 番目を current にする

    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    const auto& cmds = gen_.GenerateMdPane(pl.nodes, pl.cache, pane, 0.0f, TextSelection{});

    EXPECT_GE(CollectRectsByBrush(cmds, { BrushId::SearchHighlight }).size(), 1u) << "非カレントの検索マッチが少なくとも 1 つあるはず";
    EXPECT_GE(CollectRectsByBrush(cmds, { BrushId::SearchHighlightCurrent }).size(), 1u) << "カレントマッチが 1 つあるはず";
}

// start == end の選択範囲は描画ハイライトを発行しない。
// SetScrollSelectionMoved 等で起きうる空選択で余計な FillRectCmd が出ないことの確認。
TEST_F(HighlightOrderTest, EmptySelectionProducesNoSelectionRect)
{
    auto pl = ParseAndLayout("Hello World");
    gen_.SetSearchMatches(nullptr, -1, 0);
    TextSelection sel = TextSelection::MakeOrdered(0, 5, 0, 5);
    sel.active = true;

    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    const auto& cmds = gen_.GenerateMdPane(pl.nodes, pl.cache, pane, 0.0f, sel);

    for (const auto& c : cmds) {
        if (auto* fr = std::get_if<FillRectCmd>(&c)) {
            EXPECT_FALSE(fr->brush_id == BrushId::Selection)
                << "空選択で BrushId::Selection の FillRectCmd は出ない";
        }
    }
}

// kinds 抽出ヘルパーが順序を正しく取り出すこと（ヘルパー自身のスモーク）。
TEST_F(HighlightOrderTest, ExtractCmdKindsIsOrdered)
{
    auto pl = ParseAndLayout("Hello");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    const auto& cmds = gen_.GenerateMdPane(pl.nodes, pl.cache, pane, 0.0f, TextSelection{});
    const auto kinds = ExtractCmdKinds(cmds);
    ASSERT_EQ(kinds.size(), cmds.size());
    for (size_t i = 0; i < cmds.size(); ++i) {
        EXPECT_EQ(kinds[i], cmds[i].index());
    }
}

// SearchMatch::start, length は UTF-8 byte 単位。HitTestTextRange は UTF-16 code unit
// を要求するため、変換漏れがあると非 ASCII 後方の ASCII マッチで矩形位置が破綻する。
// 「あいうabc」の "abc" を検索して、ASCII 単独 "abc" と同等幅の矩形が出ることで担保する。
TEST_F(HighlightOrderTest, SearchHighlightHandlesUtf8MultibyteOffset)
{
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };

    auto pl1 = ParseAndLayout("abc");
    std::pmr::vector<SearchMatch> matches1;
    matches1.push_back({ 0, 0, 3, -1, -1, 0, 3 });
    gen_.SetSearchMatches(&matches1, 0, 1);
    const auto& cmds1 = gen_.GenerateMdPane(pl1.nodes, pl1.cache, pane, 0.0f, TextSelection{});
    const auto rect1 = FindFirstFillRectByBrush(cmds1,
        { BrushId::SearchHighlight, BrushId::SearchHighlightCurrent });
    ASSERT_TRUE(rect1.has_value());

    // "あいう" は UTF-8 で 9 byte / UTF-16 で 3 code unit。
    auto pl2 = ParseAndLayout("\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86" "abc");
    std::pmr::vector<SearchMatch> matches2;
    matches2.push_back({ 0, 9, 3, -1, -1, 3, 3 });
    gen_.SetSearchMatches(&matches2, 0, 1);
    const auto& cmds2 = gen_.GenerateMdPane(pl2.nodes, pl2.cache, pane, 0.0f, TextSelection{});
    const auto rect2 = FindFirstFillRectByBrush(cmds2,
        { BrushId::SearchHighlight, BrushId::SearchHighlightCurrent });
    ASSERT_TRUE(rect2.has_value()) << "非 ASCII オフセット後の ASCII マッチが描画されるべき";
    EXPECT_NEAR(rect2->right - rect2->left, rect1->right - rect1->left, 2.0f)
        << "ハイライト幅は ASCII 単独時と一致すべき";
}

// 選択範囲も UTF-8 byte 単位で保持されるため、同じ取り違えバグを抱えうる。
// "あいうabc" の "abc" を選択して、矩形幅が ASCII 単独時と一致することで担保する。
TEST_F(HighlightOrderTest, SelectionHighlightHandlesUtf8MultibyteOffset)
{
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };

    auto pl1 = ParseAndLayout("abc");
    TextSelection sel1 = TextSelection::MakeOrdered(0, 0, 0, 3);
    sel1.active = true;
    const auto& cmds1 = gen_.GenerateMdPane(pl1.nodes, pl1.cache, pane, 0.0f, sel1);
    const auto rect1 = FindFirstFillRectByBrush(cmds1, { BrushId::Selection });
    ASSERT_TRUE(rect1.has_value());

    auto pl2 = ParseAndLayout("\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86" "abc");
    TextSelection sel2 = TextSelection::MakeOrdered(0, 9, 0, 12);
    sel2.active = true;
    const auto& cmds2 = gen_.GenerateMdPane(pl2.nodes, pl2.cache, pane, 0.0f, sel2);
    const auto rect2 = FindFirstFillRectByBrush(cmds2, { BrushId::Selection });
    ASSERT_TRUE(rect2.has_value());
    EXPECT_NEAR(rect2->right - rect2->left, rect1->right - rect1->left, 2.0f);
}

namespace {

size_t ComputedMatchCount(const NodeLayoutEntry& entry)
{
    return static_cast<size_t>(std::ranges::count_if(entry.ensure_search_hl_cache().spans, [](const SearchHlCache::Span& s) static {
        return s.count != SearchHlCache::Span::NOT_COMPUTED;
    }));
}

class VisibleHighlightTest : public HighlightOrderTest {
protected:
    static constexpr PaneRect PANE{ 0.0f, 0.0f, 800.0f, 200.0f };
    SearchState search_;

    void Search(const ParsedLayout& pl, std::string_view query)
    {
        search_.SetQuery(query);
        search_.ExecuteSearch(pl.nodes);
        gen_.SetSearchMatches(&search_.GetMatches(), -1, search_.GetGeneration());
    }

    // ノード 0 の全マッチを HitTestTextRange して可視判定した結果と、描かれた検索ハイライトを突き合わせる。
    void ExpectSearchRectsMatchAllMatches(const ParsedLayout& pl, float scroll_y)
    {
        auto* layout = pl.cache[0].text_layout();
        ASSERT_NE(layout, nullptr);
        const auto& cmds = gen_.GenerateMdPane(pl.nodes, pl.cache, PANE, scroll_y, TextSelection{});
        const auto text_cmd = FindFirst<DrawTextLayoutCmd>(cmds, [&](const DrawTextLayoutCmd& t) { return t.layout == layout; });
        ASSERT_TRUE(text_cmd.has_value());

        std::vector<D2D1_RECT_F> expected;
        std::pmr::vector<DWRITE_HIT_TEST_METRICS> buf;
        for (const auto& m : search_.GetMatches()) {
            const UINT32 n = FetchHitTestMetrics(layout, m.start_w, m.length_w, buf);
            for (UINT32 k = 0; k < n; ++k) {
                const auto r = RectFromHitTest(buf[k], text_cmd->origin.x, text_cmd->origin.y);
                if (!IsOffscreen(r.top, r.bottom - r.top, 0.0f, PANE.height)) {
                    expected.push_back(r);
                }
            }
        }
        const auto actual = CollectRectsByBrush(cmds, { BrushId::SearchHighlight, BrushId::SearchHighlightCurrent });
        ASSERT_FALSE(expected.empty());
        ASSERT_EQ(actual.size(), expected.size()) << "scroll_y=" << scroll_y;
        for (size_t i = 0; i < actual.size(); ++i) {
            EXPECT_NEAR(actual[i].left, expected[i].left, 0.01f);
            EXPECT_NEAR(actual[i].top, expected[i].top, 0.01f);
            EXPECT_NEAR(actual[i].right, expected[i].right, 0.01f);
            EXPECT_NEAR(actual[i].bottom, expected[i].bottom, 0.01f);
        }
    }
};

} // namespace

// 巨大コードブロックで頻出語を検索しても、矩形を計算するのは可視範囲の前後のマッチだけ。
TEST_F(VisibleHighlightTest, CodeBlockSearchComputesOnlyMatchesNearViewport)
{
    auto pl = ParseAndLayout(MakeCodeBlockMd(1000));
    ASSERT_EQ(pl.nodes.size(), 1u);
    Search(pl, "line");
    ASSERT_EQ(search_.GetMatchCount(), 1000);

    ASSERT_NO_FATAL_FAILURE(ExpectSearchRectsMatchAllMatches(pl, 0.0f));
    const size_t computed_top = ComputedMatchCount(pl.cache[0]);
    EXPECT_GT(computed_top, 0u);
    EXPECT_LT(computed_top, 100u);

    // 下へスクロールしても、離れた位置へ飛んでも、全マッチを引いた場合と同じ矩形を描く。
    const float h = pl.cache[0].height;
    for (const float y : { 150.0f, 300.0f, h * 0.5f, h * 0.5f - 120.0f, h - PANE.height }) {
        ASSERT_NO_FATAL_FAILURE(ExpectSearchRectsMatchAllMatches(pl, y));
    }
    EXPECT_LT(ComputedMatchCount(pl.cache[0]), 400u);
}

// 折り返しのある長い段落でも、可視範囲の上下の行にかかるマッチを取りこぼさない。
TEST_F(VisibleHighlightTest, WrappedParagraphSearchMatchesAllMatchesRects)
{
    std::string md;
    for (int i = 0; i < 3000; ++i) {
        md += "alpha beta ";
    }
    auto pl = ParseAndLayout(md);
    ASSERT_EQ(pl.nodes.size(), 1u);
    // マッチは空白を含むので、空白で折り返すと 2 行にまたがる。
    Search(pl, "a beta a");
    ASSERT_GT(search_.GetMatchCount(), 1000);

    const float h = pl.cache[0].height;
    ASSERT_GT(h, PANE.height * 4.0f);
    for (const float y : { 0.0f, 37.0f, h * 0.5f, h * 0.5f + 13.0f, h - PANE.height }) {
        ASSERT_NO_FATAL_FAILURE(ExpectSearchRectsMatchAllMatches(pl, y));
    }
    EXPECT_LT(ComputedMatchCount(pl.cache[0]), static_cast<size_t>(search_.GetMatchCount()));
}

// 巨大テーブルでも、矩形を計算するのは描いたセルのマッチだけ。
TEST_F(VisibleHighlightTest, TableSearchComputesOnlyVisibleCells)
{
    std::string md = "| A | B | C |\n|---|---|---|\n";
    for (int r = 0; r < 300; ++r) {
        md += "| cell | cell | cell |\n";
    }
    auto pl = ParseAndLayout(md);
    ASSERT_EQ(pl.nodes.size(), 1u);
    Search(pl, "cell");
    ASSERT_EQ(search_.GetMatchCount(), 900);

    const auto& cmds = gen_.GenerateMdPane(pl.nodes, pl.cache, PANE, 0.0f, TextSelection{});
    EXPECT_FALSE(CollectRectsByBrush(cmds, { BrushId::SearchHighlight }).empty());
    const size_t computed = ComputedMatchCount(pl.cache[0]);
    EXPECT_GT(computed, 0u);
    EXPECT_LT(computed, 100u);

    const auto& cmds2 = gen_.GenerateMdPane(pl.nodes, pl.cache, PANE, pl.cache[0].height * 0.5f, TextSelection{});
    EXPECT_FALSE(CollectRectsByBrush(cmds2, { BrushId::SearchHighlight }).empty());
    EXPECT_LT(ComputedMatchCount(pl.cache[0]), 200u);
}

// evict 中のセルのマッチは未計算のまま残す。空として確定すると、行を復元してもハイライトが出ない。
TEST_F(VisibleHighlightTest, TableSearchHighlightAppearsAfterEvictedRowsRestore)
{
    std::string md = "| A | B | C |\n|---|---|---|\n";
    for (int r = 0; r < 60; ++r) {
        md += "| cell | cell | cell |\n";
    }
    auto pl = ParseAndLayout(md);
    ASSERT_EQ(pl.nodes.size(), 1u);
    Search(pl, "cell");

    const float far_top = pl.cache.Top(0) - 10000.0f;
    pl.cache.EvictInvisibleTableRows(std::array{ size_t{ 0 } }, far_top, far_top + 1000.0f, 0.0f);
    ASSERT_TRUE(pl.cache[0].table_layout()->HasEvictedRows());

    const auto& evicted = gen_.GenerateMdPane(pl.nodes, pl.cache, PANE, 0.0f, TextSelection{});
    EXPECT_TRUE(CollectRectsByBrush(evicted, { BrushId::SearchHighlight }).empty());
    EXPECT_EQ(ComputedMatchCount(pl.cache[0]), 0u);

    const auto restored = measurer_.RestoreEvictedTableRows(pl.nodes[0], pl.cache[0], theme_.ContentWidth(PANE.width),
                                                            MeasureViewportRange{ 0.0f, PANE.height });
    ASSERT_TRUE(restored.restored);
    const auto& cmds = gen_.GenerateMdPane(pl.nodes, pl.cache, PANE, 0.0f, TextSelection{});
    EXPECT_FALSE(CollectRectsByBrush(cmds, { BrushId::SearchHighlight }).empty());
    EXPECT_GT(ComputedMatchCount(pl.cache[0]), 0u);
}

// 全行選択でも、矩形を引くのは可視範囲の前後の行だけ。スクロール先でも選択を描く。
TEST_F(VisibleHighlightTest, SelectionComputesOnlyLinesNearViewport)
{
    auto pl = ParseAndLayout(MakeCodeBlockMd(1000));
    ASSERT_EQ(pl.nodes.size(), 1u);
    const auto text_size = static_cast<uint32_t>(pl.nodes[0].GetText().size());
    TextSelection sel = TextSelection::MakeOrdered(0, 0, 0, text_size);
    sel.active = true;

    for (const float y : { 0.0f, 100.0f, pl.cache[0].height * 0.5f }) {
        const auto& cmds = gen_.GenerateMdPane(pl.nodes, pl.cache, PANE, y, sel);
        const auto rects = CollectRectsByBrush(cmds, { BrushId::Selection });
        ASSERT_GT(rects.size(), 5u) << "scroll_y=" << y;
        EXPECT_LT(rects.size(), 50u) << "scroll_y=" << y << " で 1000 行ぶんの矩形を積んでいる";
        // 可視範囲の上端 (本文の始まりより下へスクロールしたとき) と下端まで塗られている。
        if (y > 0.0f) {
            EXPECT_LE(std::ranges::min(rects, {}, &D2D1_RECT_F::top).top, 0.0f) << "scroll_y=" << y;
        }
        EXPECT_GE(std::ranges::max(rects, {}, &D2D1_RECT_F::bottom).bottom, PANE.height) << "scroll_y=" << y;
        EXPECT_LT(pl.cache[0].ensure_selection_hl_cache().rects.size(), 100u) << "scroll_y=" << y;
    }
}
