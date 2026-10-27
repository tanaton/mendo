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
#include "wic_util.h"
#include <algorithm>
#include <array>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <wincodec.h>
#include <wrl/client.h>

namespace {

// DrawCommand の variant index を順に並べたものを返す。
// 失敗時のメッセージで「コマンド型の並び」が読みやすくなることを目的とする。
std::vector<size_t> ExtractCmdKinds(const DrawCommandList& cmds)
{
    return cmds | std::views::transform([](const DrawCommand& c) static noexcept { return c.index(); }) | std::ranges::to<std::vector>();
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
        gen_.Init(theme_);
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
    // ASCII のみの本文なので UTF-16 の位置は byte の位置と同じ。
    std::vector<SearchMatch> matches;
    matches.push_back({ 0, -1, -1, 0, 5 });

    TextSelection sel = TextSelection::MakeOrdered(0, 6, 0, 11);

    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    const auto& cmds = gen_.GenerateMdPane({ .nodes = pl.nodes, .cache = pl.cache, .selection = sel, .md_pane_rect = pane, .search_matches = matches, .current_match_index = 0, .search_generation = 1 }, 1.0f);

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
    const TextSelection sel{};
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    const auto& cmds = gen_.GenerateMdPane({ .nodes = pl.nodes, .cache = pl.cache, .selection = sel, .md_pane_rect = pane }, 1.0f);

    EXPECT_TRUE(CollectRectsByBrush(cmds, { BrushId::SearchHighlight, BrushId::SearchHighlightCurrent, BrushId::Selection }).empty());
}

// 現在マッチは search_highlight_current_color、それ以外は search_highlight_color
// で塗られる。current_match_index がインデックス指定 → そのマッチだけ別色。
// 失敗するとユーザー視点で「現在マッチがどこにあるか分からない」状態になる。
TEST_F(HighlightOrderTest, CurrentMatchUsesCurrentColor)
{
    auto pl = ParseAndLayout("Hello Hello");
    std::vector<SearchMatch> matches;
    matches.push_back({ 0, -1, -1, 0, 5 });
    matches.push_back({ 0, -1, -1, 6, 5 });

    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    // 2 番目を current にする
    const auto& cmds = gen_.GenerateMdPane({ .nodes = pl.nodes, .cache = pl.cache, .selection = TextSelection{}, .md_pane_rect = pane, .search_matches = matches, .current_match_index = 1, .search_generation = 1 }, 1.0f);

    EXPECT_GE(CollectRectsByBrush(cmds, { BrushId::SearchHighlight }).size(), 1u) << "非カレントの検索マッチが少なくとも 1 つあるはず";
    EXPECT_GE(CollectRectsByBrush(cmds, { BrushId::SearchHighlightCurrent }).size(), 1u) << "カレントマッチが 1 つあるはず";
}

// start == end の選択範囲は描画ハイライトを発行しない。
// SetScrollSelectionMoved 等で起きうる空選択で余計な FillRectCmd が出ないことの確認。
TEST_F(HighlightOrderTest, EmptySelectionProducesNoSelectionRect)
{
    auto pl = ParseAndLayout("Hello World");
    TextSelection sel = TextSelection::MakeOrdered(0, 5, 0, 5);

    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    const auto& cmds = gen_.GenerateMdPane({ .nodes = pl.nodes, .cache = pl.cache, .selection = sel, .md_pane_rect = pane }, 1.0f);

    EXPECT_TRUE(CollectRectsByBrush(cmds, { BrushId::Selection }).empty())
        << "空選択で BrushId::Selection の FillRectCmd は出ない";
}

// kinds 抽出ヘルパーが順序を正しく取り出すこと（ヘルパー自身のスモーク）。
TEST_F(HighlightOrderTest, ExtractCmdKindsIsOrdered)
{
    auto pl = ParseAndLayout("Hello");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    const auto& cmds = gen_.GenerateMdPane({ .nodes = pl.nodes, .cache = pl.cache, .selection = TextSelection{}, .md_pane_rect = pane }, 1.0f);
    const auto kinds = ExtractCmdKinds(cmds);
    ASSERT_EQ(kinds.size(), cmds.size());
    for (size_t i = 0; i < cmds.size(); ++i) {
        EXPECT_EQ(kinds[i], cmds[i].index());
    }
}

// SearchMatch は UTF-16 code unit の位置を持つ。HitTestTextRange に UTF-8 byte 位置を渡す取り違えがあると、
// 非 ASCII 後方の ASCII マッチで矩形位置が破綻する。
// 「あいうabc」の "abc" を検索して、ASCII 単独 "abc" と同等幅の矩形が出ることで担保する。
TEST_F(HighlightOrderTest, SearchHighlightHandlesUtf8MultibyteOffset)
{
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };

    auto pl1 = ParseAndLayout("abc");
    std::vector<SearchMatch> matches1;
    matches1.push_back({ 0, -1, -1, 0, 3 });
    const auto& cmds1 = gen_.GenerateMdPane({ .nodes = pl1.nodes, .cache = pl1.cache, .selection = TextSelection{}, .md_pane_rect = pane, .search_matches = matches1, .current_match_index = 0, .search_generation = 1 }, 1.0f);
    const auto rect1 = FindFirstFillRectByBrush(cmds1,
        { BrushId::SearchHighlight, BrushId::SearchHighlightCurrent });
    ASSERT_TRUE(rect1.has_value());

    // "あいう" は UTF-8 で 9 byte / UTF-16 で 3 code unit。
    auto pl2 = ParseAndLayout("\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86" "abc");
    std::vector<SearchMatch> matches2;
    matches2.push_back({ 0, -1, -1, 3, 3 });
    const auto& cmds2 = gen_.GenerateMdPane({ .nodes = pl2.nodes, .cache = pl2.cache, .selection = TextSelection{}, .md_pane_rect = pane, .search_matches = matches2, .current_match_index = 0, .search_generation = 1 }, 1.0f);
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
    const auto& cmds1 = gen_.GenerateMdPane({ .nodes = pl1.nodes, .cache = pl1.cache, .selection = sel1, .md_pane_rect = pane }, 1.0f);
    const auto rect1 = FindFirstFillRectByBrush(cmds1, { BrushId::Selection });
    ASSERT_TRUE(rect1.has_value());

    auto pl2 = ParseAndLayout("\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86" "abc");
    TextSelection sel2 = TextSelection::MakeOrdered(0, 9, 0, 12);
    const auto& cmds2 = gen_.GenerateMdPane({ .nodes = pl2.nodes, .cache = pl2.cache, .selection = sel2, .md_pane_rect = pane }, 1.0f);
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
    int current_match_ = -1;

    void Search(const ParsedLayout& pl, std::string_view query)
    {
        search_.SetQuery(query);
        RunSearchToEnd(search_, pl.nodes);
    }

    const DrawCommandList& Generate(const ParsedLayout& pl, float scroll_y, const TextSelection& sel = {})
    {
        return gen_.GenerateMdPane({ .nodes = pl.nodes, .cache = pl.cache, .selection = sel, .md_pane_rect = PANE, .scroll_y = scroll_y,
                                     .search_matches = search_.GetMatches(), .current_match_index = current_match_, .search_generation = search_.GetGeneration() }, 1.0f);
    }

    // ノード 0 の全マッチを HitTestTextRange して可視判定した結果と、描かれた検索ハイライトを突き合わせる。
    void ExpectSearchRectsMatchAllMatches(const ParsedLayout& pl, float scroll_y)
    {
        auto* layout = pl.cache[0].text_layout();
        ASSERT_NE(layout, nullptr);
        const auto& cmds = Generate(pl, scroll_y);
        const auto text_cmd = FindFirst<DrawTextLayoutCmd>(cmds, [&](const DrawTextLayoutCmd& t) { return t.layout == layout; });
        ASSERT_TRUE(text_cmd.has_value());

        std::vector<D2D1_RECT_F> expected;
        std::vector<DWRITE_HIT_TEST_METRICS> buf;
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
    for (int i = 0; i < 1600; ++i) {
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

// 区切ったノードの継ぎ目をまたぐマッチは、続きのノードの先頭にもハイライトを描き、両方を現在のマッチの色にする。
TEST_F(VisibleHighlightTest, SearchMatchAcrossChunkSeamHighlightsBothNodes)
{
    std::string md;
    for (int i = 0; i < 20000; ++i) {
        md += static_cast<char>('a' + i % 26);
    }
    auto pl = ParseAndLayout(md);
    ASSERT_GE(pl.nodes.size(), 2u);
    ASSERT_FALSE(pl.nodes[1].newline_before_chunk());
    const auto head = pl.nodes[0].GetText();
    const std::string query = std::string{ head.substr(head.size() - 3) } + std::string{ pl.nodes[1].GetText().substr(0, 2) };
    Search(pl, query);
    const auto& matches = search_.GetMatches();
    const auto seam = std::ranges::find_if(matches, [](const SearchMatch& m) { return m.next_length_w > 0; });
    ASSERT_NE(seam, matches.end());
    EXPECT_EQ(seam->node_index, 0);
    EXPECT_EQ(seam->length_w, 3u);
    EXPECT_EQ(seam->next_length_w, 2u);
    current_match_ = static_cast<int>(seam - matches.begin());

    const float seam_y = 100.0f;
    const float scroll_y = pl.cache.Top(1) - seam_y;
    const auto& cmds = Generate(pl, scroll_y);
    const auto current = CollectRectsByBrush(cmds, { BrushId::SearchHighlightCurrent });
    EXPECT_TRUE(std::ranges::any_of(current, [&](const D2D1_RECT_F& r) { return r.bottom <= seam_y + 0.5f; })) << "前のノードの末尾";
    EXPECT_TRUE(std::ranges::any_of(current, [&](const D2D1_RECT_F& r) { return r.top >= seam_y - 0.5f; })) << "続きのノードの先頭";
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

    const auto& cmds = Generate(pl, 0.0f);
    EXPECT_FALSE(CollectRectsByBrush(cmds, { BrushId::SearchHighlight }).empty());
    const size_t computed = ComputedMatchCount(pl.cache[0]);
    EXPECT_GT(computed, 0u);
    EXPECT_LT(computed, 100u);

    const auto& cmds2 = Generate(pl, pl.cache[0].height * 0.5f);
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
    pl.cache.EvictInvisibleTableRows(std::array{ size_t{ 0 } }, YRange{ far_top, far_top + 1000.0f });
    ASSERT_TRUE(pl.cache[0].table_layout()->HasEvictedRows());

    const auto& evicted = Generate(pl, 0.0f);
    EXPECT_TRUE(CollectRectsByBrush(evicted, { BrushId::SearchHighlight }).empty());
    EXPECT_EQ(ComputedMatchCount(pl.cache[0]), 0u);

    const auto restored = RestoreEntryTableRows(measurer_, pl.nodes[0], pl.cache[0], theme_.ContentWidth(PANE.width),
                                                YRange{ 0.0f, PANE.height });
    ASSERT_TRUE(restored.restored);
    const auto& cmds = Generate(pl, 0.0f);
    EXPECT_FALSE(CollectRectsByBrush(cmds, { BrushId::SearchHighlight }).empty());
    EXPECT_GT(ComputedMatchCount(pl.cache[0]), 0u);
}

// 図の保存/コピーボタンは、その図の画像領域をホバーしている間だけ描く。
TEST_F(HighlightOrderTest, DiagramOverlayButtonsAreGatedByDiagramHover)
{
    auto pl = ParseAndLayout("```mermaid\ngraph TD;A-->B\n```\n");
    const int idx = FindFirstNodeIndexByType(pl.nodes, NodeType::Diagram);
    ASSERT_GE(idx, 0);

    // ボタンは描けた (bitmap があり) 保存・コピー元の PNG もある図にだけ付く。
    auto& diagram = pl.cache.EnsureDiagram(static_cast<size_t>(idx));
    diagram.width = 100.0f;
    diagram.height = 80.0f;
    diagram.png = std::make_shared<const ByteBuffer>(std::vector<uint8_t>{ 0x89 });

    const auto wic = wic_util::CreateWicFactory("HighlightOrderTest");
    ASSERT_NE(wic, nullptr);
    Microsoft::WRL::ComPtr<IWICBitmap> wic_bitmap;
    ASSERT_HRESULT_SUCCEEDED(wic->CreateBitmap(10, 10, GUID_WICPixelFormat32bppPBGRA,
                                               WICBitmapCacheOnLoad, wic_bitmap.GetAddressOf()));
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d;
    ASSERT_HRESULT_SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()));
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> rt;
    ASSERT_HRESULT_SUCCEEDED(d2d->CreateWicBitmapRenderTarget(
        wic_bitmap.Get(), D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE), rt.GetAddressOf()));
    ASSERT_HRESULT_SUCCEEDED(rt->CreateBitmapFromWicBitmap(wic_bitmap.Get(), diagram.bitmap.GetAddressOf()));

    auto icon = CreateThemeFormat(COPY_BTN_SIZE);
    ASSERT_NE(icon, nullptr);
    const MdPaneFormats formats{ .copy_btn_icon = icon };
    gen_.Init(theme_, &formats);

    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    const auto count_overlay_fills = [](const DrawCommandList& cmds) {
        return std::ranges::count_if(cmds, [](const DrawCommand& c) {
            const auto* fill = std::get_if<FillRoundedRectCmd>(&c);
            return fill && fill->brush_id == OverlayBrushId(false);
        });
    };

    const auto& hidden = gen_.GenerateMdPane({ .nodes = pl.nodes, .cache = pl.cache, .selection = TextSelection{}, .md_pane_rect = pane }, 1.0f);
    EXPECT_EQ(count_overlay_fills(hidden), 0);

    const auto& shown = gen_.GenerateMdPane({ .nodes = pl.nodes, .cache = pl.cache, .selection = TextSelection{}, .md_pane_rect = pane, .hovered_diagram = idx }, 1.0f);
    EXPECT_EQ(count_overlay_fills(shown), 2);
}

// 全行選択でも、矩形を引くのは可視範囲の前後の行だけ。スクロール先でも選択を描く。
TEST_F(VisibleHighlightTest, SelectionComputesOnlyLinesNearViewport)
{
    auto pl = ParseAndLayout(MakeCodeBlockMd(1000));
    ASSERT_EQ(pl.nodes.size(), 1u);
    const auto text_size = static_cast<uint32_t>(pl.nodes[0].GetText().size());
    TextSelection sel = TextSelection::MakeOrdered(0, 0, 0, text_size);

    for (const float y : { 0.0f, 100.0f, pl.cache[0].height * 0.5f }) {
        const auto& cmds = Generate(pl, y, sel);
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
