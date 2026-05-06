#include <gtest/gtest.h>
#include "search_state.h"
#include "test_helpers.h"
#include <memory_resource>

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
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode("hello world"));
    SearchState s;
    s.Show();
    s.SetQuery("hello");
    s.ExecuteSearch(nodes);
    EXPECT_EQ(s.GetMatchCount(), 1);
    s.Hide();
    EXPECT_EQ(s.GetMatchCount(), 0);
    EXPECT_EQ(s.GetCurrentMatchIndex(), -1);
}

TEST(SearchStateTest, ResetClearsQueryAndMatches)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode("hello"));
    SearchState s;
    s.SetQuery("hello");
    s.ExecuteSearch(nodes);
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
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode("hello"));
    SearchState s;
    s.SetQuery("");
    s.ExecuteSearch(nodes);
    EXPECT_EQ(s.GetMatchCount(), 0);
}

TEST(SearchStateTest, SingleMatch)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode("Hello World"));
    SearchState s;
    s.SetQuery("World");
    s.ExecuteSearch(nodes);
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].node_index, 0);
    EXPECT_EQ(s.GetMatches()[0].start, 6u);
    EXPECT_EQ(s.GetMatches()[0].length, 5u);
}

TEST(SearchStateTest, MultipleMatchesInOneNode)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode("abcabcabc"));
    SearchState s;
    s.SetQuery("abc");
    s.ExecuteSearch(nodes);
    ASSERT_EQ(s.GetMatchCount(), 3);
    EXPECT_EQ(s.GetMatches()[0].start, 0u);
    EXPECT_EQ(s.GetMatches()[1].start, 3u);
    EXPECT_EQ(s.GetMatches()[2].start, 6u);
}

TEST(SearchStateTest, MatchesAcrossMultipleNodes)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode("first test"));
    nodes.push_back(MakeTextNode("no match here"));
    nodes.push_back(MakeTextNode("another test"));
    SearchState s;
    s.SetQuery("test");
    s.ExecuteSearch(nodes);
    ASSERT_EQ(s.GetMatchCount(), 2);
    EXPECT_EQ(s.GetMatches()[0].node_index, 0);
    EXPECT_EQ(s.GetMatches()[1].node_index, 2);
}

TEST(SearchStateTest, CaseInsensitiveByDefault)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode("Hello HELLO hello"));
    SearchState s;
    s.SetQuery("hello");
    s.ExecuteSearch(nodes);
    EXPECT_EQ(s.GetMatchCount(), 3);
}

TEST(SearchStateTest, NoMatchReturnsEmpty)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode("hello world"));
    SearchState s;
    s.SetQuery("xyz");
    s.ExecuteSearch(nodes);
    EXPECT_EQ(s.GetMatchCount(), 0);
}

TEST(SearchStateTest, EmptyNodesProducesNoMatches)
{
    std::pmr::vector<Node> nodes;
    SearchState s;
    s.SetQuery("hello");
    s.ExecuteSearch(nodes);
    EXPECT_EQ(s.GetMatchCount(), 0);
}

TEST(SearchStateTest, EmptyTextNodeSkipped)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode(""));
    nodes.push_back(MakeTextNode("hello"));
    SearchState s;
    s.SetQuery("hello");
    s.ExecuteSearch(nodes);
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
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode("Hello HELLO hello"));
    SearchState s;
    s.SetCaseSensitive(true);
    s.SetQuery("Hello");
    s.ExecuteSearch(nodes);
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].start, 0u);
}

TEST(SearchStateTest, CaseSensitiveNoMatch)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode("HELLO"));
    SearchState s;
    s.SetCaseSensitive(true);
    s.SetQuery("hello");
    s.ExecuteSearch(nodes);
    EXPECT_EQ(s.GetMatchCount(), 0);
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
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTableNode("hello", "world"));
    SearchState s;
    s.SetQuery("hello");
    s.ExecuteSearch(nodes);
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].node_index, 0);
    EXPECT_EQ(s.GetMatches()[0].table_row, 0);
    EXPECT_EQ(s.GetMatches()[0].table_col, 0);
    EXPECT_EQ(s.GetMatches()[0].start, 0u);
    EXPECT_EQ(s.GetMatches()[0].length, 5u);
}

TEST(SearchStateTest, TableMultipleCellMatches)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTableNode("test one", "test two"));
    SearchState s;
    s.SetQuery("test");
    s.ExecuteSearch(nodes);
    ASSERT_EQ(s.GetMatchCount(), 2);
    EXPECT_EQ(s.GetMatches()[0].table_col, 0);
    EXPECT_EQ(s.GetMatches()[1].table_col, 1);
}

TEST(SearchStateTest, TableCaseSensitiveMatch)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTableNode("Hello", "hello"));
    SearchState s;
    s.SetCaseSensitive(true);
    s.SetQuery("Hello");
    s.ExecuteSearch(nodes);
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].table_col, 0);
}

TEST(SearchStateTest, MixedNodeAndTableMatches)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode("find me"));
    nodes.push_back(MakeTableNode("find here", "no"));
    SearchState s;
    s.SetQuery("find");
    s.ExecuteSearch(nodes);
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
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode("aaa"));
    SearchState s;
    s.SetQuery("a");
    s.ExecuteSearch(nodes);
    ASSERT_EQ(s.GetMatchCount(), 3);

    // 初期状態は -1
    EXPECT_EQ(s.GetCurrentMatchIndex(), -1);

    EXPECT_FALSE(s.NextMatch()); // -1→0は初回移動（ラップではない）
    EXPECT_EQ(s.GetCurrentMatchIndex(), 0);
    EXPECT_FALSE(s.NextMatch());
    EXPECT_EQ(s.GetCurrentMatchIndex(), 1);
    EXPECT_FALSE(s.NextMatch());
    EXPECT_EQ(s.GetCurrentMatchIndex(), 2);
    // ラップアラウンド
    EXPECT_TRUE(s.NextMatch());
    EXPECT_EQ(s.GetCurrentMatchIndex(), 0);
}

TEST(SearchStateTest, PrevMatchCycles)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode("aaa"));
    SearchState s;
    s.SetQuery("a");
    s.ExecuteSearch(nodes);
    ASSERT_EQ(s.GetMatchCount(), 3);

    // -1からPrevで最後へ（ラップ）
    EXPECT_TRUE(s.PrevMatch());
    EXPECT_EQ(s.GetCurrentMatchIndex(), 2);
    EXPECT_FALSE(s.PrevMatch());
    EXPECT_EQ(s.GetCurrentMatchIndex(), 1);
    EXPECT_FALSE(s.PrevMatch());
    EXPECT_EQ(s.GetCurrentMatchIndex(), 0);
    // ラップアラウンド
    EXPECT_TRUE(s.PrevMatch());
    EXPECT_EQ(s.GetCurrentMatchIndex(), 2);
}

TEST(SearchStateTest, NextMatchNoOpWhenEmpty)
{
    SearchState s;
    EXPECT_FALSE(s.NextMatch());
    EXPECT_EQ(s.GetCurrentMatchIndex(), -1);
}

TEST(SearchStateTest, PrevMatchNoOpWhenEmpty)
{
    SearchState s;
    EXPECT_FALSE(s.PrevMatch());
    EXPECT_EQ(s.GetCurrentMatchIndex(), -1);
}

// ═══════════════════════════════════════════════
// SetCurrentMatchNear
// ═══════════════════════════════════════════════

TEST(SearchStateTest, SetCurrentMatchNearSelectsFirstAfterScroll)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode("first match"));
    nodes.push_back(MakeTextNode("second match"));
    nodes.push_back(MakeTextNode("third match"));
    SearchState s;
    s.SetQuery("match");
    s.ExecuteSearch(nodes);
    ASSERT_EQ(s.GetMatchCount(), 3);

    auto cache = MakeUniformCache(3, 100.0f); // y=0,100,200
    s.SetCurrentMatchNear(150.0f, cache);
    EXPECT_EQ(s.GetCurrentMatchIndex(), 2); // node2 @ y=200
}

TEST(SearchStateTest, SetCurrentMatchNearSelectsFirstWhenAllAbove)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode("match"));
    SearchState s;
    s.SetQuery("match");
    s.ExecuteSearch(nodes);
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
    // 3 行 1 列のテーブル。各行に "hit" が 1 件ずつ含まれる。
    Node table;
    table.type = NodeType::Table;
    table.ensure_table();
    auto* tbl = table.table_data();
    tbl->row_count = 3;
    tbl->col_count = 1;
    tbl->concat_text = "hit\nhit\nhit";
    tbl->cell_text_starts = { 0u, 4u, 8u, 11u };
    tbl->cell_run_starts = { 0u, 0u, 0u, 0u };
    tbl->aligns.push_back(TableAlign::Default);
    tbl->is_header_row = { false, false, false };
    std::pmr::vector<Node> nodes;
    nodes.push_back(std::move(table));

    SearchState s;
    s.SetQuery("hit");
    s.ExecuteSearch(nodes);
    ASSERT_EQ(s.GetMatchCount(), 3);

    // ブロックは y=0, 各行は高さ 100 → 行Y = 0, 100, 200
    LayoutCache cache;
    cache.Resize(1);
    cache[0].text_top = 0.0f;
    cache[0].height = 300.0f;
    auto& tl = cache[0].ensure_table_layout();
    tl.row_heights = { 100.0f, 100.0f, 100.0f };
    tl.row_cum_y = { 0.0f, 100.0f, 200.0f, 300.0f };

    // scroll_y=150 → 行2(y=200) の先頭マッチを選ぶ。
    // ブロック先頭 Y (=0) だけを見る旧実装だと 3 件目以降を探して 0 にフォールバックしていた。
    s.SetCurrentMatchNear(150.0f, cache);
    EXPECT_EQ(s.GetCurrentMatchIndex(), 2);

    // scroll_y=50 → 行1(y=100) のマッチ。
    s.SetCurrentMatchNear(50.0f, cache);
    EXPECT_EQ(s.GetCurrentMatchIndex(), 1);

    // テーブル全体より下にスクロールしても必ずどこかにフォールバック。
    s.SetCurrentMatchNear(1000.0f, cache);
    EXPECT_EQ(s.GetCurrentMatchIndex(), 0);
}

// ═══════════════════════════════════════════════
// 再検索（クエリ変更後の再実行）
// ═══════════════════════════════════════════════

TEST(SearchStateTest, ReExecuteSearchUpdatesMatches)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode("abc def abc"));

    SearchState s;
    s.SetQuery("abc");
    s.ExecuteSearch(nodes);
    EXPECT_EQ(s.GetMatchCount(), 2);

    s.SetQuery("def");
    s.ExecuteSearch(nodes);
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].start, 4u);
}

TEST(SearchStateTest, ReExecuteAfterCaseSensitiveChange)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode("Abc abc ABC"));

    SearchState s;
    s.SetQuery("abc");
    s.ExecuteSearch(nodes);
    EXPECT_EQ(s.GetMatchCount(), 3); // 大文字小文字無視

    s.SetCaseSensitive(true);
    s.ExecuteSearch(nodes);
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].start, 4u);
}

// ═══════════════════════════════════════════════
// 日本語テキスト
// ═══════════════════════════════════════════════

TEST(SearchStateTest, JapaneseTextSearch)
{
    std::pmr::vector<Node> nodes;
    nodes.push_back(MakeTextNode("これはテストです。テスト完了。"));
    SearchState s;
    s.SetQuery("テスト");
    s.ExecuteSearch(nodes);
    ASSERT_EQ(s.GetMatchCount(), 2);
    // UTF-8: 各 BMP CJK 文字 3 byte。"これは" 9 / "テスト" 9 / "です。" 9
    EXPECT_EQ(s.GetMatches()[0].start, 9u);
    EXPECT_EQ(s.GetMatches()[0].length, 9u);
    EXPECT_EQ(s.GetMatches()[1].start, 27u);
}

// ═══════════════════════════════════════════════
// HorizontalRule/Imageなどテキストなしノードのスキップ
// ═══════════════════════════════════════════════

TEST(SearchStateTest, NonTextNodesSkipped)
{
    std::pmr::vector<Node> nodes;
    Node hr;
    hr.type = NodeType::HorizontalRule;
    nodes.push_back(std::move(hr));
    nodes.push_back(MakeTextNode("hello"));
    SearchState s;
    s.SetQuery("hello");
    s.ExecuteSearch(nodes);
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].node_index, 1);
}

// ═══════════════════════════════════════════════
// 画像・Mermaidノードの検索除外
// ═══════════════════════════════════════════════

TEST(SearchStateTest, ImageNodeExcludedFromSearch)
{
    std::pmr::vector<Node> nodes;
    Node img;
    img.type = NodeType::Image;
    img.SetText("alt text with keyword");
    nodes.push_back(std::move(img));
    nodes.push_back(MakeTextNode("keyword in paragraph"));
    SearchState s;
    s.SetQuery("keyword");
    s.ExecuteSearch(nodes);
    // 画像ノードのaltテキストはマッチしない（段落のみマッチ）
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].node_index, 1);
}

TEST(SearchStateTest, MermaidCodeBlockExcludedFromSearch)
{
    std::pmr::vector<Node> nodes;
    Node mermaid;
    mermaid.type = NodeType::CodeBlock;
    mermaid.code_language = SyntaxLanguage::Mermaid;
    mermaid.SetText("graph TD; A-->B");
    nodes.push_back(std::move(mermaid));
    nodes.push_back(MakeTextNode("graph description"));
    SearchState s;
    s.SetQuery("graph");
    s.ExecuteSearch(nodes);
    // Mermaidコードブロックはマッチしない（段落のみマッチ）
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].node_index, 1);
}

TEST(SearchStateTest, NonMermaidCodeBlockIncludedInSearch)
{
    std::pmr::vector<Node> nodes;
    Node code;
    code.type = NodeType::CodeBlock;
    code.code_language = SyntaxLanguage::Cpp;
    code.SetText("int main()");
    nodes.push_back(std::move(code));
    SearchState s;
    s.SetQuery("main");
    s.ExecuteSearch(nodes);
    // Mermaid以外のコードブロックは通常通り検索対象
    ASSERT_EQ(s.GetMatchCount(), 1);
    EXPECT_EQ(s.GetMatches()[0].node_index, 0);
}
