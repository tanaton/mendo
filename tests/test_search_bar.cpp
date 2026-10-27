#include <gtest/gtest.h>
#include "app_state.h"
#include "reducer.h"
#include "reducer_harness.h"
#include "test_helpers.h"
#include "ui_constants.h"
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

// 検索バーのアクションを Reduce に通し、状態と積まれた副作用を確かめる。
class SearchBarTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        InitReducerState(state_, theme_);
        SetPaneHeight(800.0f);
    }

    void SetPaneHeight(float height)
    {
        PaneLayout pl{};
        pl.md_rect.height = height;
        state_.pane_layout_cache.Set(pl);
    }

    // markdown を文書にし、各ノードを node_height ずつ縦に並べる。
    void SetDocument(std::string_view md, float node_height = 100.0f)
    {
        state_.document.doc = Document::FromMarkdown(std::string(md), L"C:\\docs\\a.md");
        state_.document.layout_cache = MakeUniformCache(static_cast<int>(Nodes().size()), node_height);
    }

    const std::vector<Node>& Nodes() const noexcept
    {
        return state_.document.doc.GetNodes();
    }

    SideEffectList Dispatch(const AppAction& action)
    {
        return Reduce(state_, action, layout_);
    }

    SearchBarRenderState RenderState() const
    {
        return BuildSearchBarRenderState(state_);
    }

    // count 個の "x" の段落で "x" を探し始め、最初の区切りで止まった状態にする。
    void StartSlicedSearch(int count)
    {
        std::string md;
        for (int i = 0; i < count; ++i) {
            md += "x\n\n";
        }
        SetDocument(md, 10.0f);
        ASSERT_EQ(Nodes().size(), static_cast<size_t>(count));
        auto& ss = state_.search.search_state;
        ss.SetQuery("x");
        ss.BeginSearch();
        ss.ContinueSearch(Nodes(), std::chrono::steady_clock::now() - std::chrono::seconds(1));
        ASSERT_TRUE(ss.IsSearching());
    }

    // 計測を頼まれたら on_ensure を呼ぶ。テストが計測の結果 (位置と高さ) を差し込む。
    struct CallbackLayout final : NodeLayoutAccess {
        std::function<void(int)> on_ensure;
        void EnsureNodeLayout(int node_index) override
        {
            if (on_ensure) {
                on_ensure(node_index);
            }
        }
    };

    Theme theme_{};
    AppState state_;
    CallbackLayout layout_;
};

// ═══════════════════════════════════════════════
// 初期状態
// ═══════════════════════════════════════════════

TEST_F(SearchBarTest, InitialState)
{
    const auto rs = RenderState();
    EXPECT_FALSE(rs.visible);
    EXPECT_EQ(rs.caret_pos, -1);
    EXPECT_EQ(rs.selection_start, -1);
    EXPECT_TRUE(rs.ime_composition.empty());
}

// ═══════════════════════════════════════════════
// 開く/閉じる
// ═══════════════════════════════════════════════

TEST_F(SearchBarTest, OpenSetsFocusAndInvalidates)
{
    const auto effects = Dispatch(OpenSearchBarAction{});

    EXPECT_TRUE(state_.search.search_state.IsVisible());
    EXPECT_TRUE(HasEffect<effect::SearchFocus>(effects));
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
    EXPECT_FALSE(HasEffect<effect::SetTimer>(effects));
}

TEST_F(SearchBarTest, OpenTogglesIfAlreadyOpen)
{
    Dispatch(OpenSearchBarAction{});
    EXPECT_TRUE(state_.search.search_state.IsVisible());

    const auto effects = Dispatch(OpenSearchBarAction{});
    EXPECT_FALSE(state_.search.search_state.IsVisible());
    EXPECT_TRUE(HasEffect<effect::SearchUnfocus>(effects));
}

TEST_F(SearchBarTest, CloseHidesAndUnfocuses)
{
    Dispatch(OpenSearchBarAction{});
    Dispatch(ImeCompositionAction{ L"あ" });
    const auto effects = Dispatch(CloseSearchBarAction{});

    EXPECT_FALSE(state_.search.search_state.IsVisible());
    EXPECT_TRUE(state_.search.ime_composition.empty());
    EXPECT_TRUE(HasEffect<effect::SearchUnfocus>(effects));
}

// ═══════════════════════════════════════════════
// テキスト変更
// ═══════════════════════════════════════════════

TEST_F(SearchBarTest, TextChangedEmptyQuery)
{
    SetDocument("hello world");

    Dispatch(SearchTextChangedAction{ L"" });
    EXPECT_TRUE(state_.search.search_state.GetQuery().empty());
    EXPECT_EQ(state_.search.search_state.GetMatchCount(), 0);
}

TEST_F(SearchBarTest, TextChangedSearchesImmediately)
{
    SetDocument("hello world");

    const auto effects = Dispatch(SearchTextChangedAction{ L"hello" });
    EXPECT_EQ(state_.search.search_state.GetMatchCount(), 1);
    EXPECT_EQ(state_.search.query_wide, L"hello");
    EXPECT_FALSE(HasEffect<effect::SetTimer>(effects));
}

// 区切った検索の続きは空き時間に進み、終わったら見ている位置の近くのマッチを選ぶ。
// 初めての検索を調べ終えるまでは件数を出さず (-1)、0 件を「見つからない」と見せない。
TEST_F(SearchBarTest, ContinueFinishesSlicedSearch)
{
    StartSlicedSearch(600);
    EXPECT_EQ(RenderState().total_matches, -1);

    const auto effects = Dispatch(SearchContinueAction{});

    EXPECT_FALSE(state_.search.search_state.IsSearching());
    EXPECT_EQ(RenderState().total_matches, 600);
    EXPECT_GE(state_.search.search_state.GetCurrentMatchIndex(), 0);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

// 入力が変わったら、前のクエリの検索の続きは止める。
TEST_F(SearchBarTest, TextChangedStopsRunningSearch)
{
    StartSlicedSearch(600);

    Dispatch(SearchTextChangedAction{ L"y" });

    EXPECT_FALSE(state_.search.search_state.IsSearching()) << "y は無いので新しい検索は 1 回で終わる";
    EXPECT_EQ(state_.search.search_state.GetMatchCount(), 0);
}

// 文書の切り替えで入力欄を空にしたときのように、中身が変わらない通知では検索も再描画もしない。
TEST_F(SearchBarTest, TextChangedSameTextIsNoop)
{
    SetDocument("text");

    EXPECT_TRUE(Dispatch(SearchTextChangedAction{ L"" }).empty());
}

// ═══════════════════════════════════════════════
// 選択・IME
// ═══════════════════════════════════════════════

TEST_F(SearchBarTest, Selection)
{
    Dispatch(OpenSearchBarAction{});

    const auto effects = Dispatch(SearchSelectionAction{ 2, 5 });
    EXPECT_EQ(state_.search.selection_start, 2);
    EXPECT_EQ(state_.search.caret_pos, 5);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

TEST_F(SearchBarTest, SelectionNoOpWhenUnchanged)
{
    Dispatch(OpenSearchBarAction{});

    Dispatch(SearchSelectionAction{ 2, 5 });
    EXPECT_TRUE(Dispatch(SearchSelectionAction{ 2, 5 }).empty());
}

// 閉じている間の EDIT の変化 (文書の切り替えで空にしたときなど) は描き直さない。
TEST_F(SearchBarTest, SelectionWhileHiddenDoesNotInvalidate)
{
    EXPECT_TRUE(Dispatch(SearchSelectionAction{ 0, 0 }).empty());
    EXPECT_EQ(state_.search.caret_pos, 0);
}

TEST_F(SearchBarTest, ImeComposition)
{
    Dispatch(OpenSearchBarAction{});

    Dispatch(ImeCompositionAction{ L"あい" });
    EXPECT_EQ(RenderState().ime_composition, L"あい");
}

TEST_F(SearchBarTest, ImeCompositionNoOpWhenUnchanged)
{
    Dispatch(OpenSearchBarAction{});

    Dispatch(ImeCompositionAction{ L"あ" });
    EXPECT_TRUE(Dispatch(ImeCompositionAction{ L"あ" }).empty());
}

// ═══════════════════════════════════════════════
// 大文字小文字切替・ハイライト切替
// ═══════════════════════════════════════════════

TEST_F(SearchBarTest, ToggleCaseSensitive)
{
    SetDocument("Hello hello");
    Dispatch(SearchTextChangedAction{ L"hello" });
    EXPECT_EQ(state_.search.search_state.GetMatchCount(), 2);

    EXPECT_FALSE(state_.search.search_state.IsCaseSensitive());
    Dispatch(ToggleCaseSensitiveAction{});
    EXPECT_TRUE(state_.search.search_state.IsCaseSensitive());
    EXPECT_EQ(state_.search.search_state.GetMatchCount(), 1) << "切り替えたら検索し直す";
}

TEST_F(SearchBarTest, ToggleHighlight)
{
    const bool before = state_.search.search_state.IsHighlightEnabled();
    const auto effects = Dispatch(ToggleHighlightAction{});
    EXPECT_NE(state_.search.search_state.IsHighlightEnabled(), before);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

// ═══════════════════════════════════════════════
// リセット
// ═══════════════════════════════════════════════

// 別の文書を開いたら閉じて空にし、入力欄も空にさせる。
TEST_F(SearchBarTest, DocumentOpenedClearsAll)
{
    SetDocument("test");
    Dispatch(OpenSearchBarAction{});
    Dispatch(SearchTextChangedAction{ L"test" });
    Dispatch(SearchSelectionAction{ 1, 3 });
    Dispatch(ImeCompositionAction{ L"test" });

    const auto effects = Dispatch(DocumentOpenedAction{});
    const auto* unfocus = FindEffect<effect::SearchUnfocus>(effects);
    ASSERT_NE(unfocus, nullptr);
    EXPECT_TRUE(unfocus->clear_text);
    const auto rs = RenderState();
    EXPECT_FALSE(rs.visible);
    EXPECT_EQ(rs.caret_pos, -1);
    EXPECT_EQ(rs.selection_start, -1);
    EXPECT_TRUE(rs.ime_composition.empty());
    EXPECT_TRUE(rs.query.empty());
    EXPECT_TRUE(state_.search.search_state.GetQuery().empty());
    // 入力欄を空にした EN_CHANGE は何もせず終わる。
    EXPECT_TRUE(Dispatch(SearchTextChangedAction{ L"" }).empty());
}

// ═══════════════════════════════════════════════
// BuildSearchBarRenderState
// ═══════════════════════════════════════════════

TEST_F(SearchBarTest, RenderStateReflectsState)
{
    SetDocument("abc");

    Dispatch(OpenSearchBarAction{});
    Dispatch(SearchTextChangedAction{ L"abc" });

    const auto rs = RenderState();
    EXPECT_TRUE(rs.visible);
    EXPECT_EQ(rs.query, L"abc");
    EXPECT_GE(rs.total_matches, 1);
    EXPECT_TRUE(rs.caret_visible);
    EXPECT_EQ(rs.hovered, SearchBarHitZone::None);

    // ホバーと、ウィンドウが非アクティブならキャレットを隠すことも描画状態に入れる。
    state_.view.hover.search_bar = SearchBarHitZone::Close;
    state_.window.window_active = false;
    const auto inactive = RenderState();
    EXPECT_FALSE(inactive.caret_visible);
    EXPECT_EQ(inactive.hovered, SearchBarHitZone::Close);
}

// リロードの後の検索し直し。スクロールはせず、見ている位置の近くのマッチを選ぶ。
TEST_F(SearchBarTest, ReloadSettledResearchesWithoutScrolling)
{
    SetDocument("hello\n\nworld hello\n", 500.0f);
    Dispatch(OpenSearchBarAction{});
    Dispatch(SearchTextChangedAction{ L"hello" });
    state_.view.viewport.ScrollTo(200.0f, MdMaxScroll(state_));

    const auto effects = Dispatch(ReloadSettledAction{});

    EXPECT_FLOAT_EQ(state_.view.viewport.GetScrollY(), 200.0f);
    EXPECT_EQ(state_.search.search_state.GetMatchCount(), 2);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

// 検索バーを閉じていれば、リロードの後も検索しない。
TEST_F(SearchBarTest, ReloadSettledSkipsWhenHidden)
{
    SetDocument("hello");
    EXPECT_TRUE(Dispatch(ReloadSettledAction{}).empty());
}

// リロードで差し替えても、検索し直すまでは変わらなかったノードのマッチを残す。
TEST_F(SearchBarTest, DocumentReloadedKeepsMatchesOfUnchangedNodes)
{
    SetDocument("hello\n\nworld hello\n");
    Dispatch(OpenSearchBarAction{});
    Dispatch(SearchTextChangedAction{ L"hello" });
    ASSERT_EQ(state_.search.search_state.GetMatchCount(), 2);

    const size_t n = Nodes().size();
    Dispatch(DocumentReloadedAction{ UnchangedEnds{ .head = n, .tail = 0, .old_count = n, .new_count = n } });

    EXPECT_EQ(state_.search.search_state.GetMatchCount(), 2);
    EXPECT_FALSE(state_.search.search_state.IsSearching());
}

// ═══════════════════════════════════════════════
// マッチへのスクロール
// ═══════════════════════════════════════════════

// scroll_target_ がジャンプでクリアされることを保証する。
// Why: クリアされないと後続の ApplyScrollTarget で検索ジャンプが上書きされる (検索ジャンプ修正)。
TEST_F(SearchBarTest, ScrollToMatchClearsScrollTarget)
{
    SetDocument("hello\n\nworld hello\n", 500.0f);
    ASSERT_EQ(Nodes().size(), 2u);
    ASSERT_FLOAT_EQ(MdMaxScroll(state_), 1000.0f - 800.0f);

    auto& viewport = state_.view.viewport;
    viewport.SetScrollTarget(0, 0.0f);
    EXPECT_TRUE(viewport.HasScrollTarget());

    Dispatch(OpenSearchBarAction{});
    Dispatch(SearchTextChangedAction{ L"hello" });
    Dispatch(SearchNextAction{});

    EXPECT_FALSE(viewport.HasScrollTarget());
}

// 同一ノード内でテーブル複数行のマッチを連続で「次へ」したとき、行ごとに scroll_y が進むこと。
// Why: 行毎の精密な Y が使われず block 先頭に丸まると、複数マッチ間でスクロールが進まない。
TEST_F(SearchBarTest, NextMatchAcrossTableRowsAdvancesScroll)
{
    SetDocument("| hit |\n|---|\n| hit |\n| hit |\n| hit |\n| hit |\n", 2000.0f);
    ASSERT_EQ(Nodes().size(), 1u);
    ASSERT_EQ(Nodes()[0].type, NodeType::Table);
    auto& tl = state_.document.layout_cache[0].ensure_table_layout(5, 1);
    tl.row_heights = { 400.0f, 400.0f, 400.0f, 400.0f, 400.0f };
    tl.row_cum_y = { 0.0f, 400.0f, 800.0f, 1200.0f, 1600.0f, 2000.0f };
    SetPaneHeight(600.0f);

    Dispatch(OpenSearchBarAction{});
    Dispatch(SearchTextChangedAction{ L"hit" });
    ASSERT_EQ(state_.search.search_state.GetMatchCount(), 5);

    float prev_scroll = state_.view.viewport.GetScrollY();
    int advanced = 0;
    for (int i = 0; i < 4; i++) {
        Dispatch(SearchNextAction{});
        const float now = state_.view.viewport.GetScrollY();
        if (now > prev_scroll) {
            advanced++;
        }
        prev_scroll = now;
    }
    EXPECT_GE(advanced, 1)
        << "行ごとに異なる Y に到達でき、最低1回はスクロールが進むこと";
}

// 未計測や evict 済みのノードは、計測してから行の Y を求める。計測前の位置で
// スクロール先を決めると、計測で位置が変わったマッチが画面外に残る。
TEST_F(SearchBarTest, ScrollToMatchMeasuresNodeBeforeResolvingY)
{
    SetDocument("a\n\nb\n\nx hello\n", 500.0f);
    ASSERT_EQ(Nodes().size(), 3u);
    std::vector<int> ensured;
    layout_.on_ensure = [this, &ensured](int node_index) {
        ensured.push_back(node_index);
        if (node_index == 2) {
            state_.document.layout_cache.SetTop(2, 5000.0f);
            state_.document.layout_cache[2].height = 5000.0f;
        }
    };

    Dispatch(OpenSearchBarAction{});
    Dispatch(SearchTextChangedAction{ L"hello" });

    ASSERT_FALSE(ensured.empty());
    EXPECT_TRUE(std::ranges::all_of(ensured, [](int i) static { return i == 2; }));
    const float visible = 800.0f - SEARCH_BAR_HEIGHT;
    EXPECT_FLOAT_EQ(state_.view.viewport.GetScrollY(), 5000.0f - visible / 3.0f);
}

// 現在位置が既に可視範囲内ならスクロールしない（余計な再描画を避ける）。
TEST_F(SearchBarTest, ScrollToMatchNoOpWhenAlreadyVisible)
{
    SetDocument("hello");
    auto& cache = state_.document.layout_cache;
    cache.SetTop(0, 100.0f);
    cache[0].height = 50.0f;

    Dispatch(OpenSearchBarAction{});
    Dispatch(SearchTextChangedAction{ L"hello" });

    const float before = state_.view.viewport.GetScrollY();
    const auto effects = Dispatch(SearchNextAction{});
    EXPECT_FLOAT_EQ(state_.view.viewport.GetScrollY(), before);
    EXPECT_FALSE(HasEffect<effect::BitmapManage>(effects));
}
