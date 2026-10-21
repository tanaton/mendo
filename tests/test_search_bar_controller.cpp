#include <gtest/gtest.h>
#include "search_bar_controller.h"
#include "app_constants.h"
#include "search_state.h"
#include "viewport_manager.h"
#include "layout_cache.h"
#include "test_helpers.h"
#include <algorithm>
#include <functional>
#include <vector>

namespace {

struct CallbackTracker {
    float md_pane_height = 800.0f;
    std::function<void(int)> on_ensure;
};

struct TestSearchBarCallbacks {
    CallbackTracker* t = nullptr;

    float get_md_pane_height()
    {
        return t->md_pane_height;
    }
    void ensure_node_layout(int node_index)
    {
        if (t->on_ensure) {
            t->on_ensure(node_index);
        }
    }
};

} // namespace

// 積まれた副作用を溜めて数えるテストフィクスチャ
class SearchBarControllerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ctrl_.Init(state_, viewport_, cache_, TestSearchBarCallbacks{ &tracker_ });
    }

    SearchState state_;
    ViewportManager viewport_;
    LayoutCache cache_;
    CallbackTracker tracker_;
    SearchBarControllerT<TestSearchBarCallbacks> ctrl_;
    std::pmr::vector<Node> nodes_;
    // 検証しない操作の副作用の捨て先。検証する操作はその場で新しいリストに積む。
    SideEffectList effects_;
};

// ═══════════════════════════════════════════════
// 初期状態
// ═══════════════════════════════════════════════

TEST_F(SearchBarControllerTest, InitialState)
{
    EXPECT_FALSE(ctrl_.HasFocus());
    EXPECT_EQ(ctrl_.GetCaretPos(), -1);
    EXPECT_EQ(ctrl_.GetSelectionStart(), -1);
    EXPECT_TRUE(ctrl_.GetImeComposition().empty());
}

// ═══════════════════════════════════════════════
// 開く/閉じる
// ═══════════════════════════════════════════════

TEST_F(SearchBarControllerTest, OnOpenSetsFocusAndInvalidates)
{
    SideEffectList effects;
    ctrl_.OnOpen(effects, nodes_);

    EXPECT_TRUE(ctrl_.HasFocus());
    EXPECT_TRUE(state_.IsVisible());
    EXPECT_TRUE(HasEffect<effect::SearchFocus>(effects));
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
    EXPECT_FALSE(HasEffect<effect::SetTimer>(effects));
}

TEST_F(SearchBarControllerTest, OnOpenTogglesIfAlreadyOpen)
{
    ctrl_.OnOpen(effects_, nodes_);
    EXPECT_TRUE(state_.IsVisible());

    ctrl_.OnOpen(effects_, nodes_);
    EXPECT_FALSE(state_.IsVisible());
    EXPECT_FALSE(ctrl_.HasFocus());
}

TEST_F(SearchBarControllerTest, OnCloseResetsState)
{
    ctrl_.OnOpen(effects_, nodes_);
    SideEffectList effects;
    ctrl_.OnClose(effects);

    EXPECT_FALSE(state_.IsVisible());
    EXPECT_FALSE(ctrl_.HasFocus());
    EXPECT_TRUE(HasEffect<effect::SearchUnfocus>(effects));
}

// ═══════════════════════════════════════════════
// テキスト変更とデバウンス
// ═══════════════════════════════════════════════

TEST_F(SearchBarControllerTest, OnTextChangedEmptyQuery)
{
    nodes_.push_back(MakeTextNode("hello world"));

    ctrl_.OnTextChanged(effects_, L"", nodes_, 0);
    EXPECT_TRUE(state_.GetQuery().empty());
    EXPECT_EQ(state_.GetMatchCount(), 0);
}

TEST_F(SearchBarControllerTest, OnTextChangedSmallDocImmediate)
{
    nodes_.push_back(MakeTextNode("hello world"));
    cache_ = MakeUniformCache(1, 100.0f);

    ctrl_.OnTextChanged(effects_, L"hello", nodes_, 0);
    EXPECT_EQ(state_.GetMatchCount(), 1);
}

// まだ検索していない大きな文書は、打鍵ごとの全走査が重い可能性があるのでデバウンスする。
TEST_F(SearchBarControllerTest, OnTextChangedLargeUnmeasuredDocDebounces)
{
    nodes_.push_back(MakeTextNode("text"));

    SideEffectList effects;
    ctrl_.OnTextChanged(effects, L"text", nodes_, decltype(ctrl_)::kImmediateSearchMaxBytes + 1);
    const auto* timer = FindEffect<effect::SetTimer>(effects);
    ASSERT_NE(timer, nullptr);
    EXPECT_EQ(timer->id, app_timer::Id::SEARCH_DEBOUNCE);
    EXPECT_EQ(state_.GetMatchCount(), 0);
}

// 一度速く検索できた文書は、大きくても待たずに検索する。文書を替えたら測り直す。
TEST_F(SearchBarControllerTest, OnTextChangedFastMeasuredSearchIsImmediate)
{
    nodes_.push_back(MakeTextNode("text"));
    cache_ = MakeUniformCache(1, 100.0f);
    constexpr size_t kLargeBytes = decltype(ctrl_)::kImmediateSearchMaxBytes + 1;

    ctrl_.OnOpen(effects_, nodes_);
    ctrl_.OnTextChanged(effects_, L"te", nodes_, 0);
    SideEffectList measured;
    ctrl_.OnTextChanged(measured, L"text", nodes_, kLargeBytes);
    EXPECT_FALSE(HasEffect<effect::SetTimer>(measured));
    EXPECT_EQ(state_.GetMatchCount(), 1);

    ctrl_.Reset(effects_);
    SideEffectList after_reset;
    ctrl_.OnTextChanged(after_reset, L"text", nodes_, kLargeBytes);
    EXPECT_TRUE(HasEffect<effect::SetTimer>(after_reset)) << "文書を替えたら測るまでデバウンスする";
}

// 文書の切り替えで入力欄を空にしたときのように、中身が変わらない通知では検索も再描画もしない。
// 区切った検索の続きはタイマーで進み、終わったら見ている位置の近くのマッチを選んでタイマーを止める。
TEST_F(SearchBarControllerTest, OnContinueTimerFinishesSlicedSearch)
{
    for (int i = 0; i < 600; ++i) {
        nodes_.push_back(MakeTextNode("x"));
    }
    cache_ = MakeUniformCache(600, 10.0f);
    state_.SetQuery("x");
    state_.BeginSearch();
    state_.ContinueSearch(nodes_, std::chrono::steady_clock::now() - std::chrono::seconds(1));
    ASSERT_TRUE(state_.IsSearching());

    SideEffectList effects;
    ctrl_.OnContinueTimer(effects, nodes_);

    EXPECT_FALSE(state_.IsSearching());
    EXPECT_EQ(state_.GetMatchCount(), 600);
    EXPECT_GE(state_.GetCurrentMatchIndex(), 0);
    const auto* kill = FindEffect<effect::KillTimer>(effects);
    ASSERT_NE(kill, nullptr);
    EXPECT_EQ(kill->id, app_timer::Id::SEARCH_CONTINUE);
}

// 検索の途中で移ったマッチは、終わった時点で選び直さない。
TEST_F(SearchBarControllerTest, OnContinueTimerKeepsMatchChosenDuringSearch)
{
    for (int i = 0; i < 600; ++i) {
        nodes_.push_back(MakeTextNode("x"));
    }
    cache_ = MakeUniformCache(600, 10.0f);
    state_.SetQuery("x");
    state_.BeginSearch();
    state_.ContinueSearch(nodes_, std::chrono::steady_clock::now() - std::chrono::seconds(1));
    ASSERT_TRUE(state_.IsSearching());

    SideEffectList effects;
    ctrl_.OnNext(effects);
    ctrl_.OnNext(effects);
    const int chosen = state_.GetCurrentMatchIndex();
    ASSERT_GT(chosen, 0) << "前提: 見ている位置 (先頭) に最も近いマッチとは違うものを選んでいる";

    ctrl_.OnContinueTimer(effects, nodes_);

    ASSERT_FALSE(state_.IsSearching());
    EXPECT_EQ(state_.GetCurrentMatchIndex(), chosen);
}

// 入力が変わったら、前のクエリの検索の続きは止める。
TEST_F(SearchBarControllerTest, OnTextChangedStopsRunningSearch)
{
    for (int i = 0; i < 600; ++i) {
        nodes_.push_back(MakeTextNode("x"));
    }
    state_.SetQuery("x");
    state_.BeginSearch();
    state_.ContinueSearch(nodes_, std::chrono::steady_clock::now() - std::chrono::seconds(1));
    ASSERT_TRUE(state_.IsSearching());

    SideEffectList effects;
    ctrl_.OnTextChanged(effects, L"y", nodes_, decltype(ctrl_)::kImmediateSearchMaxBytes + 1);

    EXPECT_FALSE(state_.IsSearching());
    EXPECT_NE(FindEffect<effect::SetTimer>(effects), nullptr) << "大きい文書はデバウンスしてから検索し直す";
}

TEST_F(SearchBarControllerTest, OnTextChangedSameTextIsNoop)
{
    nodes_.push_back(MakeTextNode("text"));

    SideEffectList effects;
    ctrl_.OnTextChanged(effects, L"", nodes_, 0);
    EXPECT_TRUE(effects.empty());
}

// ═══════════════════════════════════════════════
// 選択・IME
// ═══════════════════════════════════════════════

TEST_F(SearchBarControllerTest, SetSelection)
{
    ctrl_.OnOpen(effects_, nodes_);

    ctrl_.SetSelection(effects_, 2, 5);
    EXPECT_EQ(ctrl_.GetSelectionStart(), 2);
    EXPECT_EQ(ctrl_.GetCaretPos(), 5);
}

TEST_F(SearchBarControllerTest, SetSelectionNoOpWhenUnchanged)
{
    ctrl_.OnOpen(effects_, nodes_);

    ctrl_.SetSelection(effects_, 2, 5);
    SideEffectList effects;
    ctrl_.SetSelection(effects, 2, 5);
    EXPECT_TRUE(effects.empty());
}

TEST_F(SearchBarControllerTest, SetImeComposition)
{
    ctrl_.OnOpen(effects_, nodes_);

    ctrl_.SetImeComposition(effects_, L"あい");
    EXPECT_EQ(ctrl_.GetImeComposition(), L"あい");
}

TEST_F(SearchBarControllerTest, SetImeCompositionNoOpWhenUnchanged)
{
    ctrl_.OnOpen(effects_, nodes_);

    ctrl_.SetImeComposition(effects_, L"あ");
    SideEffectList effects;
    ctrl_.SetImeComposition(effects, L"あ");
    EXPECT_TRUE(effects.empty());
}

// ═══════════════════════════════════════════════
// 大文字小文字切替・ハイライト切替
// ═══════════════════════════════════════════════

TEST_F(SearchBarControllerTest, ToggleCaseSensitive)
{
    nodes_.push_back(MakeTextNode("Hello hello"));
    cache_.Resize(1);

    EXPECT_FALSE(state_.IsCaseSensitive());
    ctrl_.OnToggleCaseSensitive(effects_, nodes_);
    EXPECT_TRUE(state_.IsCaseSensitive());
}

TEST_F(SearchBarControllerTest, ToggleHighlight)
{
    const bool before = state_.IsHighlightEnabled();
    ctrl_.OnToggleHighlight(effects_);
    EXPECT_NE(state_.IsHighlightEnabled(), before);
}

// ═══════════════════════════════════════════════
// リセット
// ═══════════════════════════════════════════════

TEST_F(SearchBarControllerTest, ResetClearsAll)
{
    ctrl_.OnOpen(effects_, nodes_);
    ctrl_.SetImeComposition(effects_, L"test");

    ctrl_.Reset(effects_);
    EXPECT_FALSE(ctrl_.HasFocus());
    EXPECT_EQ(ctrl_.GetCaretPos(), -1);
    EXPECT_EQ(ctrl_.GetSelectionStart(), -1);
    EXPECT_TRUE(ctrl_.GetImeComposition().empty());
}

// ═══════════════════════════════════════════════
// BuildRenderState
// ═══════════════════════════════════════════════

TEST_F(SearchBarControllerTest, BuildRenderStateReflectsState)
{
    nodes_.push_back(MakeTextNode("abc"));
    cache_ = MakeUniformCache(1, 100.0f);

    ctrl_.OnOpen(effects_, nodes_);
    ctrl_.OnTextChanged(effects_, L"abc", nodes_, 0);

    const auto rs = ctrl_.BuildRenderState();
    EXPECT_TRUE(rs.visible);
    const int matches = rs.total_matches;
    EXPECT_GE(matches, 1);
}

// ═══════════════════════════════════════════════
// ScrollToCurrentMatch の挙動
// ═══════════════════════════════════════════════

// scroll_target_ がジャンプでクリアされることを保証する。
// Why: クリアされないと後続の ApplyScrollTarget で検索ジャンプが上書きされる (検索ジャンプ修正)。
TEST_F(SearchBarControllerTest, ScrollToMatchClearsScrollTarget)
{
    nodes_.push_back(MakeTextNode("hello"));
    nodes_.push_back(MakeTextNode("world hello"));

    cache_ = MakeUniformCache(2, 500.0f);

    viewport_.SyncMaxScroll(1000.0f, 800.0f);
    viewport_.SetScrollTarget(0, 0.0f);
    EXPECT_TRUE(viewport_.HasScrollTarget());

    state_.Show();
    ctrl_.OnTextChanged(effects_, L"hello", nodes_, 0);
    ctrl_.OnNext(effects_);

    EXPECT_FALSE(viewport_.HasScrollTarget());
}

// 同一ノード内でテーブル複数行のマッチを連続で「次へ」したとき、行ごとに scroll_y が進むこと。
// Why: 行毎の精密な Y が使われず block 先頭に丸まると、複数マッチ間でスクロールが進まない。
TEST_F(SearchBarControllerTest, NextMatchAcrossTableRowsAdvancesScroll)
{
    nodes_.push_back(MakeTableNode({ { "hit" }, { "hit" }, { "hit" }, { "hit" }, { "hit" } }));

    cache_ = MakeUniformCache(1, 2000.0f);
    auto& tl = cache_[0].ensure_table_layout();
    tl.col_count = 1;
    tl.row_heights = { 400.0f, 400.0f, 400.0f, 400.0f, 400.0f };
    tl.row_cum_y = { 0.0f, 400.0f, 800.0f, 1200.0f, 1600.0f, 2000.0f };

    tracker_.md_pane_height = 600.0f;
    viewport_.SyncMaxScroll(2000.0f, tracker_.md_pane_height);

    state_.Show();
    ctrl_.OnTextChanged(effects_, L"hit", nodes_, 0);

    float prev_scroll = viewport_.GetScrollY();
    int advanced = 0;
    for (int i = 0; i < 4; i++) {
        ctrl_.OnNext(effects_);
        const float now = viewport_.GetScrollY();
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
TEST_F(SearchBarControllerTest, ScrollToMatchMeasuresNodeBeforeResolvingY)
{
    nodes_.push_back(MakeTextNode("a"));
    nodes_.push_back(MakeTextNode("b"));
    nodes_.push_back(MakeTextNode("x hello"));
    cache_ = MakeUniformCache(3, 500.0f);
    std::vector<int> ensured;
    tracker_.on_ensure = [this, &ensured](int node_index) {
        ensured.push_back(node_index);
        if (node_index == 2) {
            cache_.SetTop(2, 5000.0f);
        }
    };
    viewport_.SyncMaxScroll(10000.0f, tracker_.md_pane_height);

    state_.Show();
    ctrl_.OnTextChanged(effects_, L"hello", nodes_, 0);

    ASSERT_FALSE(ensured.empty());
    EXPECT_TRUE(std::ranges::all_of(ensured, [](int i) static { return i == 2; }));
    const float visible = tracker_.md_pane_height - SEARCH_BAR_HEIGHT;
    EXPECT_FLOAT_EQ(viewport_.GetScrollY(), 5000.0f - visible / 3.0f);
}

// 現在位置が既に可視範囲内ならスクロールしない（余計な再描画を避ける）。
TEST_F(SearchBarControllerTest, ScrollToMatchNoOpWhenAlreadyVisible)
{
    nodes_.push_back(MakeTextNode("hello"));

    cache_.Resize(1);
    cache_.SetTop(0, 100.0f);
    cache_[0].height = 50.0f;

    viewport_.SyncMaxScroll(1000.0f, tracker_.md_pane_height);
    state_.Show();
    ctrl_.OnTextChanged(effects_, L"hello", nodes_, 0);

    const float before = viewport_.GetScrollY();
    ctrl_.OnNext(effects_);
    EXPECT_FLOAT_EQ(viewport_.GetScrollY(), before);
}

// ---- 検索欄の選択範囲を LPARAM に詰める ----

TEST(SearchSelectionLParamTest, RoundTripsAnchorAndCaret)
{
    const std::pair<int, int> kCases[] = {
        { 0, 0 }, { 3, 7 }, { 7, 3 }, { -1, 5 }, { 5, -1 }, { INT_MAX, INT_MIN },
    };
    for (const auto& [anchor, caret] : kCases) {
        SCOPED_TRACE(::testing::Message() << anchor << "," << caret);
        const auto [a, c] = app_param::UnpackSearchSelectionLParam(app_param::MakeSearchSelectionLParam(anchor, caret));
        EXPECT_EQ(a, anchor);
        EXPECT_EQ(c, caret);
    }
}
