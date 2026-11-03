#include <gtest/gtest.h>
#include "search_state.h"
#include "test_helpers.h"
#include <initializer_list>
#include <utility>
#include "command_generator.h"
#include "draw_cmd_test_util.h"
#include "draw_command.h"
#include "dwrite_test_base.h"
#include <algorithm>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

std::vector<Node> RepeatedTextNodes(const char* text, int count)
{
    std::vector<Node> nodes;
    for (int i = 0; i < count; ++i) {
        nodes.push_back(MakeTextNode(text));
    }
    return nodes;
}

std::vector<Node> Nodes(Node node)
{
    std::vector<Node> nodes;
    nodes.push_back(std::move(node));
    return nodes;
}

SearchState Searched(const std::vector<Node>& nodes, std::string_view query, bool case_sensitive = false)
{
    SearchState s;
    // 既定値のまま検索する経路も通すため、false のときは切り替えない。
    if (case_sensitive) {
        s.ToggleCaseSensitive();
    }
    RunSearchToEnd(s, nodes, query);
    return s;
}

} // namespace

// ═══════════════════════════════════════════════
// 非表示
// ═══════════════════════════════════════════════

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

// ═══════════════════════════════════════════════
// 基本検索
// ═══════════════════════════════════════════════

// 既定は大文字小文字を無視し、区別するときは完全一致だけ。空のノードは飛ばす。
TEST(SearchStateTest, FindsAllMatchesInTextNodes)
{
    struct Expected {
        int node;
        uint32_t start_w;
        uint32_t length_w;
    };
    struct Case {
        const char* name;
        std::vector<Node> nodes;
        const char* query;
        bool case_sensitive;
        std::vector<Expected> expected;
    };
    const Case kCases[] = {
        { "SingleMatch", TextNodes({ "Hello World" }), "World", false, { { 0, 6, 5 } } },
        { "MultipleMatchesInOneNode", TextNodes({ "abcabcabc" }), "abc", false, { { 0, 0, 3 }, { 0, 3, 3 }, { 0, 6, 3 } } },
        { "MatchesAcrossMultipleNodes", TextNodes({ "first test", "no match here", "another test" }), "test", false, { { 0, 6, 4 }, { 2, 8, 4 } } },
        { "CaseInsensitiveByDefault", TextNodes({ "Hello HELLO hello" }), "hello", false, { { 0, 0, 5 }, { 0, 6, 5 }, { 0, 12, 5 } } },
        { "CaseInsensitiveMixedQuery", TextNodes({ "Hello HELLO hello" }), "hELLo", false, { { 0, 0, 5 }, { 0, 6, 5 }, { 0, 12, 5 } } },
        { "NoMatch", TextNodes({ "hello world" }), "xyz", false, {} },
        { "EmptyNodes", {}, "hello", false, {} },
        { "EmptyTextNodeSkipped", TextNodes({ "", "hello" }), "hello", false, { { 1, 0, 5 } } },
        { "CaseSensitiveMatchesExact", TextNodes({ "Hello HELLO hello" }), "Hello", true, { { 0, 0, 5 } } },
        { "CaseSensitiveNoMatch", TextNodes({ "HELLO" }), "hello", true, {} },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        const auto s = Searched(c.nodes, c.query, c.case_sensitive);
        ASSERT_EQ(s.GetMatchCount(), static_cast<int>(c.expected.size()));
        for (size_t k = 0; k < c.expected.size(); ++k) {
            SCOPED_TRACE(k);
            const auto& m = s.GetMatches()[k];
            EXPECT_EQ(m.node_index, c.expected[k].node);
            EXPECT_EQ(m.table_row, -1);
            EXPECT_EQ(m.start_w, c.expected[k].start_w);
            EXPECT_EQ(m.length_w, c.expected[k].length_w);
        }
    }
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
    auto& tl = cache[0].set_table_layout(std::make_unique<TableLayoutData>(3, 1, false));
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
    s.BeginSearch("x");

    EXPECT_FALSE(s.ContinueSearch(nodes, kExpiredDeadline));
    EXPECT_TRUE(s.IsSearching());
    EXPECT_EQ(s.GetMatchCount(), 0);
    EXPECT_FALSE(s.HasResult()) << "初めての検索の途中は 0 件を「見つからない」と見せない";

    while (!s.ContinueSearch(nodes, kExpiredDeadline)) {
    }
    EXPECT_FALSE(s.IsSearching());
    EXPECT_TRUE(s.HasResult());
    ASSERT_EQ(s.GetMatchCount(), 600);
    EXPECT_EQ(s.GetMatches()[599].node_index, 599);
}

// 調べ直している間は前回の結果を見せ続ける (保存のたびの再検索で 0 件に戻ってちらつかない)。
TEST(SearchStateTest, ShowsPreviousMatchesUntilSearchFinishes)
{
    const auto nodes = RepeatedTextNodes("x", 600);
    auto s = Searched(nodes, "x");
    s.NextMatch();
    s.BeginSearch("z");

    EXPECT_FALSE(s.ContinueSearch(nodes, kExpiredDeadline));
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
    s.BeginSearch("w");
    EXPECT_FALSE(s.ContinueSearch(nodes, kExpiredDeadline));
    EXPECT_TRUE(s.HasResult());
    EXPECT_EQ(s.GetMatchCount(), 0);
}

// 空のクエリは調べるまでもなく、すぐ結果を消す。
TEST(SearchStateTest, EmptyQueryClearsMatchesImmediately)
{
    const auto nodes = RepeatedTextNodes("x", 3);
    auto s = Searched(nodes, "x");
    s.BeginSearch("");
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

    RunSearchToEnd(s, nodes, "xy");
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

    RunSearchToEnd(s, nodes, "def");
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].start_w, 4u);
}

TEST(SearchStateTest, ReExecuteAfterCaseSensitiveChange)
{
    const auto nodes = TextNodes({ "Abc abc ABC" });
    auto s = Searched(nodes, "abc");
    EXPECT_EQ(s.GetMatchCount(), 3);

    s.ToggleCaseSensitive();
    RunSearchToEnd(s, nodes, "abc");
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
    img.SetText("alt text with keyword");
    Node mermaid;
    mermaid.type = NodeType::Diagram;
    mermaid.code_language = SyntaxLanguage::Mermaid;
    mermaid.SetText("graph keyword TD; A-->B");

    std::vector<Node> nodes;
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
    code.SetText("int main()");
    const auto s = Searched(Nodes(std::move(code)), "main");
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].node_index, 0);
}

// ═══════════════════════════════════════════════
// テーブル一括走査
// ═══════════════════════════════════════════════

// concat_text 一括走査でもセル区切りをまたぐ一致は拾わない。
TEST(SearchStateTest, TableMatchDoesNotCrossCellBoundary)
{
    const auto nodes = Nodes(MakeTableNode({ { "abc", "def" } }));
    EXPECT_EQ(Searched(nodes, "c\td").GetMatchCount(), 0);
    auto s = Searched(nodes, "cd");
    EXPECT_EQ(s.GetMatchCount(), 0);

    // 同じ状態で表ノードを検索し直しても、セル内の一致は見つかる。
    RunSearchToEnd(s, nodes, "abc");
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
    std::vector<Node> nodes = TextNodes({ "aaab", "xyz", "Aab aab", "b" });
    nodes.push_back(MakeTableNode({ { "aab", "q" }, { "aa", "aaB" } }));
    for (const bool case_sensitive : { false, true }) {
        SCOPED_TRACE(case_sensitive);
        SearchState s = Searched(nodes, "a", case_sensitive);
        for (const char* q : { "aa", "aab" }) {
            SCOPED_TRACE(q);
            RunSearchToEnd(s, nodes, q);
            EXPECT_TRUE(SameMatches(s, Searched(nodes, q, case_sensitive)));
        }
    }
}

// 調べ終わる前に伸ばしたときは、途中までのマッチが全マッチではないので最初から調べる。
TEST(SearchStateTest, ExtendingQueryBeforeSearchFinishesScansAll)
{
    const auto nodes = RepeatedTextNodes("ab", 600);
    SearchState s;
    s.BeginSearch("a");
    ASSERT_FALSE(s.ContinueSearch(nodes, kExpiredDeadline));
    RunSearchToEnd(s, nodes, "ab");
    EXPECT_EQ(s.GetMatchCount(), 600);
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
    RunSearchToEnd(s, nodes, "foo b");
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

// 実セルレイアウト (DWriteTestBase) を使う検索マッチ Y のテスト。
// 表の複数行セルでは (row, col, start) 順に並ぶマッチの Y が単調にならないため、
// モックの固定行高では再現できない。
namespace {

// 1 列目の長文が折り返して "hit" が 3 行目以降に来る一方、同じ行の 2 列目の "hit" は 1 行目にある。
constexpr std::string_view TABLE_MD =
    "| first column | second |\n"
    "|---|---|\n"
    "| this cell has enough words to wrap over several lines before the final hit | hit |\n"
    "| hit | plain |\n"
    "| more words here so the cell wraps again and again before hit | another hit |\n\n";
constexpr float VIEWPORT_W = 420.0f;

// 表の前後のマッチ数を変え、二分探索の中央が表内の逆転箇所に当たる配置を網羅する。
std::string MakeDoc(int leading, int trailing)
{
    std::string md;
    for (int i = 0; i < leading; i++) {
        md += "leading hit paragraph\n\n";
    }
    md += TABLE_MD;
    for (int i = 0; i < trailing; i++) {
        md += "trailing hit paragraph\n\n";
    }
    return md;
}

class SearchMatchYTest : public DWriteTestBase {
protected:
    struct Doc {
        ParsedLayout pl;
        SearchState search;
        std::vector<float> match_ys;
    };

    void Load(Doc& d, const std::string& md)
    {
        d.pl = ParseAndLayout(md, VIEWPORT_W);
        RunSearchToEnd(d.search, d.pl.nodes, "hit");
        for (const auto& m : d.search.GetMatches()) {
            const auto node = static_cast<size_t>(m.node_index);
            d.match_ys.push_back(d.pl.cache[node].GetMatchYRange(m.table_row, m.table_col, m.start_w, d.pl.cache.Top(node)).first);
        }
    }
};

// 仕様: マッチ順で最初に match_y >= scroll_y となるマッチ、無ければ先頭。
int LinearNearest(const std::vector<float>& ys, float scroll_y)
{
    const auto it = std::ranges::find_if(ys, [scroll_y](float y) { return y >= scroll_y; });
    return (it != ys.end()) ? static_cast<int>(it - ys.begin()) : 0;
}

} // namespace

TEST_F(SearchMatchYTest, SetCurrentMatchNearAgreesWithLinearScan)
{
    for (int leading = 0; leading <= 6; leading++) {
        for (int trailing = 0; trailing <= 2; trailing++) {
            SCOPED_TRACE("leading=" + std::to_string(leading) + " trailing=" + std::to_string(trailing));
            Doc d;
            Load(d, MakeDoc(leading, trailing));
            // 前提: 表の同一行内でマッチ順と Y 順が逆転している (二分探索の単調性が崩れる状況)。
            ASSERT_FALSE(std::ranges::is_sorted(d.match_ys));

            std::vector<float> probes{ -10.0f, d.match_ys.back() + 100.0f };
            for (const float y : d.match_ys) {
                for (const float delta : { -2.0f, -0.5f, 0.0f, 0.5f, 2.0f }) {
                    probes.push_back(y + delta);
                }
            }
            for (const float y : probes) {
                d.search.SetCurrentMatchNear(y, d.pl.cache);
                ASSERT_EQ(d.search.GetCurrentMatchIndex(), LinearNearest(d.match_ys, y)) << "scroll_y=" << y;
            }
        }
    }
}

// スクロール位置合わせに使う GetMatchYRange の Y は、実際に描かれるハイライト矩形の上端と一致する。
TEST_F(SearchMatchYTest, MatchYRangeAgreesWithDrawnHighlight)
{
    Doc d;
    Load(d, MakeDoc(1, 1));
    CommandGenerator gen;
    InitWithoutText(gen, theme_);

    const auto& matches = d.search.GetMatches();
    for (int i = 0; i < static_cast<int>(matches.size()); i++) {
        SCOPED_TRACE(i);
        DrawCommandList cmds;
        gen.GenerateMdPane(cmds, { .nodes = d.pl.nodes, .cache = d.pl.cache, .selection = TextSelection{}, .md_pane_rect = PaneRect{ 0.0f, 0.0f, 4000.0f, 4000.0f },
                                                .search_matches = matches, .current_match_index = i }, 1.0f);
        std::optional<float> drawn_top;
        for (const auto& c : cmds) {
            if (const auto* r = std::get_if<FillRectCmd>(&c); r && r->brush_id == BrushId::SearchHighlightCurrent) {
                drawn_top = drawn_top ? std::min(*drawn_top, r->rect.top) : r->rect.top;
            }
        }
        ASSERT_TRUE(drawn_top.has_value());
        EXPECT_NEAR(d.match_ys[static_cast<size_t>(i)], *drawn_top, 0.01f);
    }
}
