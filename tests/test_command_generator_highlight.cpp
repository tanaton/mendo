// CommandGenerator の DirectWrite 必須経路（ハイライト系）のテスト。
// MockTextMeasurer では entry.text_layout が nullptr になるため、検索/選択
// ハイライトの矩形発行と「検索→選択→本文」の順序は検証できない。本ファイルは
// 実 IDWriteFactory を使う DWriteTestBase の上で、その経路を踏む。
#include <gtest/gtest.h>
#include "command_generator.h"
#include "document_test_helpers.h"
#include "draw_cmd_test_util.h"
#include "draw_command.h"
#include "i18n.h"
#include "mermaid_util.h"
#include "dwrite_test_base.h"
#include "search_state.h"
#include "syntax.h"
#include "text_types.h"
#include "wic_util.h"
#include <algorithm>
#include <array>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <wincodec.h>
#include <wrl/client.h>

namespace {

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

// "あいう" は UTF-8 で 9 byte / UTF-16 で 3 code unit。
constexpr std::string_view kKanaThenAbc = "\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86" "abc";

class HighlightOrderTest : public DWriteTestBase {
protected:
    static constexpr PaneRect kPane{ 0.0f, 0.0f, 800.0f, 600.0f };
    CommandGenerator gen_;
    DrawCommandList cmds_;

    void SetUp() override
    {
        DWriteTestBase::SetUp();
        InitWithoutText(gen_, theme_);
    }

    // 次の呼び出しまで有効。
    const DrawCommandList& Draw(const ParsedLayout& pl, const TextSelection& sel = {}, std::span<const SearchMatch> matches = {},
                                int current_match = -1, const PaneRect& pane = kPane, float scroll_y = 0.0f)
    {
        cmds_.clear();
        gen_.GenerateMdPane(cmds_, { .nodes = pl.nodes, .cache = pl.cache, .selection = sel, .md_pane_rect = pane, .scroll_y = scroll_y,
                                     .search_matches = matches, .current_match_index = current_match }, 1.0f);
        return cmds_;
    }

    // md を描いたときの brushes の最初の矩形の幅。無ければ nullopt。
    std::optional<float> FirstRectWidth(std::string_view md, std::initializer_list<BrushId> brushes, const TextSelection& sel,
                                        std::span<const SearchMatch> matches = {})
    {
        const auto pl = ParseAndLayout(md);
        const auto rect = FindFirstFillRectByBrush(Draw(pl, sel, matches, matches.empty() ? -1 : 0), brushes);
        return rect ? std::optional{ rect->right - rect->left } : std::nullopt;
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

    // 範囲が重ならないことで、ハイライト矩形が独立に発行されることを保証する。
    // ASCII のみの本文なので UTF-16 の位置は byte の位置と同じ。
    const SearchMatch matches[] = { { 0, -1, -1, 0, 5 } };
    const auto& cmds = Draw(pl, TextSelection::MakeOrdered(0, 6, 0, 11), matches, 0);

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
        << "検索ハイライトは選択ハイライトより先に発行されるべき";
    EXPECT_LT(*first_selection, *first_text_layout)
        << "選択ハイライトは本文 DrawTextLayoutCmd より先に発行されるべき";
}

// 検索マッチが空・選択も無い場合は、ハイライト系 FillRectCmd は発行されないこと。
// 失敗時に「ノイズ FillRect が出ている」=どの装飾が漏れているかが分かる。
TEST_F(HighlightOrderTest, NoHighlightsWhenNeitherActive)
{
    auto pl = ParseAndLayout("Hello World");
    EXPECT_TRUE(CollectRectsByBrush(Draw(pl), { BrushId::SearchHighlight, BrushId::SearchHighlightCurrent, BrushId::Selection }).empty());
}

// 現在マッチは search_highlight_current_color、それ以外は search_highlight_color
// で塗られる。current_match_index がインデックス指定 → そのマッチだけ別色。
// 失敗するとユーザー視点で「現在マッチがどこにあるか分からない」状態になる。
TEST_F(HighlightOrderTest, CurrentMatchUsesCurrentColor)
{
    auto pl = ParseAndLayout("Hello Hello");
    const SearchMatch matches[] = { { 0, -1, -1, 0, 5 }, { 0, -1, -1, 6, 5 } };
    const auto& cmds = Draw(pl, {}, matches, 1);

    EXPECT_GE(CollectRectsByBrush(cmds, { BrushId::SearchHighlight }).size(), 1u) << "非カレントの検索マッチが少なくとも 1 つあるはず";
    EXPECT_GE(CollectRectsByBrush(cmds, { BrushId::SearchHighlightCurrent }).size(), 1u) << "カレントマッチが 1 つあるはず";
}

// start == end の選択範囲は描画ハイライトを発行しない。
// SetScrollSelectionMoved 等で起きうる空選択で余計な FillRectCmd が出ないことの確認。
TEST_F(HighlightOrderTest, EmptySelectionProducesNoSelectionRect)
{
    auto pl = ParseAndLayout("Hello World");
    EXPECT_TRUE(CollectRectsByBrush(Draw(pl, TextSelection::MakeOrdered(0, 5, 0, 5)), { BrushId::Selection }).empty())
        << "空選択で BrushId::Selection の FillRectCmd は出ない";
}

// SearchMatch は UTF-16 code unit の位置を持つ。HitTestTextRange に UTF-8 byte 位置を渡す取り違えがあると、
// 非 ASCII 後方の ASCII マッチで矩形位置が破綻する。
// 「あいうabc」の "abc" を検索して、ASCII 単独 "abc" と同等幅の矩形が出ることで担保する。
TEST_F(HighlightOrderTest, SearchHighlightHandlesUtf8MultibyteOffset)
{
    const SearchMatch ascii[] = { { 0, -1, -1, 0, 3 } };
    const SearchMatch after_kana[] = { { 0, -1, -1, 3, 3 } };
    const auto w1 = FirstRectWidth("abc", { BrushId::SearchHighlight, BrushId::SearchHighlightCurrent }, {}, ascii);
    const auto w2 = FirstRectWidth(kKanaThenAbc, { BrushId::SearchHighlight, BrushId::SearchHighlightCurrent }, {}, after_kana);
    ASSERT_TRUE(w1.has_value());
    ASSERT_TRUE(w2.has_value()) << "非 ASCII オフセット後の ASCII マッチが描画されるべき";
    EXPECT_NEAR(*w2, *w1, 2.0f) << "ハイライト幅は ASCII 単独時と一致すべき";
}

// 選択範囲は UTF-8 byte 単位で保持されるため、同じ取り違えバグを抱えうる。
TEST_F(HighlightOrderTest, SelectionHighlightHandlesUtf8MultibyteOffset)
{
    const auto w1 = FirstRectWidth("abc", { BrushId::Selection }, TextSelection::MakeOrdered(0, 0, 0, 3));
    const auto w2 = FirstRectWidth(kKanaThenAbc, { BrushId::Selection }, TextSelection::MakeOrdered(0, 9, 0, 12));
    ASSERT_TRUE(w1.has_value());
    ASSERT_TRUE(w2.has_value());
    EXPECT_NEAR(*w2, *w1, 2.0f);
}

namespace {

class VisibleHighlightTest : public HighlightOrderTest {
protected:
    static constexpr PaneRect PANE{ 0.0f, 0.0f, 800.0f, 200.0f };
    SearchState search_;
    int current_match_ = -1;

    void Search(const ParsedLayout& pl, std::string_view query)
    {
        RunSearchToEnd(search_, pl.nodes, query);
    }

    // 次の呼び出しまで有効。
    const DrawCommandList& Generate(const ParsedLayout& pl, float scroll_y, const TextSelection& sel = {})
    {
        return Draw(pl, sel, search_.GetMatches(), current_match_, PANE, scroll_y);
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

// 巨大コードブロックを可視範囲の行に絞って引いても、全マッチを引いた場合と同じ矩形を描く。
TEST_F(VisibleHighlightTest, CodeBlockSearchMatchesAllMatchesRects)
{
    auto pl = ParseAndLayout(MakeCodeBlockMd(1000));
    ASSERT_EQ(pl.nodes.size(), 1u);
    Search(pl, "line");
    ASSERT_EQ(search_.GetMatchCount(), 1000);

    const float h = pl.cache[0].height;
    for (const float y : { 0.0f, 150.0f, 300.0f, h * 0.5f, h * 0.5f - 120.0f, h - PANE.height }) {
        ASSERT_NO_FATAL_FAILURE(ExpectSearchRectsMatchAllMatches(pl, y));
    }
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
}

// 結合文字の途中までのマッチも、クラスタ全体を塗る (基底文字だけの検索で箱が幅 0 にならない)。
TEST_F(VisibleHighlightTest, SearchHighlightCoversWholeCluster)
{
    auto pl = ParseAndLayout("xéy");
    Search(pl, "e");
    ASSERT_EQ(search_.GetMatchCount(), 1);
    ASSERT_EQ(search_.GetMatches()[0].length_w, 1u);
    std::vector<DWRITE_HIT_TEST_METRICS> buf;
    ASSERT_EQ(FetchHitTestMetrics(pl.cache[0].text_layout(), 1, 2, buf), 1u);
    const float cluster_width = buf[0].width;
    ASSERT_GT(cluster_width, 0.0f);
    const auto rect = FindFirstFillRectByBrush(Generate(pl, 0.0f), { BrushId::SearchHighlight });
    ASSERT_TRUE(rect.has_value());
    EXPECT_NEAR(rect->right - rect->left, cluster_width, 0.01f);
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

// 巨大テーブルでも、描いたセルのマッチのハイライトを描く。
TEST_F(VisibleHighlightTest, TableSearchHighlightsVisibleCells)
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

    const auto& cmds2 = Generate(pl, pl.cache[0].height * 0.5f);
    EXPECT_FALSE(CollectRectsByBrush(cmds2, { BrushId::SearchHighlight }).empty());
}

// evict 中のセルは描かず、行を復元したらハイライトを描く。
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

    const auto restored = RestoreEntryTableRows(measurer_, pl.nodes[0], pl.cache[0], YRange{ 0.0f, PANE.height });
    ASSERT_TRUE(restored.restored);
    const auto& cmds = Generate(pl, 0.0f);
    EXPECT_FALSE(CollectRectsByBrush(cmds, { BrushId::SearchHighlight }).empty());
}

// 図のボタンは、その図の画像領域をホバーしている間だけ描く。
TEST_F(HighlightOrderTest, DiagramOverlayButtonsAreGatedByDiagramHover)
{
    auto pl = ParseAndLayout("```mermaid\ngraph TD;A-->B\n```\n");
    const int idx = FindFirstNodeIndexByType(pl.nodes, NodeType::Diagram);
    ASSERT_GE(idx, 0);

    // ボタンは描けた (commands があり) 保存・コピー元の描画命令もある図にだけ付く。
    auto& diagram = pl.cache.EnsureDiagram(static_cast<size_t>(idx));
    diagram.width = 100.0f;
    diagram.height = 80.0f;
    diagram.drawing = std::make_shared<const mermaid_util::DiagramDrawing>();

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
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc;
    ASSERT_HRESULT_SUCCEEDED(rt.As(&dc));
    ASSERT_HRESULT_SUCCEEDED(dc->CreateCommandList(diagram.commands.GetAddressOf()));

    auto icon = CreateThemeFormat(COPY_BTN_SIZE);
    ASSERT_NE(icon, nullptr);
    const MdPaneFormats formats{ .copy_btn_icon = icon };
    gen_.Init(theme_, formats, NoTextLayouts());

    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    const auto overlay = [](const FillRoundedRectCmd& r) static { return r.brush_id == BrushId::Overlay; };
    EXPECT_EQ(CountCmd<FillRoundedRectCmd>(GenerateMdPaneAt(gen_, pl.nodes, pl.cache, pane), overlay), 0);
    EXPECT_EQ(CountCmd<FillRoundedRectCmd>(GenerateMdPaneAt(gen_, pl.nodes, pl.cache, pane, 0.0f, idx), overlay), 3);
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
    }
}

// ---- ダイアグラムのプレースホルダのエラー表示 (issue #271) ----
// 文字列のテキストレイアウトを作るのに実 DirectWrite が要る。

namespace {

class DiagramPlaceholderTest : public DWriteTestBase {
protected:
    CommandGenerator gen_;
    TextLayoutCache text_layouts_;
    MdPaneFormats formats_;

    void SetUp() override
    {
        DWriteTestBase::SetUp();
        // Renderer::CreateTextFormats の placeholder_text と同じ設定。
        const TextFormatSpec spec{
            .family = theme_.font_family.c_str(),
            .size = theme_.font_size_body,
            .locale = L"ja-jp",
            .text_align = DWRITE_TEXT_ALIGNMENT_CENTER,
            .para_align = DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
            .ellipsis = true,
        };
        formats_.placeholder_text = CreateTextFormat(dwrite_factory_.Get(), spec);
        ASSERT_NE(formats_.placeholder_text, nullptr);
        // Renderer::CreateTextFormats の image_alt と同じ設定。
        formats_.image_alt = CreateTextFormat(dwrite_factory_.Get(), { .family = theme_.font_family.c_str(), .size = theme_.font_size_body,
                                                                       .para_align = DWRITE_PARAGRAPH_ALIGNMENT_CENTER, .no_wrap = true, .ellipsis = true });
        ASSERT_NE(formats_.image_alt, nullptr);
        text_layouts_.SetFactory(dwrite_factory_.Get());
        gen_.Init(theme_, formats_, text_layouts_);
    }
};

} // namespace

TEST_F(DiagramPlaceholderTest, NoErrorShowsLoadingText)
{
    auto pl = ParseAndLayout("```mermaid\ngraph TD;A-->B\n```\n");
    ASSERT_FALSE(pl.nodes.empty());

    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    const auto cmds = GenerateMdPaneAt(gen_, pl.nodes, pl.cache, pane);

    const auto text_cmd = FindFirst<DrawTextLayoutCmd>(cmds);
    ASSERT_TRUE(text_cmd.has_value());
    EXPECT_EQ(text_layouts_.TextOf(text_cmd->layout), i18n::S().loading);
}

TEST_F(DiagramPlaceholderTest, ErrorShowsErrorMessageInCautionColor)
{
    auto pl = ParseAndLayout("```mermaid\ngraph TD;A-->B\n```\n");
    ASSERT_FALSE(pl.nodes.empty());

    const int diagram_idx = FindFirstNodeIndexByType(pl.nodes, NodeType::Diagram);
    ASSERT_GE(diagram_idx, 0);
    pl.cache.EnsureDiagram(static_cast<size_t>(diagram_idx)).error = { DiagramError::Kind::Render, L"Parse error on line 1" };

    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    const auto cmds = GenerateMdPaneAt(gen_, pl.nodes, pl.cache, pane);

    const auto error_cmd = FindFirst<DrawTextLayoutCmd>(cmds);
    ASSERT_TRUE(error_cmd.has_value());
    EXPECT_EQ(text_layouts_.TextOf(error_cmd->layout), L"Parse error on line 1");
    EXPECT_EQ(error_cmd->brush_id, BrushId::AlertCaution);
}

// 読めない画像は「読み込み中」ではなく alt を枠で囲んで出す。枠は文字に合わせ、長い alt は内容幅で切る。
TEST_F(DiagramPlaceholderTest, UnavailableImageShowsAltInFrame)
{
    auto pl = ParseAndLayout("![badge](https://example.com/a.png)\n");
    const int img_idx = FindFirstNodeIndexByType(pl.nodes, NodeType::Image);
    ASSERT_GE(img_idx, 0);
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };

    for (const std::wstring& alt : { std::wstring(L"badge"), std::wstring(400, L'x') }) {
        SCOPED_TRACE(alt.size());
        pl.cache.EnsureDiagram(static_cast<size_t>(img_idx)).error = { DiagramError::Kind::ImageUnavailable, alt };
        const auto cmds = GenerateMdPaneAt(gen_, pl.nodes, pl.cache, pane);
        const auto text = FindFirst<DrawTextLayoutCmd>(cmds);
        ASSERT_TRUE(text.has_value());
        EXPECT_EQ(text_layouts_.TextOf(text->layout), alt);
        const auto frame = FindFirst<DrawRoundedRectCmd>(cmds);
        ASSERT_TRUE(frame.has_value());
        EXPECT_EQ(frame->brush_id, BrushId::Hr);
        // 見えている文字は枠の内側に収まる。
        DWRITE_TEXT_METRICS m{};
        ASSERT_TRUE(SUCCEEDED(text->layout->GetMetrics(&m)));
        EXPECT_GE(text->origin.x + m.left, frame->rect.left);
        EXPECT_LE(text->origin.x + m.left + m.width, frame->rect.right);
        EXPECT_LE(frame->rect.right, pane.width);
        if (alt.size() < 10) {
            EXPECT_LT(frame->rect.right - frame->rect.left, 200.0f) << "短い alt は枠を文字に合わせる";
        }
    }
}

// 言語で変わる文言は描くときに引くので、失敗した図も切り替えた言語の文言で出る。
// 失敗はコードで決まるので要求を止める。
TEST(DiagramEntryTest, ErrorsStopRendering)
{
    DiagramEntry diagram;
    EXPECT_TRUE(diagram.NeedsRender());
    diagram.error = { DiagramError::Kind::Render };
    EXPECT_FALSE(diagram.NeedsRender());
    diagram.error = { DiagramError::Kind::Unsupported };
    EXPECT_FALSE(diagram.NeedsRender());
}

TEST_F(DiagramPlaceholderTest, LocalizedErrorFollowsLanguageSwitch)
{
    auto pl = ParseAndLayout("```mermaid\ngraph TD;A-->B\n```\n");
    const int diagram_idx = FindFirstNodeIndexByType(pl.nodes, NodeType::Diagram);
    ASSERT_GE(diagram_idx, 0);
    auto& error = pl.cache.EnsureDiagram(static_cast<size_t>(diagram_idx)).error;
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    const auto text_of = [&] {
        const auto cmds = GenerateMdPaneAt(gen_, pl.nodes, pl.cache, pane);
        const auto cmd = FindFirst<DrawTextLayoutCmd>(cmds);
        return cmd ? std::wstring(text_layouts_.TextOf(cmd->layout)) : std::wstring();
    };

    for (const i18n::Strings* language : { &i18n::kJa, &i18n::kEn }) {
        SCOPED_TRACE(std::wstring(language->lang_key));
        const ScopedLanguage scoped(language);
        error = { DiagramError::Kind::Unsupported };
        EXPECT_EQ(text_of(), language->diagram_unsupported);
        error = { DiagramError::Kind::Render };
        EXPECT_EQ(text_of(), language->diagram_error);
    }
}

// mermaid の構文エラーは数行に折り返され、高さが本文 3 行ぶんの枠に収まらない。枠の外 (前後のブロックの上) には描かない。
TEST_F(DiagramPlaceholderTest, LongErrorStaysInsideBox)
{
    auto pl = ParseAndLayout("```mermaid\ngraph TD;A-->B\n```\n");
    const int diagram_idx = FindFirstNodeIndexByType(pl.nodes, NodeType::Diagram);
    ASSERT_GE(diagram_idx, 0);
    std::wstring message = L"Parse error on line 3:";
    for (int i = 0; i < 20; ++i) {
        message += L" Expecting 'SQE', 'DOUBLECIRCLEEND', 'PE', got 'NEWLINE'";
    }
    pl.cache.EnsureDiagram(static_cast<size_t>(diagram_idx)).error = { DiagramError::Kind::Render, message };

    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    const auto cmds = GenerateMdPaneAt(gen_, pl.nodes, pl.cache, pane);

    const auto cmd = FindFirst<DrawTextLayoutCmd>(cmds);
    ASSERT_TRUE(cmd.has_value());
    DWRITE_TEXT_METRICS metrics{};
    ASSERT_TRUE(SUCCEEDED(cmd->layout->GetMetrics(&metrics)));
    EXPECT_GE(metrics.top, 0.0f);
    EXPECT_LE(metrics.top + metrics.height, metrics.layoutHeight);
}
