#include <gtest/gtest.h>
#include "search_state.h"
#include "test_helpers.h"
#include <initializer_list>
#include <memory_resource>
#include <utility>

namespace {

std::pmr::vector<Node> RepeatedTextNodes(const char* text, int count)
{
    std::pmr::vector<Node> nodes;
    for (int i = 0; i < count; ++i) {
        nodes.push_back(MakeTextNode(text));
    }
    return nodes;
}

std::pmr::vector<Node> Nodes(Node node)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(std::move(node));
    return nodes;
}

SearchState Searched(const std::pmr::vector<Node>& nodes, std::string_view query, bool case_sensitive = false)
{
    SearchState s;
    // 既定値のまま検索する経路も通すため、false のときは切り替えない。
    if (case_sensitive) {
        s.ToggleCaseSensitive();
    }
    s.SetQuery(query);
    RunSearchToEnd(s, nodes);
    return s;
}

} // namespace

// ═══════════════════════════════════════════════
// 表示/非表示の基本操作
// ═══════════════════════════════════════════════

TEST(SearchStateTest, InitiallyNotVisible)
{
    SearchState s;
    EXPECT_FALSE(s.IsVisible());
    EXPECT_TRUE(s.GetQuery().empty());
    EXPECT_EQ(s.GetMatchCount(), 0);
    EXPECT_EQ(s.GetCurrentMatchIndex(), -1);
}

TEST(SearchStateTest, ShowAndHide)
{
    SearchState s;
    s.Show();
    EXPECT_TRUE(s.IsVisible());
    s.Hide();
    EXPECT_FALSE(s.IsVisible());
}

TEST(SearchStateTest, HidePreservesQuery)
{
    SearchState s;
    s.Show();
    s.SetQuery("hello");
    s.Hide();
    EXPECT_EQ(s.GetQuery(), "hello");
}

TEST(SearchStateTest, HideClearsMatches)
{
    auto s = Searched(TextNodes({ "hello world" }), "hello");
    s.Show();
    EXPECT_EQ(s.GetMatchCount(), 1);
    s.Hide();
    EXPECT_EQ(s.GetMatchCount(), 0);
    EXPECT_EQ(s.GetCurrentMatchIndex(), -1);
    EXPECT_EQ(s.GetMatches().capacity(), 0u) << "使わない間は結果の確保を返す";
}

TEST(SearchStateTest, ResetClearsQueryAndMatches)
{
    auto s = Searched(TextNodes({ "hello" }), "hello");
    EXPECT_EQ(s.GetMatchCount(), 1);
    s.Reset();
    EXPECT_TRUE(s.GetQuery().empty());
    EXPECT_EQ(s.GetMatchCount(), 0);
    EXPECT_EQ(s.GetCurrentMatchIndex(), -1);
}

// ═══════════════════════════════════════════════
// 基本検索（大文字小文字無視）
// ═══════════════════════════════════════════════

TEST(SearchStateTest, EmptyQueryProducesNoMatches)
{
    EXPECT_EQ(Searched(TextNodes({ "hello" }), "").GetMatchCount(), 0);
}

TEST(SearchStateTest, SingleMatch)
{
    const auto s = Searched(TextNodes({ "Hello World" }), "World");
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].node_index, 0);
    EXPECT_EQ(s.GetMatches()[0].start_w, 6u);
    EXPECT_EQ(s.GetMatches()[0].length_w, 5u);
}

TEST(SearchStateTest, MultipleMatchesInOneNode)
{
    const auto s = Searched(TextNodes({ "abcabcabc" }), "abc");
    ASSERT_EQ(s.GetMatchCount(), 3);
    EXPECT_EQ(s.GetMatches()[0].start_w, 0u);
    EXPECT_EQ(s.GetMatches()[1].start_w, 3u);
    EXPECT_EQ(s.GetMatches()[2].start_w, 6u);
}

TEST(SearchStateTest, MatchesAcrossMultipleNodes)
{
    const auto s = Searched(TextNodes({ "first test", "no match here", "another test" }), "test");
    ASSERT_EQ(s.GetMatchCount(), 2);
    EXPECT_EQ(s.GetMatches()[0].node_index, 0);
    EXPECT_EQ(s.GetMatches()[1].node_index, 2);
}

TEST(SearchStateTest, CaseInsensitiveByDefault)
{
    EXPECT_EQ(Searched(TextNodes({ "Hello HELLO hello" }), "hello").GetMatchCount(), 3);
}

TEST(SearchStateTest, NoMatchReturnsEmpty)
{
    EXPECT_EQ(Searched(TextNodes({ "hello world" }), "xyz").GetMatchCount(), 0);
}

TEST(SearchStateTest, EmptyNodesProducesNoMatches)
{
    EXPECT_EQ(Searched(TextNodes({}), "hello").GetMatchCount(), 0);
}

TEST(SearchStateTest, EmptyTextNodeSkipped)
{
    const auto s = Searched(TextNodes({ "", "hello" }), "hello");
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].node_index, 1);
}

// ═══════════════════════════════════════════════
// 大文字小文字区別
// ═══════════════════════════════════════════════

TEST(SearchStateTest, CaseSensitiveDefaultOff)
{
    SearchState s;
    EXPECT_FALSE(s.IsCaseSensitive());
}

TEST(SearchStateTest, CaseSensitiveToggle)
{
    SearchState s;
    s.ToggleCaseSensitive();
    EXPECT_TRUE(s.IsCaseSensitive());
    s.ToggleCaseSensitive();
    EXPECT_FALSE(s.IsCaseSensitive());
}

TEST(SearchStateTest, CaseSensitiveMatchesExact)
{
    const auto s = Searched(TextNodes({ "Hello HELLO hello" }), "Hello", true);
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].start_w, 0u);
}

TEST(SearchStateTest, CaseSensitiveNoMatch)
{
    EXPECT_EQ(Searched(TextNodes({ "HELLO" }), "hello", true).GetMatchCount(), 0);
}

// ═══════════════════════════════════════════════
// ハイライトON/OFF
// ═══════════════════════════════════════════════

TEST(SearchStateTest, HighlightDefaultOn)
{
    SearchState s;
    EXPECT_TRUE(s.IsHighlightEnabled());
}

TEST(SearchStateTest, HighlightToggle)
{
    SearchState s;
    s.ToggleHighlightEnabled();
    EXPECT_FALSE(s.IsHighlightEnabled());
    s.ToggleHighlightEnabled();
    EXPECT_TRUE(s.IsHighlightEnabled());
}

// ═══════════════════════════════════════════════
// テーブル検索
// ═══════════════════════════════════════════════

TEST(SearchStateTest, TableCellMatch)
{
    const auto s = Searched(Nodes(MakeTableNode({ { "hello", "world" } })), "hello");
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].node_index, 0);
    EXPECT_EQ(s.GetMatches()[0].table_row, 0);
    EXPECT_EQ(s.GetMatches()[0].table_col, 0);
    EXPECT_EQ(s.GetMatches()[0].start_w, 0u);
    EXPECT_EQ(s.GetMatches()[0].length_w, 5u);
}

TEST(SearchStateTest, TableMultipleCellMatches)
{
    const auto s = Searched(Nodes(MakeTableNode({ { "test one", "test two" } })), "test");
    ASSERT_EQ(s.GetMatchCount(), 2);
    EXPECT_EQ(s.GetMatches()[0].table_col, 0);
    EXPECT_EQ(s.GetMatches()[1].table_col, 1);
}

TEST(SearchStateTest, TableCaseSensitiveMatch)
{
    const auto s = Searched(Nodes(MakeTableNode({ { "Hello", "hello" } })), "Hello", true);
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].table_col, 0);
}

TEST(SearchStateTest, MixedNodeAndTableMatches)
{
    auto nodes = TextNodes({ "find me" });
    nodes.push_back(MakeTableNode({ { "find here", "no" } }));
    const auto s = Searched(nodes, "find");
    ASSERT_EQ(s.GetMatchCount(), 2);
    EXPECT_EQ(s.GetMatches()[0].node_index, 0);
    EXPECT_EQ(s.GetMatches()[0].table_row, -1); // 通常ノード
    EXPECT_EQ(s.GetMatches()[1].node_index, 1);
    EXPECT_EQ(s.GetMatches()[1].table_row, 0); // テーブル
}

// ═══════════════════════════════════════════════
// マッチナビゲーション
// ═══════════════════════════════════════════════

TEST(SearchStateTest, NextMatchCycles)
{
    auto s = Searched(TextNodes({ "aaa" }), "a");
    ASSERT_EQ(s.GetMatchCount(), 3);

    EXPECT_EQ(s.GetCurrentMatchIndex(), -1);

    EXPECT_FALSE(s.NextMatch()); // -1→0は初回移動（ラップではない）
    EXPECT_EQ(s.GetCurrentMatchIndex(), 0);
    EXPECT_FALSE(s.NextMatch());
    EXPECT_EQ(s.GetCurrentMatchIndex(), 1);
    EXPECT_FALSE(s.NextMatch());
    EXPECT_EQ(s.GetCurrentMatchIndex(), 2);
    EXPECT_TRUE(s.NextMatch());
    EXPECT_EQ(s.GetCurrentMatchIndex(), 0);
}

TEST(SearchStateTest, PrevMatchCycles)
{
    auto s = Searched(TextNodes({ "aaa" }), "a");
    ASSERT_EQ(s.GetMatchCount(), 3);

    // -1からPrevで最後へ（ラップ）
    EXPECT_TRUE(s.PrevMatch());
    EXPECT_EQ(s.GetCurrentMatchIndex(), 2);
    EXPECT_FALSE(s.PrevMatch());
    EXPECT_EQ(s.GetCurrentMatchIndex(), 1);
    EXPECT_FALSE(s.PrevMatch());
    EXPECT_EQ(s.GetCurrentMatchIndex(), 0);
    EXPECT_TRUE(s.PrevMatch());
    EXPECT_EQ(s.GetCurrentMatchIndex(), 2);
}

TEST(SearchStateTest, NextAndPrevMatchNoOpWhenEmpty)
{
    SearchState s;
    EXPECT_FALSE(s.NextMatch());
    EXPECT_EQ(s.GetCurrentMatchIndex(), -1);
    EXPECT_FALSE(s.PrevMatch());
    EXPECT_EQ(s.GetCurrentMatchIndex(), -1);
}

// ═══════════════════════════════════════════════
// SetCurrentMatchNear
// ═══════════════════════════════════════════════

TEST(SearchStateTest, SetCurrentMatchNearSelectsFirstAfterScroll)
{
    auto s = Searched(TextNodes({ "first match", "second match", "third match" }), "match");
    ASSERT_EQ(s.GetMatchCount(), 3);

    auto cache = MakeUniformCache(3, 100.0f); // y=0,100,200
    s.SetCurrentMatchNear(150.0f, cache);
    EXPECT_EQ(s.GetCurrentMatchIndex(), 2); // node2 @ y=200
}

TEST(SearchStateTest, SetCurrentMatchNearSelectsFirstWhenAllAbove)
{
    auto s = Searched(TextNodes({ "match" }), "match");
    ASSERT_EQ(s.GetMatchCount(), 1);

    auto cache = MakeUniformCache(1, 100.0f); // y=0
    s.SetCurrentMatchNear(500.0f, cache);
    EXPECT_EQ(s.GetCurrentMatchIndex(), 0); // フォールバック
}

TEST(SearchStateTest, SetCurrentMatchNearNoMatches)
{
    SearchState s;
    LayoutCache cache;
    s.SetCurrentMatchNear(0.0f, cache);
    EXPECT_EQ(s.GetCurrentMatchIndex(), -1);
}

// issue #97: 縦長テーブル内で下方にスクロール中にマッチを確定する際、
// ブロック先頭 Y ではなく該当行の Y を評価してマッチを選択する。
TEST(SearchStateTest, SetCurrentMatchNearUsesTableRowY)
{
    auto s = Searched(Nodes(MakeTableNode({ { "hit" }, { "hit" }, { "hit" } })), "hit");
    ASSERT_EQ(s.GetMatchCount(), 3);

    // ブロックは y=0, 各行は高さ 100 → 行Y = 0, 100, 200
    LayoutCache cache(1);
    cache.SetTop(0, 0.0f);
    cache[0].height = 300.0f;
    auto& tl = cache[0].ensure_table_layout(3, 1);
    tl.row_heights = { 100.0f, 100.0f, 100.0f };
    tl.row_cum_y = { 0.0f, 100.0f, 200.0f, 300.0f };

    // scroll_y=150 → 行2(y=200) の先頭マッチを選ぶ。
    // ブロック先頭 Y (=0) だけを見る旧実装だと 3 件目以降を探して 0 にフォールバックしていた。
    s.SetCurrentMatchNear(150.0f, cache);
    EXPECT_EQ(s.GetCurrentMatchIndex(), 2);

    s.SetCurrentMatchNear(50.0f, cache);
    EXPECT_EQ(s.GetCurrentMatchIndex(), 1);

    // テーブル全体より下にスクロールしても必ずどこかにフォールバック。
    s.SetCurrentMatchNear(1000.0f, cache);
    EXPECT_EQ(s.GetCurrentMatchIndex(), 0);
}

// ═══════════════════════════════════════════════
// 再検索（クエリ変更後の再実行）
// ═══════════════════════════════════════════════

// 期限を過ぎていても、時計を確かめるまでのまとまりは進めてから譲る。
// 途中のマッチは見せず、調べ終えたときに文書順の全マッチへ差し替える。
TEST(SearchStateTest, ContinueSearchYieldsAtDeadlineAndResumes)
{
    const auto nodes = RepeatedTextNodes("x", 600);
    SearchState s;
    s.SetQuery("x");
    s.BeginSearch();
    const auto past = std::chrono::steady_clock::now() - std::chrono::seconds(1);

    const uint32_t gen = s.GetGeneration();
    EXPECT_FALSE(s.ContinueSearch(nodes, past));
    EXPECT_TRUE(s.IsSearching());
    EXPECT_EQ(s.GetMatchCount(), 0);
    EXPECT_FALSE(s.HasResult()) << "初めての検索の途中は 0 件を「見つからない」と見せない";
    EXPECT_EQ(s.GetGeneration(), gen) << "途中ではハイライトを作り直させない";

    while (!s.ContinueSearch(nodes, past)) {
    }
    EXPECT_FALSE(s.IsSearching());
    EXPECT_TRUE(s.HasResult());
    EXPECT_NE(s.GetGeneration(), gen);
    ASSERT_EQ(s.GetMatchCount(), 600);
    EXPECT_EQ(s.GetMatches()[599].node_index, 599);
}

// 調べ直している間は前回の結果を見せ続ける (保存のたびの再検索で 0 件に戻ってちらつかない)。
TEST(SearchStateTest, ShowsPreviousMatchesUntilSearchFinishes)
{
    const auto nodes = RepeatedTextNodes("x", 600);
    auto s = Searched(nodes, "x");
    s.NextMatch();
    s.SetQuery("z");
    s.BeginSearch();

    EXPECT_FALSE(s.ContinueSearch(nodes, std::chrono::steady_clock::now() - std::chrono::seconds(1)));
    EXPECT_EQ(s.GetMatchCount(), 600);
    EXPECT_EQ(s.GetCurrentMatchIndex(), 0);

    EXPECT_TRUE(s.ContinueSearch(nodes, std::chrono::steady_clock::time_point::max()));
    EXPECT_EQ(s.GetMatchCount(), 0);
    EXPECT_EQ(s.GetCurrentMatchIndex(), -1);
}

// 前回が 0 件なら、調べ直している間も 0 件の結果として見せ続ける (赤背景が途中で消えない)。
TEST(SearchStateTest, KeepsZeroResultWhileResearching)
{
    const auto nodes = RepeatedTextNodes("x", 600);
    auto s = Searched(nodes, "z");
    ASSERT_TRUE(s.HasResult());
    s.SetQuery("w");
    s.BeginSearch();
    EXPECT_FALSE(s.ContinueSearch(nodes, std::chrono::steady_clock::now() - std::chrono::seconds(1)));
    EXPECT_TRUE(s.HasResult());
    EXPECT_EQ(s.GetMatchCount(), 0);
}

// 止めた検索は続きを進めず、前回の結果を残す。
TEST(SearchStateTest, StopSearchKeepsPreviousMatches)
{
    const auto nodes = RepeatedTextNodes("x", 600);
    auto s = Searched(nodes, "x");
    s.SetQuery("z");
    s.BeginSearch();
    s.ContinueSearch(nodes, std::chrono::steady_clock::now() - std::chrono::seconds(1));

    s.StopSearch();
    EXPECT_TRUE(s.ContinueSearch(nodes, std::chrono::steady_clock::time_point::max()));
    EXPECT_EQ(s.GetMatchCount(), 600);
}

// 空のクエリは調べるまでもなく、すぐ結果を消す。
TEST(SearchStateTest, EmptyQueryClearsMatchesImmediately)
{
    const auto nodes = RepeatedTextNodes("x", 3);
    auto s = Searched(nodes, "x");
    s.SetQuery("");
    s.BeginSearch();
    EXPECT_FALSE(s.IsSearching());
    EXPECT_EQ(s.GetMatchCount(), 0);
}

// リロードで中身の変わらなかった先頭と末尾のノードのマッチだけを、新しい番号で残す。
TEST(SearchStateTest, KeepMatchesOfUnchangedNodesRemapsTail)
{
    const auto nodes = RepeatedTextNodes("x", 5);
    auto s = Searched(nodes, "x");
    for (int i = 0; i < 4; ++i) {
        s.NextMatch();
    }
    ASSERT_EQ(s.GetCurrentMatchIndex(), 3);

    // 旧 [0,1 | 2 | 3,4] → 新 [0,1 | 2,3 | 4,5]
    s.KeepMatchesOfUnchangedNodes({ .head = 2, .tail = 2, .old_count = 5, .new_count = 6 });

    ASSERT_EQ(s.GetMatchCount(), 4);
    const std::vector<int> expected{ 0, 1, 4, 5 };
    for (size_t k = 0; k < expected.size(); ++k) {
        EXPECT_EQ(s.GetMatches()[k].node_index, expected[k]);
    }
    EXPECT_EQ(s.GetCurrentMatchIndex(), 2) << "旧ノード 3 のマッチを選んだまま";
}

// 選んでいたマッチが変わった範囲にあれば選択を外す。残したマッチは全マッチではないので絞り込みに使わない。
TEST(SearchStateTest, KeepMatchesOfUnchangedNodesDropsChangedRange)
{
    const auto nodes = RepeatedTextNodes("xy", 5);
    auto s = Searched(nodes, "x");
    for (int i = 0; i < 3; ++i) {
        s.NextMatch();
    }
    ASSERT_EQ(s.GetCurrentMatchIndex(), 2);

    s.KeepMatchesOfUnchangedNodes({ .head = 1, .tail = 1, .old_count = 5, .new_count = 5 });
    EXPECT_EQ(s.GetMatchCount(), 2);
    EXPECT_EQ(s.GetCurrentMatchIndex(), -1);
    EXPECT_TRUE(s.HasResult());

    s.SetQuery("xy");
    RunSearchToEnd(s, nodes);
    EXPECT_EQ(s.GetMatchCount(), 5);
}

// 残すマッチが無くなったら、変わった範囲にまだあるかもしれないので「見つからない」と見せない。
TEST(SearchStateTest, KeepMatchesOfUnchangedNodesWithNothingLeftHasNoResult)
{
    const auto nodes = RepeatedTextNodes("x", 3);
    auto s = Searched(nodes, "x");
    s.KeepMatchesOfUnchangedNodes({ .head = 0, .tail = 0, .old_count = 3, .new_count = 3 });
    EXPECT_EQ(s.GetMatchCount(), 0);
    EXPECT_FALSE(s.HasResult());
}

TEST(SearchStateTest, ReExecuteSearchUpdatesMatches)
{
    const auto nodes = TextNodes({ "abc def abc" });
    auto s = Searched(nodes, "abc");
    EXPECT_EQ(s.GetMatchCount(), 2);

    s.SetQuery("def");
    RunSearchToEnd(s, nodes);
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].start_w, 4u);
}

TEST(SearchStateTest, ReExecuteAfterCaseSensitiveChange)
{
    const auto nodes = TextNodes({ "Abc abc ABC" });
    auto s = Searched(nodes, "abc");
    EXPECT_EQ(s.GetMatchCount(), 3);

    s.ToggleCaseSensitive();
    RunSearchToEnd(s, nodes);
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].start_w, 4u);
}

// ═══════════════════════════════════════════════
// 日本語テキスト
// ═══════════════════════════════════════════════

TEST(SearchStateTest, JapaneseTextSearch)
{
    const auto s = Searched(TextNodes({ "これはテストです。テスト完了。" }), "テスト");
    ASSERT_EQ(s.GetMatchCount(), 2);
    // UTF-16: 各 BMP CJK 文字 1 code unit。"これは" 3 / "テスト" 3 / "です。" 3
    EXPECT_EQ(s.GetMatches()[0].start_w, 3u);
    EXPECT_EQ(s.GetMatches()[0].length_w, 3u);
    EXPECT_EQ(s.GetMatches()[1].start_w, 9u);
}

// ═══════════════════════════════════════════════
// テキストを持たないノード・画像・Mermaid の扱い
// ═══════════════════════════════════════════════

TEST(SearchStateTest, NonTextNodesSkipped)
{
    Node hr;
    hr.type = NodeType::HorizontalRule;
    auto nodes = Nodes(std::move(hr));
    nodes.push_back(MakeTextNode("hello"));
    const auto s = Searched(nodes, "hello");
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].node_index, 1);
}

// 画像の alt と Mermaid のソースは描画されないので一致させない。段落の一致だけが残る。
TEST(SearchStateTest, BitmapNodesExcludedFromSearch)
{
    Node img;
    img.type = NodeType::Image;
    img.SetText("alt text with keyword", 0);
    Node mermaid;
    mermaid.type = NodeType::Diagram;
    mermaid.code_language = SyntaxLanguage::Mermaid;
    mermaid.SetText("graph keyword TD; A-->B", 0);

    std::pmr::vector<Node> nodes;
    nodes.push_back(std::move(img));
    nodes.push_back(std::move(mermaid));
    nodes.push_back(MakeTextNode("keyword in paragraph"));
    const auto s = Searched(nodes, "keyword");
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].node_index, 2);
}

TEST(SearchStateTest, NonMermaidCodeBlockIncludedInSearch)
{
    Node code;
    code.type = NodeType::CodeBlock;
    code.code_language = SyntaxLanguage::Cpp;
    code.SetText("int main()", 0);
    const auto s = Searched(Nodes(std::move(code)), "main");
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].node_index, 0);
}

// ═══════════════════════════════════════════════
// テーブル一括走査 / 大文字小文字無視の畳み込み
// ═══════════════════════════════════════════════

// concat_text 一括走査でもセル区切りをまたぐ一致は拾わない。
TEST(SearchStateTest, TableMatchDoesNotCrossCellBoundary)
{
    const auto nodes = Nodes(MakeTableNode({ { "abc", "def" } }));
    EXPECT_EQ(Searched(nodes, "c\td").GetMatchCount(), 0);
    auto s = Searched(nodes, "cd");
    EXPECT_EQ(s.GetMatchCount(), 0);

    // 同じ状態で表ノードを検索し直しても、セル内の一致は見つかる。
    s.SetQuery("abc");
    RunSearchToEnd(s, nodes);
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].table_col, 0);
}

TEST(SearchStateTest, TableMatchesCarryCellLocalOffsets)
{
    const auto s = Searched(Nodes(MakeTableNode({ { "xx Foo", "\xE3\x81\x82" "foo foo" } })), "FOO");
    ASSERT_EQ(s.GetMatchCount(), 3);
    EXPECT_EQ(s.GetMatches()[0].table_col, 0);
    EXPECT_EQ(s.GetMatches()[0].start_w, 3u);
    EXPECT_EQ(s.GetMatches()[1].table_col, 1);
    EXPECT_EQ(s.GetMatches()[1].start_w, 1u);
    EXPECT_EQ(s.GetMatches()[1].length_w, 3u);
    EXPECT_EQ(s.GetMatches()[2].start_w, 5u);
}

TEST(SearchStateTest, CaseInsensitiveFoldsAsciiOnTheFly)
{
    EXPECT_EQ(Searched(TextNodes({ "Hello HELLO hello" }), "hELLo").GetMatchCount(), 3);
}

TEST(SearchStateTest, JapaneseMatchUtf16Offsets)
{
    const auto s = Searched(TextNodes({ "\xF0\x9F\x98\x80\xE3\x81\x82\xE3\x81\x84" }), "\xE3\x81\x84");
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].start_w, 3u) << "サロゲートペア 2 + 'あ' 1";
    EXPECT_EQ(s.GetMatches()[0].length_w, 1u);
}

namespace {
bool SameMatches(const SearchState& a, const SearchState& b)
{
    return std::ranges::equal(a.GetMatches(), b.GetMatches(), [](const SearchMatch& x, const SearchMatch& y) {
        return x.node_index == y.node_index && x.table_row == y.table_row && x.table_col == y.table_col &&
               x.start_w == y.start_w && x.length_w == y.length_w;
    });
}
} // namespace

// クエリを伸ばしたときは前回マッチのあったノードだけを調べ直すが、結果は最初から調べた場合と同じになる。
// "aa" の貪欲なマッチは重なる位置を飛ばすので、マッチ位置ではなくノード単位で絞る必要がある。
TEST(SearchStateTest, ExtendingQueryMatchesFullSearch)
{
    std::pmr::vector<Node> nodes = TextNodes({ "aaab", "xyz", "Aab aab", "b" });
    nodes.push_back(MakeTableNode({ { "aab", "q" }, { "aa", "aaB" } }));
    for (const bool case_sensitive : { false, true }) {
        SCOPED_TRACE(case_sensitive);
        SearchState s = Searched(nodes, "a", case_sensitive);
        for (const char* q : { "aa", "aab" }) {
            SCOPED_TRACE(q);
            s.SetQuery(q);
            RunSearchToEnd(s, nodes);
            EXPECT_TRUE(SameMatches(s, Searched(nodes, q, case_sensitive)));
        }
    }
}

// 調べ終わる前に伸ばしたときは、前回の結果が全マッチではないので最初から調べる。
TEST(SearchStateTest, ExtendingQueryAfterStoppedSearchScansAll)
{
    const auto nodes = TextNodes({ "ab", "ab", "ab" });
    SearchState s;
    s.SetQuery("a");
    s.BeginSearch();
    s.StopSearch();
    s.SetQuery("ab");
    RunSearchToEnd(s, nodes);
    EXPECT_EQ(s.GetMatchCount(), 3);
}

// ═══════════════════════════════════════════════
// 区切ったノードの継ぎ目
// ═══════════════════════════════════════════════

// 改行を挟まない継ぎ目をまたぐマッチは前のノードに置き、続きのノードへはみ出す長さを持つ。
TEST(SearchStateTest, MatchAcrossChunkSeam)
{
    const auto nodes = ChunkedTextNodes({ "abc fo", "o bar" });
    const auto s = Searched(nodes, "foo");
    ASSERT_EQ(s.GetMatchCount(), 1);
    const auto& m = s.GetMatches()[0];
    EXPECT_EQ(m.node_index, 0);
    EXPECT_EQ(m.start_w, 4u);
    EXPECT_EQ(m.length_w, 2u);
    EXPECT_EQ(m.next_length_w, 1u);
}

// またいだマッチと重なる位置は、続きのノードで数え直さない。
TEST(SearchStateTest, MatchAcrossChunkSeamDoesNotOverlap)
{
    const auto nodes = ChunkedTextNodes({ "xo", "oo" });
    const auto s = Searched(nodes, "oo");
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].node_index, 0);
    EXPECT_EQ(s.GetMatches()[0].next_length_w, 1u);
}

// 位置は UTF-16 で数え、大文字小文字を無視する比べ方でもまたいだマッチを拾う。
TEST(SearchStateTest, MatchAcrossChunkSeamUtf16AndCaseFold)
{
    const auto nodes = ChunkedTextNodes({ "日本Fo", "O語" });
    const auto s = Searched(nodes, "本foo語");
    ASSERT_EQ(s.GetMatchCount(), 1);
    const auto& m = s.GetMatches()[0];
    EXPECT_EQ(m.start_w, 1u);
    EXPECT_EQ(m.length_w, 3u);
    EXPECT_EQ(m.next_length_w, 2u);
}

// 改行を挟む継ぎ目は、1 行の検索語ではまたがない。
TEST(SearchStateTest, NewlineChunkSeamIsNotJoined)
{
    const auto nodes = ChunkedTextNodes({ "abc fo", "o bar" }, /*newline_before=*/true);
    EXPECT_EQ(Searched(nodes, "foo").GetMatchCount(), 0);
}

// 前回のクエリを伸ばした絞り込みでも、継ぎ目をまたぐマッチを拾う。
TEST(SearchStateTest, NarrowedSearchFindsMatchAcrossChunkSeam)
{
    const auto nodes = ChunkedTextNodes({ "zz fo", "o bar", "fo" });
    auto s = Searched(nodes, "fo");
    ASSERT_EQ(s.GetMatchCount(), 2);
    s.SetQuery("foo b");
    RunSearchToEnd(s, nodes);
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].node_index, 0);
    EXPECT_EQ(s.GetMatches()[0].next_length_w, 3u);
}

// 変わった範囲の先頭のノードへはみ出したマッチは、リロードで残さない。
TEST(SearchStateTest, KeepMatchesOfUnchangedNodesDropsMatchSpillingIntoChangedRange)
{
    const auto nodes = ChunkedTextNodes({ "a fo", "o b", "foo" });
    auto s = Searched(nodes, "foo");
    ASSERT_EQ(s.GetMatchCount(), 2);
    s.KeepMatchesOfUnchangedNodes({ .head = 1, .tail = 1, .old_count = 3, .new_count = 3 });
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].node_index, 2);
}
