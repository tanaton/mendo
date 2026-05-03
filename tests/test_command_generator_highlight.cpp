// CommandGenerator の DirectWrite 必須経路（ハイライト系）のテスト。
// MockTextMeasurer では entry.text_layout が nullptr になるため、検索/選択
// ハイライトの矩形発行と「検索→選択→本文」の順序は検証できない。本ファイルは
// 実 IDWriteFactory を使う DWriteTestBase の上で、その経路を踏む。
#include <gtest/gtest.h>
#include "command_generator.h"
#include "draw_command.h"
#include "dwrite_test_base.h"
#include "search_state.h"
#include "text_types.h"
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

class HighlightOrderTest : public DWriteTestBase {
protected:
    CommandGenerator gen_;
    std::pmr::vector<DWRITE_HIT_TEST_METRICS> hit_test_buffer_;

    void SetUp() override
    {
        DWriteTestBase::SetUp();
        gen_.SetTheme(&theme_);
        gen_.SetFormats({ nullptr, nullptr, nullptr, nullptr });
        gen_.SetHitTestBuffer(&hit_test_buffer_);
    }
};

} // namespace

// 検索ハイライト → 選択ハイライト → 本文（DrawTextLayoutCmd）の順序を検証する。
// この順序が崩れると、選択中のテキストが検索ハイライトに塗り潰される、または
// 本文が両ハイライトの下に隠れる、という視覚的不具合が発生する。
TEST_F(HighlightOrderTest, SearchThenSelectionThenText)
{
    auto pl = ParseAndLayout(L"Hello World");
    ASSERT_FALSE(pl.nodes.empty());

    // node 0 の "Hello" を検索マッチに、"World" を選択範囲に設定する。
    // 範囲が重ならないことで、ハイライト矩形が独立に発行されることを保証する。
    std::pmr::vector<SearchMatch> matches;
    matches.push_back({ 0, 0, 5, -1, -1 });
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
            if (!first_search && (ColorEq(fr->color, theme_.search_highlight_color)
                || ColorEq(fr->color, theme_.search_highlight_current_color))) {
                first_search = i;
            }
            else if (!first_selection && ColorEq(fr->color, SELECTION_COLOR)) {
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
    auto pl = ParseAndLayout(L"Hello World");
    gen_.SetSearchMatches(nullptr, -1, 0);
    const TextSelection sel{};
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    const auto& cmds = gen_.GenerateMdPane(pl.nodes, pl.cache, pane, 0.0f, sel);

    for (const auto& c : cmds) {
        if (auto* fr = std::get_if<FillRectCmd>(&c)) {
            EXPECT_FALSE(ColorEq(fr->color, theme_.search_highlight_color));
            EXPECT_FALSE(ColorEq(fr->color, theme_.search_highlight_current_color));
            EXPECT_FALSE(ColorEq(fr->color, SELECTION_COLOR));
        }
    }
}

// 現在マッチは search_highlight_current_color、それ以外は search_highlight_color
// で塗られる。current_match_index がインデックス指定 → そのマッチだけ別色。
// 失敗するとユーザー視点で「現在マッチがどこにあるか分からない」状態になる。
TEST_F(HighlightOrderTest, CurrentMatchUsesCurrentColor)
{
    auto pl = ParseAndLayout(L"Hello Hello");
    std::pmr::vector<SearchMatch> matches;
    matches.push_back({ 0, 0, 5, -1, -1 });
    matches.push_back({ 0, 6, 5, -1, -1 });
    gen_.SetSearchMatches(&matches, 1, 1);  // 2 番目を current にする

    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    const auto& cmds = gen_.GenerateMdPane(pl.nodes, pl.cache, pane, 0.0f, TextSelection{});

    int n_default = 0;
    int n_current = 0;
    for (const auto& c : cmds) {
        if (auto* fr = std::get_if<FillRectCmd>(&c)) {
            if (ColorEq(fr->color, theme_.search_highlight_color)) {
                n_default++;
            }
            else if (ColorEq(fr->color, theme_.search_highlight_current_color)) {
                n_current++;
            }
        }
    }
    EXPECT_GE(n_default, 1) << "非カレントの検索マッチが少なくとも 1 つあるはず";
    EXPECT_GE(n_current, 1) << "カレントマッチが 1 つあるはず";
}

// start == end の選択範囲は描画ハイライトを発行しない。
// SetScrollSelectionMoved 等で起きうる空選択で余計な FillRectCmd が出ないことの確認。
TEST_F(HighlightOrderTest, EmptySelectionProducesNoSelectionRect)
{
    auto pl = ParseAndLayout(L"Hello World");
    gen_.SetSearchMatches(nullptr, -1, 0);
    TextSelection sel = TextSelection::MakeOrdered(0, 5, 0, 5);
    sel.active = true;

    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    const auto& cmds = gen_.GenerateMdPane(pl.nodes, pl.cache, pane, 0.0f, sel);

    for (const auto& c : cmds) {
        if (auto* fr = std::get_if<FillRectCmd>(&c)) {
            EXPECT_FALSE(ColorEq(fr->color, SELECTION_COLOR))
                << "空選択で SELECTION_COLOR の FillRectCmd は出ない";
        }
    }
}

// kinds 抽出ヘルパーが順序を正しく取り出すこと（ヘルパー自身のスモーク）。
TEST_F(HighlightOrderTest, ExtractCmdKindsIsOrdered)
{
    auto pl = ParseAndLayout(L"Hello");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    const auto& cmds = gen_.GenerateMdPane(pl.nodes, pl.cache, pane, 0.0f, TextSelection{});
    const auto kinds = ExtractCmdKinds(cmds);
    ASSERT_EQ(kinds.size(), cmds.size());
    for (size_t i = 0; i < cmds.size(); ++i) {
        EXPECT_EQ(kinds[i], cmds[i].index());
    }
}
