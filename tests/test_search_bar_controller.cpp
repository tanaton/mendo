#include <gtest/gtest.h>
#include "search_bar_controller.h"
#include "search_state.h"
#include "viewport_manager.h"
#include "layout_cache.h"
#include "renderer.h"
#include "test_helpers.h"

// コールバック呼び出しを記録するテストフィクスチャ
class SearchBarControllerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ctrl_.Init(state_, viewport_, cache_, MakeCallbacks());
    }

    SearchBarController::Callbacks MakeCallbacks()
    {
        return {
            .invalidate = [this]() { invalidate_count_++; },
            .invalidate_search_bar = [this]() { invalidate_search_bar_count_++; },
            .set_timer = [this](UINT_PTR id, UINT ms) {
                last_timer_id_ = id;
                last_timer_ms_ = ms;
                set_timer_count_++;
            },
            .kill_timer = [this](UINT_PTR id) {
                last_killed_timer_ = id;
                kill_timer_count_++;
            },
            .focus_select_all = [this]() { focus_select_all_count_++; },
            .focus_set_caret = [this](int pos) { last_caret_pos_ = pos; },
            .focus_set_selection = [this](int a, int c) {
                last_sel_anchor_ = a;
                last_sel_caret_ = c;
            },
            .unfocus = [this]() { unfocus_count_++; },
            .get_md_pane_height = [this]() -> float { return md_pane_height_; },
            .on_scroll_changed = [this](float v) {
                last_scroll_changed_value_ = v;
                on_scroll_changed_count_++;
            },
        };
    }

    SearchState state_;
    ViewportManager viewport_;
    LayoutCache cache_;
    SearchBarController ctrl_;

    int invalidate_count_ = 0;
    int invalidate_search_bar_count_ = 0;
    int set_timer_count_ = 0;
    int kill_timer_count_ = 0;
    int focus_select_all_count_ = 0;
    int unfocus_count_ = 0;
    UINT_PTR last_timer_id_ = 0;
    UINT last_timer_ms_ = 0;
    UINT_PTR last_killed_timer_ = 0;
    int last_caret_pos_ = -1;
    int last_sel_anchor_ = -1;
    int last_sel_caret_ = -1;
    float md_pane_height_ = 800.0f;
    float last_scroll_changed_value_ = -1.0f;
    int on_scroll_changed_count_ = 0;
};

// ═══════════════════════════════════════════════
// 初期状態
// ═══════════════════════════════════════════════

TEST_F(SearchBarControllerTest, InitialState)
{
    EXPECT_FALSE(ctrl_.HasFocus());
    EXPECT_EQ(ctrl_.GetCaretPos(), -1);
    EXPECT_EQ(ctrl_.GetSelectionStart(), -1);
    EXPECT_EQ(ctrl_.GetHover(), SearchBarHitZone::None);
    EXPECT_FALSE(ctrl_.IsDragging());
    EXPECT_TRUE(ctrl_.GetImeComposition().empty());
}

// ═══════════════════════════════════════════════
// 開く/閉じる
// ═══════════════════════════════════════════════

TEST_F(SearchBarControllerTest, OnOpenSetsFocusAndInvalidates)
{
    std::pmr::vector<Node> nodes;
    ctrl_.OnOpen(nodes);

    EXPECT_TRUE(ctrl_.HasFocus());
    EXPECT_TRUE(state_.IsVisible());
    EXPECT_EQ(focus_select_all_count_, 1);
    EXPECT_GT(invalidate_count_, 0);
}

TEST_F(SearchBarControllerTest, OnOpenTogglesIfAlreadyOpen)
{
    std::pmr::vector<Node> nodes;
    ctrl_.OnOpen(nodes);
    EXPECT_TRUE(state_.IsVisible());

    ctrl_.OnOpen(nodes);
    EXPECT_FALSE(state_.IsVisible());
    EXPECT_FALSE(ctrl_.HasFocus());
}

TEST_F(SearchBarControllerTest, OnCloseResetsState)
{
    std::pmr::vector<Node> nodes;
    ctrl_.OnOpen(nodes);
    ctrl_.OnClose();

    EXPECT_FALSE(state_.IsVisible());
    EXPECT_FALSE(ctrl_.HasFocus());
    EXPECT_EQ(ctrl_.GetHover(), SearchBarHitZone::None);
    EXPECT_GE(unfocus_count_, 1);
}

// ═══════════════════════════════════════════════
// テキスト変更とデバウンス
// ═══════════════════════════════════════════════

TEST_F(SearchBarControllerTest, OnTextChangedEmptyQuery)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode(L"hello world"));

    ctrl_.OnTextChanged(L"", nodes);
    EXPECT_TRUE(state_.GetQuery().empty());
    EXPECT_EQ(state_.GetMatchCount(), 0);
}

TEST_F(SearchBarControllerTest, OnTextChangedSmallDocImmediate)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode(L"hello world"));
    cache_.Resize(nodes.size());
    cache_[0].text_top = 0.0f;
    cache_[0].height = 100.0f;

    ctrl_.OnTextChanged(L"hello", nodes);
    EXPECT_EQ(state_.GetMatchCount(), 1);
}

TEST_F(SearchBarControllerTest, OnTextChangedLargeDocDebounces)
{
    std::pmr::vector<Node> nodes;
    for (int i = 0; i < 1001; ++i) {
        nodes.push_back(MakeTextNode(L"text"));
    }

    const int before = set_timer_count_;
    ctrl_.OnTextChanged(L"text", nodes);
    EXPECT_GT(set_timer_count_, before);
    EXPECT_EQ(last_timer_id_, SearchBarController::TIMER_DEBOUNCE);
}

// ═══════════════════════════════════════════════
// 選択・IME
// ═══════════════════════════════════════════════

TEST_F(SearchBarControllerTest, SetSelection)
{
    std::pmr::vector<Node> nodes;
    ctrl_.OnOpen(nodes);

    ctrl_.SetSelection(2, 5);
    EXPECT_EQ(ctrl_.GetSelectionStart(), 2);
    EXPECT_EQ(ctrl_.GetCaretPos(), 5);
}

TEST_F(SearchBarControllerTest, SetSelectionNoOpWhenUnchanged)
{
    std::pmr::vector<Node> nodes;
    ctrl_.OnOpen(nodes);

    ctrl_.SetSelection(2, 5);
    const int before = invalidate_search_bar_count_;
    ctrl_.SetSelection(2, 5);
    EXPECT_EQ(invalidate_search_bar_count_, before);
}

TEST_F(SearchBarControllerTest, SetImeComposition)
{
    std::pmr::vector<Node> nodes;
    ctrl_.OnOpen(nodes);

    ctrl_.SetImeComposition(L"あい");
    EXPECT_EQ(ctrl_.GetImeComposition(), L"あい");
}

TEST_F(SearchBarControllerTest, SetImeCompositionNoOpWhenUnchanged)
{
    std::pmr::vector<Node> nodes;
    ctrl_.OnOpen(nodes);

    ctrl_.SetImeComposition(L"あ");
    const int before = invalidate_search_bar_count_;
    ctrl_.SetImeComposition(L"あ");
    EXPECT_EQ(invalidate_search_bar_count_, before);
}

// ═══════════════════════════════════════════════
// ドラッグ選択
// ═══════════════════════════════════════════════

TEST_F(SearchBarControllerTest, DragLifecycle)
{
    EXPECT_FALSE(ctrl_.IsDragging());

    ctrl_.StartDrag(3);
    EXPECT_TRUE(ctrl_.IsDragging());
    EXPECT_EQ(ctrl_.GetDragAnchor(), 3);

    ctrl_.EndDrag();
    EXPECT_FALSE(ctrl_.IsDragging());
}

TEST_F(SearchBarControllerTest, OnCaptureChangedEndsDrag)
{
    ctrl_.StartDrag(0);
    EXPECT_TRUE(ctrl_.IsDragging());

    ctrl_.OnCaptureChanged();
    EXPECT_FALSE(ctrl_.IsDragging());
}

// ═══════════════════════════════════════════════
// キャレットブリンク
// ═══════════════════════════════════════════════

TEST_F(SearchBarControllerTest, CaretBlinkToggles)
{
    std::pmr::vector<Node> nodes;
    ctrl_.OnOpen(nodes);

    auto rs1 = ctrl_.BuildRenderState();
    EXPECT_TRUE(rs1.caret_visible);

    ctrl_.OnCaretBlinkTimer();
    auto rs2 = ctrl_.BuildRenderState();
    EXPECT_FALSE(rs2.caret_visible);

    ctrl_.OnCaretBlinkTimer();
    auto rs3 = ctrl_.BuildRenderState();
    EXPECT_TRUE(rs3.caret_visible);
}

// ═══════════════════════════════════════════════
// 大文字小文字切替・ハイライト切替
// ═══════════════════════════════════════════════

TEST_F(SearchBarControllerTest, ToggleCaseSensitive)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode(L"Hello hello"));
    cache_.Resize(1);

    EXPECT_FALSE(state_.IsCaseSensitive());
    ctrl_.OnToggleCaseSensitive(nodes);
    EXPECT_TRUE(state_.IsCaseSensitive());
}

TEST_F(SearchBarControllerTest, ToggleHighlight)
{
    const bool before = state_.IsHighlightEnabled();
    ctrl_.OnToggleHighlight();
    EXPECT_NE(state_.IsHighlightEnabled(), before);
}

// ═══════════════════════════════════════════════
// リセット
// ═══════════════════════════════════════════════

TEST_F(SearchBarControllerTest, ResetClearsAll)
{
    std::pmr::vector<Node> nodes;
    ctrl_.OnOpen(nodes);
    ctrl_.StartDrag(5);
    ctrl_.SetImeComposition(L"test");

    ctrl_.Reset();
    EXPECT_FALSE(ctrl_.HasFocus());
    EXPECT_EQ(ctrl_.GetCaretPos(), -1);
    EXPECT_EQ(ctrl_.GetSelectionStart(), -1);
    EXPECT_FALSE(ctrl_.IsDragging());
    EXPECT_TRUE(ctrl_.GetImeComposition().empty());
    EXPECT_EQ(ctrl_.GetHover(), SearchBarHitZone::None);
}

// ═══════════════════════════════════════════════
// BuildRenderState
// ═══════════════════════════════════════════════

TEST_F(SearchBarControllerTest, BuildRenderStateReflectsState)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode(L"abc"));
    cache_.Resize(1);
    cache_[0].text_top = 0.0f;
    cache_[0].height = 100.0f;

    ctrl_.OnOpen(nodes);
    ctrl_.OnTextChanged(L"abc", nodes);

    const auto rs = ctrl_.BuildRenderState();
    EXPECT_TRUE(rs.visible);
    EXPECT_TRUE(rs.has_focus);
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
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode(L"hello"));
    nodes.push_back(MakeTextNode(L"world hello"));

    cache_.Resize(2);
    cache_[0].text_top = 0.0f;
    cache_[0].height = 500.0f;
    cache_[1].text_top = 500.0f;
    cache_[1].height = 500.0f;

    viewport_.SyncMaxScroll(1000.0f, 800.0f);
    viewport_.SetScrollTarget(0, 0.0f);
    EXPECT_TRUE(viewport_.HasScrollTarget());

    state_.Show();
    ctrl_.OnTextChanged(L"hello", nodes);
    ctrl_.OnNext();

    EXPECT_FALSE(viewport_.HasScrollTarget());
}

// on_scroll_changed には md_rect 全体の高さを渡す。SEARCH_BAR_HEIGHT を引いた値を渡すと
// 他箇所（SyncMaxScroll）との viewport_height の意味論がブレる。
TEST_F(SearchBarControllerTest, ScrollToMatchPassesMdPaneHeightToCallback)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode(L"a"));
    nodes.push_back(MakeTextNode(L"target"));

    cache_.Resize(2);
    cache_[0].text_top = 0.0f;
    cache_[0].height = 1000.0f;
    cache_[1].text_top = 1000.0f;
    cache_[1].height = 1000.0f;

    viewport_.SyncMaxScroll(2000.0f, md_pane_height_);
    state_.Show();
    ctrl_.OnTextChanged(L"target", nodes);
    on_scroll_changed_count_ = 0;
    last_scroll_changed_value_ = -1.0f;
    ctrl_.OnNext();

    ASSERT_GE(on_scroll_changed_count_, 1);
    EXPECT_FLOAT_EQ(last_scroll_changed_value_, md_pane_height_);
}

// 同一ノード内でテーブル複数行のマッチを連続で「次へ」したとき、行ごとに scroll_y が進むこと。
// Why: 行毎の精密な Y が使われず block 先頭に丸まると、複数マッチ間でスクロールが進まない。
TEST_F(SearchBarControllerTest, NextMatchAcrossTableRowsAdvancesScroll)
{
    Node table;
    table.type = NodeType::Table;
    table.ensure_table();
    auto* tbl = table.table_data();
    tbl->row_count = 5;
    tbl->col_count = 1;
    tbl->concat_text = L"hit\nhit\nhit\nhit\nhit";
    tbl->cell_text_starts = { 0u, 4u, 8u, 12u, 16u, 19u };
    tbl->cell_run_starts = { 0u, 0u, 0u, 0u, 0u, 0u };
    tbl->aligns = { TableAlign::Default };
    tbl->is_header_row = { false, false, false, false, false };
    std::pmr::vector<Node> nodes;
    nodes.push_back(std::move(table));

    cache_.Resize(1);
    cache_[0].text_top = 0.0f;
    cache_[0].height = 2000.0f;
    auto& tl = cache_[0].ensure_table_layout();
    tl.col_count = 1;
    tl.row_heights = {400.0f, 400.0f, 400.0f, 400.0f, 400.0f};
    tl.row_cum_y = {0.0f, 400.0f, 800.0f, 1200.0f, 1600.0f, 2000.0f};

    md_pane_height_ = 600.0f;
    viewport_.SyncMaxScroll(2000.0f, md_pane_height_);

    state_.Show();
    ctrl_.OnTextChanged(L"hit", nodes);

    float prev_scroll = viewport_.GetScrollY();
    int advanced = 0;
    for (int i = 0; i < 4; i++) {
        ctrl_.OnNext();
        const float now = viewport_.GetScrollY();
        if (now > prev_scroll) {
            advanced++;
        }
        prev_scroll = now;
    }
    EXPECT_GE(advanced, 1)
        << "行ごとに異なる Y に到達でき、最低1回はスクロールが進むこと";
}

// 現在位置が既に可視範囲内ならスクロールしない（余計な再描画を避ける）。
TEST_F(SearchBarControllerTest, ScrollToMatchNoOpWhenAlreadyVisible)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode(L"hello"));

    cache_.Resize(1);
    cache_[0].text_top = 100.0f;
    cache_[0].height = 50.0f;

    viewport_.SyncMaxScroll(1000.0f, md_pane_height_);
    state_.Show();
    ctrl_.OnTextChanged(L"hello", nodes);

    on_scroll_changed_count_ = 0;
    const float before = viewport_.GetScrollY();
    ctrl_.OnNext();
    EXPECT_FLOAT_EQ(viewport_.GetScrollY(), before);
    EXPECT_EQ(on_scroll_changed_count_, 0);
}
