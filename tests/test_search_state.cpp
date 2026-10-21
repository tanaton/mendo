#include <gtest/gtest.h>
#include "search_state.h"
#include "test_helpers.h"
#include <initializer_list>
#include <memory_resource>
#include <utility>

namespace {

std::pmr::vector<Node> TextNodes(std::initializer_list<const char*> texts)
{
    std::pmr::vector<Node> nodes;
    for (const char* text : texts) {
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
    s.ExecuteSearch(nodes);
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
    EXPECT_EQ(s.GetMatches()[0].start, 6u);
    EXPECT_EQ(s.GetMatches()[0].length, 5u);
}

TEST(SearchStateTest, MultipleMatchesInOneNode)
{
    const auto s = Searched(TextNodes({ "abcabcabc" }), "abc");
    ASSERT_EQ(s.GetMatchCount(), 3);
    EXPECT_EQ(s.GetMatches()[0].start, 0u);
    EXPECT_EQ(s.GetMatches()[1].start, 3u);
    EXPECT_EQ(s.GetMatches()[2].start, 6u);
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
    EXPECT_EQ(s.GetMatches()[0].start, 0u);
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
    EXPECT_EQ(s.GetMatches()[0].start, 0u);
    EXPECT_EQ(s.GetMatches()[0].length, 5u);
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
    LayoutCache cache;
    cache.Resize(1);
    cache.SetTop(0, 0.0f);
    cache[0].height = 300.0f;
    auto& tl = cache[0].ensure_table_layout();
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

// 期限を過ぎていても、時計を確かめるまでのまとまりは進めてから譲る。マッチは文書順に増え、世代も進む。
TEST(SearchStateTest, ContinueSearchYieldsAtDeadlineAndResumes)
{
    std::pmr::vector<Node> nodes;
    for (int i = 0; i < 600; ++i) {
        nodes.push_back(MakeTextNode("x"));
    }
    SearchState s;
    s.SetQuery("x");
    s.BeginSearch();
    const auto past = std::chrono::steady_clock::now() - std::chrono::seconds(1);

    const uint32_t gen = s.GetGeneration();
    EXPECT_FALSE(s.ContinueSearch(nodes, past));
    EXPECT_TRUE(s.IsSearching());
    const int first = s.GetMatchCount();
    EXPECT_GT(first, 0);
    EXPECT_LT(first, 600);
    EXPECT_NE(s.GetGeneration(), gen) << "途中のマッチもハイライトさせる";

    while (!s.ContinueSearch(nodes, past)) {
    }
    EXPECT_FALSE(s.IsSearching());
    ASSERT_EQ(s.GetMatchCount(), 600);
    EXPECT_EQ(s.GetMatches()[599].node_index, 599);
}

// 止めた検索は続きを進めず、途中までのマッチを残す。
TEST(SearchStateTest, StopSearchKeepsPartialMatches)
{
    std::pmr::vector<Node> nodes;
    for (int i = 0; i < 600; ++i) {
        nodes.push_back(MakeTextNode("x"));
    }
    SearchState s;
    s.SetQuery("x");
    s.BeginSearch();
    s.ContinueSearch(nodes, std::chrono::steady_clock::now() - std::chrono::seconds(1));
    const int partial = s.GetMatchCount();

    s.StopSearch();
    EXPECT_TRUE(s.ContinueSearch(nodes, std::chrono::steady_clock::time_point::max()));
    EXPECT_EQ(s.GetMatchCount(), partial);
}

TEST(SearchStateTest, ReExecuteSearchUpdatesMatches)
{
    const auto nodes = TextNodes({ "abc def abc" });
    auto s = Searched(nodes, "abc");
    EXPECT_EQ(s.GetMatchCount(), 2);

    s.SetQuery("def");
    s.ExecuteSearch(nodes);
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].start, 4u);
}

TEST(SearchStateTest, ReExecuteAfterCaseSensitiveChange)
{
    const auto nodes = TextNodes({ "Abc abc ABC" });
    auto s = Searched(nodes, "abc");
    EXPECT_EQ(s.GetMatchCount(), 3);

    s.ToggleCaseSensitive();
    s.ExecuteSearch(nodes);
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].start, 4u);
}

// ═══════════════════════════════════════════════
// 日本語テキスト
// ═══════════════════════════════════════════════

TEST(SearchStateTest, JapaneseTextSearch)
{
    const auto s = Searched(TextNodes({ "これはテストです。テスト完了。" }), "テスト");
    ASSERT_EQ(s.GetMatchCount(), 2);
    // UTF-8: 各 BMP CJK 文字 3 byte。"これは" 9 / "テスト" 9 / "です。" 9
    EXPECT_EQ(s.GetMatches()[0].start, 9u);
    EXPECT_EQ(s.GetMatches()[0].length, 9u);
    EXPECT_EQ(s.GetMatches()[1].start, 27u);
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
    mermaid.set_code_language(SyntaxLanguage::Mermaid);
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
    code.set_code_language(SyntaxLanguage::Cpp);
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
    s.ExecuteSearch(nodes);
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].table_col, 0);
}

TEST(SearchStateTest, TableMatchesCarryCellLocalOffsets)
{
    const auto s = Searched(Nodes(MakeTableNode({ { "xx Foo", "\xE3\x81\x82" "foo foo" } })), "FOO");
    ASSERT_EQ(s.GetMatchCount(), 3);
    EXPECT_EQ(s.GetMatches()[0].table_col, 0);
    EXPECT_EQ(s.GetMatches()[0].start, 3u);
    EXPECT_EQ(s.GetMatches()[0].start_w, 3u);
    EXPECT_EQ(s.GetMatches()[1].table_col, 1);
    EXPECT_EQ(s.GetMatches()[1].start, 3u);
    EXPECT_EQ(s.GetMatches()[1].start_w, 1u);
    EXPECT_EQ(s.GetMatches()[1].length_w, 3u);
    EXPECT_EQ(s.GetMatches()[2].start, 7u);
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
    EXPECT_EQ(s.GetMatches()[0].start, 7u);
    EXPECT_EQ(s.GetMatches()[0].start_w, 3u) << "サロゲートペア 2 + 'あ' 1";
    EXPECT_EQ(s.GetMatches()[0].length_w, 1u);
}

namespace {
bool SameMatches(const SearchState& a, const SearchState& b)
{
    return std::ranges::equal(a.GetMatches(), b.GetMatches(), [](const SearchMatch& x, const SearchMatch& y) {
        return x.node_index == y.node_index && x.start == y.start && x.length == y.length &&
               x.table_row == y.table_row && x.table_col == y.table_col && x.start_w == y.start_w && x.length_w == y.length_w;
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
            s.ExecuteSearch(nodes);
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
    s.ExecuteSearch(nodes);
    EXPECT_EQ(s.GetMatchCount(), 3);
}
