#include <gtest/gtest.h>
#include <chrono>
#include <iostream>
#include <memory_resource>
#include "command_generator.h"
#include "dwrite_test_base.h"
#include "parser.h"
#include "test_helpers.h"

class LayoutTest : public DWriteTestBase {};

TEST_F(LayoutTest, EmptyNodesProduceZeroHeight)
{
    std::pmr::vector<Node> nodes;
    LayoutCache cache;
    engine_.ComputeLayout(nodes, cache, 800.0f);
    // margin_topのみが寄与
    EXPECT_FLOAT_EQ(engine_.GetTotalHeight(), theme_.margin_top * 2);
}

TEST_F(LayoutTest, SingleParagraphHasPositiveHeight)
{
    auto nodes = ParseMarkdown("Hello world").nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    EXPECT_GT(engine_.GetTotalHeight(), 0.0f);
    EXPECT_GT(cache[0].height, 0.0f);
}

TEST_F(LayoutTest, HeadingIsTallerThanParagraph)
{
    auto heading_nodes = ParseMarkdown("# Big Title").nodes;
    LayoutCache heading_cache;
    heading_cache.Resize(heading_nodes.size());

    auto para_nodes = ParseMarkdown("Small text").nodes;
    LayoutCache para_cache;
    para_cache.Resize(para_nodes.size());

    engine_.ComputeLayout(heading_nodes, heading_cache, 800.0f);
    float heading_height = heading_cache[0].height;

    engine_.ComputeLayout(para_nodes, para_cache, 800.0f);
    float para_height = para_cache[0].height;

    EXPECT_GT(heading_height, para_height);
}

TEST_F(LayoutTest, YPositionsIncreaseMonotonically)
{
    auto nodes = ParseMarkdown("# A\n\nB\n\nC\n\nD").nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(cache[i].text_top, cache[i - 1].text_top)
            << "ノード " << i << " のyはノード " << (i - 1) << " より大きいこと";
    }
}

TEST_F(LayoutTest, NodesDoNotOverlap)
{
    auto nodes = ParseMarkdown("# Heading\n\nParagraph\n\n---\n\n- List").nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    for (size_t i = 1; i < nodes.size(); i++) {
        float prev_bottom = cache[i - 1].text_top + cache[i - 1].height;
        EXPECT_LE(prev_bottom, cache[i].text_top)
            << "ノード " << (i - 1) << " がノード " << i << " と重なっている";
    }
}

// ComputeLayout (full pass) 後に Fenwick が total_height と整合していること。
// 個別の Y は spacing_above ぶん entry.text_top と意味が異なる (Fenwick は
// ブロック上端、entry はテキスト上端) ので、ここでは total のみ確認する。
TEST_F(LayoutTest, FenwickMatchesTotalHeightAfterFullLayout)
{
    auto nodes = ParseMarkdown("# Heading\n\nParagraph\n\n---\n\n- A\n- B\n\nLast").nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f); // partial = false

    EXPECT_NEAR(cache.GetTotalHeightFromFenwick(theme_.margin_top), engine_.GetTotalHeight(), 0.01f);

    // 隣接ノード間の Y 差分は Fenwick の block_height に等しい。
    // (i 番目ブロック上端と i+1 番目ブロック上端の差 = block_height[i])
    for (size_t i = 1; i < nodes.size(); i++) {
        const float fenwick_top_i = cache.GetBlockTop(i, theme_.margin_top);
        const float fenwick_top_prev = cache.GetBlockTop(i - 1, theme_.margin_top);
        EXPECT_GT(fenwick_top_i, fenwick_top_prev) << "ノード " << i;
    }
}

TEST_F(LayoutTest, NarrowViewportWrapsText)
{
    auto nodes_wide = ParseMarkdown("This is a somewhat long paragraph that should wrap.").nodes;
    LayoutCache cache_wide;
    cache_wide.Resize(nodes_wide.size());

    auto nodes_narrow = ParseMarkdown("This is a somewhat long paragraph that should wrap.").nodes;
    LayoutCache cache_narrow;
    cache_narrow.Resize(nodes_narrow.size());

    engine_.ComputeLayout(nodes_wide, cache_wide, 800.0f);
    float wide_height = cache_wide[0].height;

    engine_.ComputeLayout(nodes_narrow, cache_narrow, 200.0f);
    float narrow_height = cache_narrow[0].height;

    // ビューポートが狭いほどテキストが高くなる（折り返しが増える）
    EXPECT_GE(narrow_height, wide_height);
}

TEST_F(LayoutTest, LayoutDirtyFlagCleared)
{
    auto nodes = ParseMarkdown("Test").nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    EXPECT_TRUE(cache[0].layout_dirty);
    engine_.ComputeLayout(nodes, cache, 800.0f);
    EXPECT_FALSE(cache[0].layout_dirty);
}

TEST_F(LayoutTest, TextLayoutCreated)
{
    auto nodes = ParseMarkdown("Test paragraph").nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    EXPECT_NE(cache[0].text_layout.Get(), nullptr);
}

TEST_F(LayoutTest, HorizontalRuleHasNoTextLayout)
{
    auto nodes = ParseMarkdown("---").nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    EXPECT_EQ(cache[0].text_layout.Get(), nullptr);
    EXPECT_GT(cache[0].height, 0.0f);
}

TEST_F(LayoutTest, CodeBlockTextLayout)
{
    auto nodes = ParseMarkdown("```\ncode\n```").nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    EXPECT_NE(cache[0].text_layout.Get(), nullptr);
}

TEST_F(LayoutTest, TableLayout)
{
    auto nodes = ParseMarkdown(
                     "| A | B |\n"
                     "|---|---|\n"
                     "| 1 | 2 |")
                     .nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Table);
    EXPECT_GT(cache[0].height, 0.0f);
    ASSERT_TRUE(cache[0].has_table_layout());
    EXPECT_FALSE(cache[0].table_layout->col_widths.empty());
}

TEST_F(LayoutTest, TableCellLayoutsCreated)
{
    auto nodes = ParseMarkdown(
                     "| A | B |\n"
                     "|---|---|\n"
                     "| 1 | 2 |")
                     .nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    ASSERT_TRUE(cache[0].has_table_layout());
    const auto& tl = *cache[0].table_layout;
    const auto* tbl = nodes[0].table_data();
    for (size_t r = 0; r < tbl->row_count; r++) {
        for (size_t c = 0; c < tbl->col_count; c++) {
            if (!tbl->GetCellText(r, c).empty()) {
                EXPECT_NE(tl.GetCellLayout(r, c), nullptr);
            }
        }
    }
}

TEST_F(LayoutTest, TableCellLinkHasUnderline)
{
    auto nodes = ParseMarkdown(
                     "| Text | Link |\n"
                     "|------|------|\n"
                     "| hello | [click](https://example.com) |")
                     .nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    ASSERT_EQ(nodes.size(), 1u);
    const auto* tbl = nodes[0].table_data();
    ASSERT_GE(tbl->row_count, 2u);

    // データ行の2番目のセルにはリンクランがあり、下線が適用されていること
    ASSERT_TRUE(cache[0].has_table_layout());
    IDWriteTextLayout* cell_layout = cache[0].table_layout->GetCellLayout(1, 1);
    ASSERT_NE(cell_layout, nullptr);

    // セル内にリンクランが存在することを確認
    bool has_link_run = false;
    for (const auto& run : tbl->GetCellRuns(1, 1)) {
        if (run.has_link()) {
            has_link_run = true;

            // テキストレイアウトに下線が適用されていることを確認
            BOOL underline = FALSE;
            cell_layout->GetUnderline(run.start, &underline);
            EXPECT_TRUE(underline) << "テーブルセル内のリンクランには下線があること";
        }
    }
    EXPECT_TRUE(has_link_run);
}

TEST_F(LayoutTest, MultipleHeadingLevelsDecreasingSize)
{
    auto nodes = ParseMarkdown("# H1\n\n## H2\n\n### H3").nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    ASSERT_EQ(nodes.size(), 3u);
    // H1はH2より高く、H2はH3以上であること
    EXPECT_GT(cache[0].height, cache[1].height);
    EXPECT_GE(cache[1].height, cache[2].height);
}

TEST_F(LayoutTest, TotalHeightWithManyNodes)
{
    std::string md;
    for (int i = 0; i < 100; i++) {
        md += "Paragraph " + std::to_string(i) + "\n\n";
    }
    auto nodes = ParseMarkdown(md).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    float total = engine_.GetTotalHeight();
    EXPECT_GT(total, 1000.0f); // 100段落あればかなり高くなるはず

    // 最後のノードの下端が全体の高さ以内であること
    size_t last = nodes.size() - 1;
    EXPECT_LE(cache[last].text_top + cache[last].height, total);
}

// ---- ProcessDirtyBatch テスト ----

TEST_F(LayoutTest, ProcessDirtyBatchCleansNodes)
{
    auto nodes = ParseMarkdown("# A\n\nB\n\nC\n\nD\n\nE").nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    // まず部分的なレイアウトを実行
    engine_.ComputeLayout(nodes, cache, 800.0f, 0.0f, 50.0f);

    // ダーティなノードがあれば処理する
    if (engine_.HasDirtyNodes()) {
        bool more = engine_.ProcessDirtyBatch(nodes, cache, 800.0f, 100);
        // 十分に処理した後、ダーティなノードがなくなること
        EXPECT_FALSE(more);
    }

    // すべてのノードが有効な位置を持つこと
    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(cache[i].text_top, cache[i - 1].text_top);
    }
}

TEST_F(LayoutTest, ProcessDirtyBatchSmallBatch)
{
    // 多数の段落を作成
    std::string md;
    for (int i = 0; i < 50; i++) {
        md += "Paragraph " + std::to_string(i) + "\n\n";
    }
    auto nodes = ParseMarkdown(md).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());

    // 非常に小さなビューポートで部分的なレイアウトを実行
    engine_.ComputeLayout(nodes, cache, 800.0f, 0.0f, 10.0f);

    if (engine_.HasDirtyNodes()) {
        // 一度に5ノードだけ処理
        bool more = engine_.ProcessDirtyBatch(nodes, cache, 800.0f, 5);
        // 50ノードでバッチ=5なら、まだダーティなノードが残るはず
        EXPECT_TRUE(more);
    }
}

// ---- 幅変更の検出 ----

TEST_F(LayoutTest, WidthChangeRecomputesLayouts)
{
    auto nodes = ParseMarkdown("This is a paragraph with some text that might wrap differently.").nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    float height_wide = cache[0].height;

    engine_.ComputeLayout(nodes, cache, 200.0f);
    float height_narrow = cache[0].height;

    // 狭い幅ではテキストが高くなる（折り返しが増える）
    EXPECT_GE(height_narrow, height_wide);
}

// ---- 空のテーブル ----

TEST_F(LayoutTest, EmptyTableMinimalHeight)
{
    Node node;
    node.type = NodeType::Table;
    node.ensure_table();
    // 0 行 0 列の空テーブル (新方式は row_count/col_count 既定で 0)
    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(node));
    LayoutCache cache;
    cache.Resize(nodes.size());

    engine_.ComputeLayout(nodes, cache, 800.0f);
    // 空のテーブルはクラッシュせず、何らかの高さを持つこと
    EXPECT_GE(cache[0].height, 0.0f);
}

// ---- インデントされたノード ----

TEST_F(LayoutTest, IndentedNodesHaveNarrowerWidth)
{
    auto nodes_plain = ParseMarkdown("This is a somewhat long paragraph that wraps.").nodes;
    LayoutCache cache_plain;
    cache_plain.Resize(nodes_plain.size());

    auto nodes_list = ParseMarkdown("- This is a somewhat long paragraph that wraps.").nodes;
    LayoutCache cache_list;
    cache_list.Resize(nodes_list.size());

    engine_.ComputeLayout(nodes_plain, cache_plain, 400.0f);
    float plain_height = cache_plain[0].height;

    engine_.ComputeLayout(nodes_list, cache_list, 400.0f);
    float list_height = cache_list[0].height;

    // リスト項目はインデントされるため、同じテキストでも高くなる（利用可能な幅が狭い）
    EXPECT_GE(list_height, plain_height);
}

// ---- ブロック引用のレイアウト ----

TEST_F(LayoutTest, BlockQuoteLayout)
{
    auto nodes = ParseMarkdown("> Quoted text here").nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_GT(cache[0].height, 0.0f);
    EXPECT_GT(nodes[0].indent_level, 0);
}

// ---- コードブロックの折り返し無効 ----

TEST_F(LayoutTest, CodeBlockDoesNotWrap)
{
    std::string long_line = "```\n";
    for (int i = 0; i < 50; i++)
        long_line += "long_word ";
    long_line += "\n```";

    auto nodes = ParseMarkdown(long_line).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 200.0f); // Very narrow

    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    // コードブロックは折り返さないため、高さは1行分になるはず
    // （おおよそコードフォントの高さ）
    EXPECT_LT(cache[0].height, 100.0f);
}

// ---- 見出しの間隔 ----

TEST_F(LayoutTest, HeadingHasSpacingAboveAndBelow)
{
    auto nodes = ParseMarkdown("Paragraph\n\n# Heading\n\nAnother paragraph").nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    ASSERT_EQ(nodes.size(), 3u);

    // 見出しの上に間隔があること（段落の下端と見出しのyの間隔）
    float para_bottom = cache[0].text_top + cache[0].height;
    float heading_y = cache[1].text_top;
    float gap_above = heading_y - para_bottom;
    EXPECT_GT(gap_above, theme_.paragraph_spacing);

    // 見出しの下に間隔があること
    float heading_bottom = cache[1].text_top + cache[1].height;
    float next_y = cache[2].text_top;
    float gap_below = next_y - heading_bottom;
    EXPECT_GT(gap_below, 0.0f);
}

// ========================================================
// 抽出されたフリー関数のテスト
// ========================================================

// ---- ComputeColumnWidths テスト ----

TEST(ComputeColumnWidthsTest, ProportionalDistributionWhenTooWide)
{
    // 自然幅の合計300、利用可能幅150のみ -> 比例配分
    std::pmr::vector<float> natural = { 100.0f, 100.0f, 100.0f };
    std::pmr::vector<float> widths;
    ComputeColumnWidths(widths, natural, 150.0f, 3);
    ASSERT_EQ(widths.size(), 3u);
    // 自然幅が等しいため、すべての列が均等な幅を得ること
    EXPECT_NEAR(widths[0], widths[1], 0.01f);
    EXPECT_NEAR(widths[1], widths[2], 0.01f);
    // 合計は利用可能幅に近似すること
    float total = widths[0] + widths[1] + widths[2];
    EXPECT_NEAR(total, 150.0f, 1.0f);
}

TEST(ComputeColumnWidthsTest, EvenDistributionWhenFits)
{
    // 自然幅の合計30、利用可能幅300 -> 均等配分
    std::pmr::vector<float> natural = { 10.0f, 10.0f, 10.0f };
    std::pmr::vector<float> widths;
    ComputeColumnWidths(widths, natural, 300.0f, 3);
    ASSERT_EQ(widths.size(), 3u);
    // 均等配分: 各列は少なくとも100であること
    float even = 300.0f / 3.0f;
    for (auto w : widths) {
        EXPECT_GE(w, even - 0.01f);
    }
}

TEST(ComputeColumnWidthsTest, MinimumWidthEnforced)
{
    // 非常に小さな利用可能スペース
    std::pmr::vector<float> natural = { 200.0f, 200.0f };
    std::pmr::vector<float> widths;
    ComputeColumnWidths(widths, natural, 40.0f, 2);
    ASSERT_EQ(widths.size(), 2u);
    // 最小幅は30
    for (auto w : widths) {
        EXPECT_GE(w, 30.0f);
    }
}

TEST(ComputeColumnWidthsTest, UnequalNaturalWidths)
{
    // 列Aは列Bよりはるかに広い
    std::pmr::vector<float> natural = { 300.0f, 100.0f };
    std::pmr::vector<float> widths;
    ComputeColumnWidths(widths, natural, 200.0f, 2);
    ASSERT_EQ(widths.size(), 2u);
    // 列Aは列Bよりも大きな割合を得ること
    EXPECT_GT(widths[0], widths[1]);
}

TEST(ComputeColumnWidthsTest, SingleColumn)
{
    std::pmr::vector<float> natural = { 50.0f };
    std::pmr::vector<float> widths;
    ComputeColumnWidths(widths, natural, 200.0f, 1);
    ASSERT_EQ(widths.size(), 1u);
    EXPECT_GE(widths[0], 50.0f);
}

TEST(ComputeColumnWidthsTest, ZeroNaturalWidths)
{
    std::pmr::vector<float> natural = { 0.0f, 0.0f };
    std::pmr::vector<float> widths;
    ComputeColumnWidths(widths, natural, 200.0f, 2);
    ASSERT_EQ(widths.size(), 2u);
    // それでも有効な幅を生成すること
    for (auto w : widths) {
        EXPECT_GT(w, 0.0f);
    }
}

// ---- RecomputeYPositions テスト ----

TEST(RecomputeYPositionsTest, EmptyNodes)
{
    std::pmr::vector<Node> nodes;
    LayoutCache cache;
    Theme theme = GetLightTheme();
    auto result = RecomputeYPositions(nodes, cache, theme);
    EXPECT_FLOAT_EQ(result.total_height, theme.margin_top * 2);
    EXPECT_FALSE(result.has_dirty_nodes);
}

// ---- ComputeTotalContentHeight テスト ----

TEST(ComputeTotalContentHeightTest, EmptyNodesReturnsZero)
{
    LayoutCache cache;
    // node_count == 0 で size_t のアンダーフローが起きないこと。0を返すべき。
    EXPECT_FLOAT_EQ(ComputeTotalContentHeight(cache, 0, 10.0f), 0.0f);
}

TEST(ComputeTotalContentHeightTest, SingleNode)
{
    LayoutCache cache;
    cache.Resize(1);
    cache[0].text_top = 15.0f;
    cache[0].height = 50.0f;
    EXPECT_FLOAT_EQ(ComputeTotalContentHeight(cache, 1, 15.0f), 80.0f);
}

TEST(ComputeTotalContentHeightTest, MultipleNodes)
{
    LayoutCache cache;
    cache.Resize(3);
    cache[0].text_top = 10.0f;
    cache[0].height = 20.0f;
    cache[1].text_top = 40.0f;
    cache[1].height = 30.0f;
    cache[2].text_top = 80.0f;
    cache[2].height = 25.0f;
    // 最後のノードのみが関係: 80 + 25 + 10 = 115
    EXPECT_FLOAT_EQ(ComputeTotalContentHeight(cache, 3, 10.0f), 115.0f);
}

TEST(RecomputeYPositionsTest, SingleParagraph)
{
    Node node;
    node.type = NodeType::Paragraph;
    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(node));
    LayoutCache cache;
    cache.Resize(nodes.size());
    cache[0].height = 20.0f;
    cache[0].layout_dirty = false;
    Theme theme = GetLightTheme();

    auto result = RecomputeYPositions(nodes, cache, theme);
    EXPECT_FLOAT_EQ(cache[0].text_top, theme.margin_top);
    EXPECT_GT(result.total_height, theme.margin_top + 20.0f);
    EXPECT_FALSE(result.has_dirty_nodes);
}

TEST(RecomputeYPositionsTest, HeadingSpacing)
{
    Node para;
    para.type = NodeType::Paragraph;

    Node heading;
    heading.type = NodeType::Heading;
    heading.ensure_heading()->heading_level = 3; // h3はheading_spacing_below（下線なし）を使う

    Node para2;
    para2.type = NodeType::Paragraph;

    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(para));
    nodes.emplace_back(std::move(heading));
    nodes.emplace_back(std::move(para2));
    LayoutCache cache;
    cache.Resize(nodes.size());
    cache[0].height = 20.0f;
    cache[0].layout_dirty = false;
    cache[1].height = 30.0f;
    cache[1].layout_dirty = false;
    cache[2].height = 20.0f;
    cache[2].layout_dirty = false;

    Theme theme = GetLightTheme();

    RecomputeYPositions(nodes, cache, theme);

    // 見出しの上に追加の間隔があること
    float para_bottom = cache[0].text_top + cache[0].height + theme.paragraph_spacing;
    float heading_y = cache[1].text_top;
    EXPECT_FLOAT_EQ(heading_y, para_bottom + theme.heading_spacing_above);

    // 見出しの後: paragraph_spacingではなくheading_spacing_below
    float heading_bottom = cache[1].text_top + cache[1].height + theme.heading_spacing_below;
    EXPECT_FLOAT_EQ(cache[2].text_top, heading_bottom);
}

TEST(RecomputeYPositionsTest, H1H2UseLargerSpacingBelow)
{
    // h1/h2 は下線を描くため heading_spacing_below_h1h2 が使われ、
    // h3以降は heading_spacing_below が使われることを検証する。
    Node h1;
    h1.type = NodeType::Heading;
    h1.ensure_heading()->heading_level = 1;

    Node p1;
    p1.type = NodeType::Paragraph;

    Node h3;
    h3.type = NodeType::Heading;
    h3.ensure_heading()->heading_level = 3;

    Node p2;
    p2.type = NodeType::Paragraph;

    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(h1));
    nodes.emplace_back(std::move(p1));
    nodes.emplace_back(std::move(h3));
    nodes.emplace_back(std::move(p2));
    LayoutCache cache;
    cache.Resize(nodes.size());
    cache[0].height = 40.0f;
    cache[0].layout_dirty = false;
    cache[1].height = 20.0f;
    cache[1].layout_dirty = false;
    cache[2].height = 30.0f;
    cache[2].layout_dirty = false;
    cache[3].height = 20.0f;
    cache[3].layout_dirty = false;

    Theme theme = GetLightTheme();
    RecomputeYPositions(nodes, cache, theme);

    // h1 の後: heading_spacing_below_h1h2 が使われる
    float h1_gap = cache[1].text_top - (cache[0].text_top + cache[0].height);
    EXPECT_FLOAT_EQ(h1_gap, theme.heading_spacing_below_h1h2);

    // h3 の後: heading_spacing_below（h3以降用）が使われる
    float h3_gap = cache[3].text_top - (cache[2].text_top + cache[2].height);
    EXPECT_FLOAT_EQ(h3_gap, theme.heading_spacing_below);

    // 両者は実際に異なる値であること（テスト対象の分岐が意味を持つ前提）
    EXPECT_GT(theme.heading_spacing_below_h1h2, theme.heading_spacing_below);
}

TEST(RecomputeYPositionsTest, DetectsDirtyNodes)
{
    Node clean;
    clean.type = NodeType::Paragraph;

    Node dirty;
    dirty.type = NodeType::Paragraph;

    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(clean));
    nodes.emplace_back(std::move(dirty));
    LayoutCache cache;
    cache.Resize(nodes.size());
    cache[0].height = 20.0f;
    cache[0].layout_dirty = false;
    cache[1].height = 20.0f;
    cache[1].layout_dirty = true;

    Theme theme = GetLightTheme();

    auto result = RecomputeYPositions(nodes, cache, theme);
    EXPECT_TRUE(result.has_dirty_nodes);
}

TEST(RecomputeYPositionsTest, MonotonicallyIncreasingY)
{
    std::pmr::vector<Node> nodes;
    for (int i = 0; i < 10; i++) {
        Node node;
        node.type = NodeType::Paragraph;
        nodes.emplace_back(std::move(node));
    }
    LayoutCache cache;
    cache.Resize(nodes.size());
    for (int i = 0; i < 10; i++) {
        cache[i].height = 15.0f + static_cast<float>(i);
        cache[i].layout_dirty = false;
    }
    Theme theme = GetLightTheme();
    RecomputeYPositions(nodes, cache, theme);

    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(cache[i].text_top, cache[i - 1].text_top);
    }
}

// ---- EnsureVisibleLayout テスト ----

TEST_F(LayoutTest, EnsureVisibleLayoutFixesDirtyVisibleNodes)
{
    // 複数の段落を作成し、ある幅でフルレイアウトを実行
    std::string md;
    for (int i = 0; i < 20; i++) {
        md += "Paragraph " + std::to_string(i) + "\n\n";
    }
    auto nodes = ParseMarkdown(md).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    // 別の幅で部分的なレイアウトを実行 — 画面外のノードがダーティにマークされる
    engine_.ComputeLayout(nodes, cache, 400.0f, 0.0f, 100.0f);

    // ビューポート外のノードはまだダーティであること
    bool any_dirty = false;
    for (size_t i = 0; i < nodes.size(); i++) {
        if (cache[i].layout_dirty) {
            any_dirty = true;
            break;
        }
    }
    ASSERT_TRUE(any_dirty);

    // 修正パスをテストするため、表示中のノードを手動でダーティにマーク
    cache[0].layout_dirty = true;

    // EnsureVisibleLayoutが表示範囲を修正すること
    bool updated = engine_.EnsureVisibleLayout(nodes, cache, 400.0f, 0.0f, 100.0f);
    EXPECT_TRUE(updated);

    // 表示範囲内のノードはもうダーティでないこと
    for (size_t i = 0; i < nodes.size(); i++) {
        if (cache[i].text_top + cache[i].height < 0.0f)
            continue;
        if (cache[i].text_top > 100.0f)
            break;
        EXPECT_FALSE(cache[i].layout_dirty)
            << "y=" << cache[i].text_top << " の表示ノードがまだダーティ";
    }
}

TEST_F(LayoutTest, EnsureVisibleLayoutReturnsFalseWhenClean)
{
    auto nodes = ParseMarkdown("Hello world").nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    // すべてのノードがクリーンなので、EnsureVisibleLayoutはfalseを返すこと
    bool updated = engine_.EnsureVisibleLayout(nodes, cache, 800.0f, 0.0f, 1000.0f);
    EXPECT_FALSE(updated);
}

TEST_F(LayoutTest, EnsureVisibleLayoutSkipsOffscreenDirtyNodes)
{
    std::string md;
    for (int i = 0; i < 30; i++) {
        md += "Paragraph " + std::to_string(i) + "\n\n";
    }
    auto nodes = ParseMarkdown(md).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f, 0.0f, 50.0f);

    // 処理前のダーティノード数をカウント
    int dirty_before = 0;
    for (size_t i = 0; i < nodes.size(); i++) {
        if (cache[i].layout_dirty)
            dirty_before++;
    }

    // 小さなビューポート範囲のみでEnsureVisibleLayoutを実行
    engine_.EnsureVisibleLayout(nodes, cache, 800.0f, 0.0f, 50.0f);

    // 遠くのダーティノードはダーティのままであること
    int dirty_after = 0;
    for (size_t i = 0; i < nodes.size(); i++) {
        if (cache[i].layout_dirty)
            dirty_after++;
    }
    // 一部のノード（画面外のもの）はまだダーティであること
    EXPECT_GT(dirty_after, 0);
    EXPECT_LE(dirty_after, dirty_before);
}

TEST_F(LayoutTest, EnsureVisibleLayoutRecomputesYPositions)
{
    std::string md;
    for (int i = 0; i < 10; i++) {
        md += "Paragraph " + std::to_string(i) + "\n\n";
    }
    auto nodes = ParseMarkdown(md).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    // 広い幅でフルレイアウト
    engine_.ComputeLayout(nodes, cache, 800.0f);

    // 狭い幅で部分的なレイアウトを実行（画面外をダーティにマーク）
    engine_.ComputeLayout(nodes, cache, 300.0f, 0.0f, 50.0f);

    // EnsureVisibleLayoutがY位置を一貫して更新すること
    engine_.EnsureVisibleLayout(nodes, cache, 300.0f, 0.0f, 50.0f);

    // Y位置が単調増加を維持していること
    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(cache[i].text_top, cache[i - 1].text_top)
            << "ノード " << i << " のyはノード " << (i - 1) << " より大きいこと";
    }
}

TEST_F(LayoutTest, EnsureVisibleLayoutUpdatesTotalHeight)
{
    std::string md;
    for (int i = 0; i < 10; i++) {
        md += "Paragraph " + std::to_string(i) + "\n\n";
    }
    auto nodes = ParseMarkdown(md).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f, 0.0f, 50.0f);

    float height_before = engine_.GetTotalHeight();
    engine_.EnsureVisibleLayout(nodes, cache, 800.0f, 0.0f, 50.0f);
    float height_after = engine_.GetTotalHeight();

    // 表示ノードが再レイアウトされると全体の高さが変わる可能性があるが
    // 正の値を維持すること
    EXPECT_GT(height_after, 0.0f);
    (void)height_before;
}

// 部分モードで不可視ノードの古い height を引きずらないこと（High-1 回帰）。
// ズーム/テーマ変更直後の SyncMaxScroll が stale な total_height_ を読まないことを保証する。
TEST_F(LayoutTest, PartialLayoutRefreshesStaleInvisibleHeights)
{
    std::string md;
    for (int i = 0; i < 30; i++) {
        md += "Paragraph " + std::to_string(i) + "\n\n";
    }
    auto nodes = ParseMarkdown(md).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());

    // フル幅でフルレイアウト → 全ノード正確な height を持つ
    engine_.ComputeLayout(nodes, cache, 800.0f);
    const float baseline_total = engine_.GetTotalHeight();
    ASSERT_GT(baseline_total, 0.0f);

    // 不可視（後方）ノードを「旧テーマで非常に大きかった」状態にしてダーティ化する。
    // これは zoom-out 直後に invisible 領域だけ stale height が残るケースを模す。
    const size_t node_count = nodes.size();
    constexpr float STALE_HEIGHT = 9999.0f;
    for (size_t i = node_count / 2; i < node_count; i++) {
        cache[i].height = STALE_HEIGHT;
        cache[i].layout_dirty = true;
    }

    // 部分レイアウト：可視範囲は先頭わずかのみ。後方の invisible ダーティ群は
    // 現テーマでの推定値に置き換わるはずで、stale な巨大 height は混ざらない。
    engine_.ComputeLayout(nodes, cache, 800.0f, 0.0f, 50.0f);
    const float total_after = engine_.GetTotalHeight();

    // baseline と同程度（推定誤差ぶんはあり得る）に収束し、stale の合計を
    // 引きずった巨大値にはならないこと。
    EXPECT_LT(total_after, baseline_total * 2.0f)
        << "stale height (=" << STALE_HEIGHT << ") を total_height に取り込んでいる";
}

// ---- RecomputeYPositions 追加テスト ----

TEST(RecomputeYPositionsTest, MultipleHeadingsHaveCorrectSpacing)
{
    Theme theme = GetLightTheme();
    Node heading_a;
    heading_a.type = NodeType::Heading;
    heading_a.ensure_heading()->heading_level = 3;

    Node heading_b;
    heading_b.type = NodeType::Heading;
    heading_b.ensure_heading()->heading_level = 3;

    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(heading_a));
    nodes.emplace_back(std::move(heading_b));
    LayoutCache cache;
    cache.Resize(nodes.size());
    cache[0].height = 40.0f;
    cache[0].layout_dirty = false;
    cache[1].height = 30.0f;
    cache[1].layout_dirty = false;

    RecomputeYPositions(nodes, cache, theme);

    // 最初の見出し: margin_top + heading_spacing_above
    EXPECT_FLOAT_EQ(cache[0].text_top, theme.margin_top + theme.heading_spacing_above);

    // 2番目の見出し: 最初の見出しの後 + heading_spacing_below + heading_spacing_above
    float expected_y = cache[0].text_top + cache[0].height + theme.heading_spacing_below + theme.heading_spacing_above;
    EXPECT_FLOAT_EQ(cache[1].text_top, expected_y);
}

TEST(RecomputeYPositionsTest, AllNodeTypesProduceValidPositions)
{
    Theme theme = GetLightTheme();
    std::pmr::vector<Node> nodes;

    auto add_node = [&](NodeType type) {
        Node n;
        n.type = type;
        nodes.emplace_back(std::move(n));
    };

    add_node(NodeType::Paragraph);
    add_node(NodeType::Heading);
    add_node(NodeType::CodeBlock);
    add_node(NodeType::HorizontalRule);
    add_node(NodeType::ListItem);
    add_node(NodeType::BlockQuote);
    add_node(NodeType::Table);

    LayoutCache cache;
    cache.Resize(nodes.size());
    float heights[] = { 20.0f, 30.0f, 50.0f, 5.0f, 18.0f, 25.0f, 60.0f };
    for (size_t i = 0; i < nodes.size(); i++) {
        cache[i].height = heights[i];
        cache[i].layout_dirty = false;
    }

    auto result = RecomputeYPositions(nodes, cache, theme);

    // すべての位置が単調増加であること
    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(cache[i].text_top, cache[i - 1].text_top);
    }
    // 全体の高さが最後のノードの下端を超えること
    size_t last = nodes.size() - 1;
    float last_bottom = cache[last].text_top + cache[last].height;
    EXPECT_GE(result.total_height, last_bottom);
}

// ---- FindFirstVisibleNodeIndex テスト（layout_cache.h のフリー関数） ----

TEST(FindFirstVisibleNodeIndex, AtStart)
{
    auto cache = MakeUniformCache(10, 50.0f);
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 10, 0.0f), 0);
}

TEST(FindFirstVisibleNodeIndex, MidDocument)
{
    auto cache = MakeUniformCache(10, 50.0f);
    // viewport_top = 120 → ノード2 (y=100, bottom=150) が最初の可視ノード
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 10, 120.0f), 2);
}

TEST(FindFirstVisibleNodeIndex, ExactBoundary)
{
    auto cache = MakeUniformCache(10, 50.0f);
    // viewport_top = 50 → ノード0はy=50で終了、ノード1はy=50で開始
    // ノード0の下端(50) == viewport_top(50)なので除外される
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 10, 50.0f), 1);
}

TEST(FindFirstVisibleNodeIndex, PastEnd)
{
    auto cache = MakeUniformCache(5, 50.0f);
    // viewport_top = 300、すべてのノードはy=250で終了
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 5, 300.0f), 5);
}

TEST(FindFirstVisibleNodeIndex, EmptyCache)
{
    LayoutCache cache;
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 0, 0.0f), 0);
}

TEST(FindFirstVisibleNodeIndex, SingleNode)
{
    auto cache = MakeUniformCache(1, 100.0f);
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 1, 0.0f), 0);
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 1, 50.0f), 0);
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 1, 100.0f), 1); // ノードを過ぎた位置
}

TEST(FindFirstVisibleNodeIndex, LastNodeVisible)
{
    auto cache = MakeUniformCache(10, 50.0f);
    // viewport_top = 449 → ノード8は450で終了、まだ可視
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 10, 449.0f), 8);
}

// ---- コードブロックの上下マージン ----

TEST(RecomputeYPositionsTest, CodeBlockHasSpacingAbove)
{
    Theme theme = GetLightTheme();
    Node para;
    para.type = NodeType::Paragraph;
    Node code;
    code.type = NodeType::CodeBlock;

    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(para));
    nodes.emplace_back(std::move(code));
    LayoutCache cache;
    cache.Resize(nodes.size());
    cache[0].height = 20.0f;
    cache[0].layout_dirty = false;
    cache[1].height = 50.0f;
    cache[1].layout_dirty = false;

    RecomputeYPositions(nodes, cache, theme);

    float para_bottom = cache[0].text_top + cache[0].height;
    float gap = cache[1].text_top - para_bottom;
    // paragraph_spacing + code_block_spacing_above
    EXPECT_FLOAT_EQ(gap, theme.paragraph_spacing + theme.code_block_spacing_above);
}

TEST(RecomputeYPositionsTest, CodeBlockHasSpacingBelow)
{
    Theme theme = GetLightTheme();
    Node code;
    code.type = NodeType::CodeBlock;
    Node para;
    para.type = NodeType::Paragraph;

    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(code));
    nodes.emplace_back(std::move(para));
    LayoutCache cache;
    cache.Resize(nodes.size());
    cache[0].height = 50.0f;
    cache[0].layout_dirty = false;
    cache[1].height = 20.0f;
    cache[1].layout_dirty = false;

    RecomputeYPositions(nodes, cache, theme);

    float code_bottom = cache[0].text_top + cache[0].height;
    float gap = cache[1].text_top - code_bottom;
    // コードブロック後: paragraph_spacing + code_block_spacing_above
    EXPECT_FLOAT_EQ(gap, theme.paragraph_spacing + theme.code_block_spacing_above);
}

// ---- 引用ブロックの上部マージン ----

TEST(RecomputeYPositionsTest, BlockQuoteHasSpacingAbove)
{
    Theme theme = GetLightTheme();
    Node para;
    para.type = NodeType::Paragraph;
    Node quote;
    quote.type = NodeType::BlockQuote;

    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(para));
    nodes.emplace_back(std::move(quote));
    LayoutCache cache;
    cache.Resize(nodes.size());
    cache[0].height = 20.0f;
    cache[0].layout_dirty = false;
    cache[1].height = 30.0f;
    cache[1].layout_dirty = false;

    RecomputeYPositions(nodes, cache, theme);

    float para_bottom = cache[0].text_top + cache[0].height;
    float gap = cache[1].text_top - para_bottom;
    // paragraph_spacing + code_block_spacing_above
    EXPECT_FLOAT_EQ(gap, theme.paragraph_spacing + theme.code_block_spacing_above);
}

// ---- リスト項目のスペーシング ----

// 空テキスト LI は issue#237 の修正で sb=0 (loose 扱い)。tight LI 間隔の検証には HasText() が要る。
TEST(RecomputeYPositionsTest, ListItemUsesListItemSpacing)
{
    Theme theme = GetLightTheme();
    Node li1;
    li1.type = NodeType::ListItem;
    SetNodeTextCounted(li1, "a");
    Node li2;
    li2.type = NodeType::ListItem;
    SetNodeTextCounted(li2, "b");

    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(li1));
    nodes.emplace_back(std::move(li2));
    LayoutCache cache;
    cache.Resize(nodes.size());
    cache[0].height = 18.0f;
    cache[0].layout_dirty = false;
    cache[1].height = 18.0f;
    cache[1].layout_dirty = false;

    RecomputeYPositions(nodes, cache, theme);

    float gap = cache[1].text_top - (cache[0].text_top + cache[0].height);
    EXPECT_FLOAT_EQ(gap, theme.list_item_spacing);
}

TEST(RecomputeYPositionsTest, TaskListItemUsesListItemSpacing)
{
    Theme theme = GetLightTheme();
    Node tli1;
    tli1.type = NodeType::TaskListItem;
    SetNodeTextCounted(tli1, "a");
    Node tli2;
    tli2.type = NodeType::TaskListItem;
    SetNodeTextCounted(tli2, "b");

    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(tli1));
    nodes.emplace_back(std::move(tli2));
    LayoutCache cache;
    cache.Resize(nodes.size());
    cache[0].height = 18.0f;
    cache[0].layout_dirty = false;
    cache[1].height = 18.0f;
    cache[1].layout_dirty = false;

    RecomputeYPositions(nodes, cache, theme);

    float gap = cache[1].text_top - (cache[0].text_top + cache[0].height);
    EXPECT_FLOAT_EQ(gap, theme.list_item_spacing);
}

// ---- from_index による途中再開 ----

TEST(RecomputeYPositionsTest, FromIndexCodeBlock)
{
    Theme theme = GetLightTheme();
    Node code;
    code.type = NodeType::CodeBlock;
    Node para;
    para.type = NodeType::Paragraph;

    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(code));
    nodes.emplace_back(std::move(para));
    LayoutCache cache;
    cache.Resize(nodes.size());
    cache[0].height = 50.0f;
    cache[0].layout_dirty = false;
    cache[1].height = 20.0f;
    cache[1].layout_dirty = false;

    // まず全体を計算
    RecomputeYPositions(nodes, cache, theme);
    float expected_y1 = cache[1].text_top;

    // from_index=1 で途中から再計算
    RecomputeYPositions(nodes, cache, theme, 1);
    EXPECT_FLOAT_EQ(cache[1].text_top, expected_y1);
}

TEST(RecomputeYPositionsTest, FromIndexListItem)
{
    Theme theme = GetLightTheme();
    Node li;
    li.type = NodeType::ListItem;
    Node para;
    para.type = NodeType::Paragraph;

    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(li));
    nodes.emplace_back(std::move(para));
    LayoutCache cache;
    cache.Resize(nodes.size());
    cache[0].height = 18.0f;
    cache[0].layout_dirty = false;
    cache[1].height = 20.0f;
    cache[1].layout_dirty = false;

    RecomputeYPositions(nodes, cache, theme);
    float expected_y1 = cache[1].text_top;

    RecomputeYPositions(nodes, cache, theme, 1);
    EXPECT_FLOAT_EQ(cache[1].text_top, expected_y1);
}

// ---- リスト箇条書き記号の垂直位置（実DWriteレイアウト使用） ----

// ---- 見出し内インラインコードのフォントサイズ ----

TEST_F(LayoutTest, InlineCodeInHeadingAllLevels)
{
    for (int level = 1; level <= 6; ++level) {
        std::string md(level, '#');
        md += " Test `code`";

        auto nodes = ParseMarkdown(md).nodes;
        LayoutCache cache;
        cache.Resize(nodes.size());
        engine_.ComputeLayout(nodes, cache, 800.0f);

        ASSERT_EQ(nodes.size(), 1u);
        ASSERT_EQ(nodes[0].type, NodeType::Heading);
        ASSERT_NE(cache[0].text_layout.Get(), nullptr);

        bool found_code_run = false;
        for (const auto& run : nodes[0].runs) {
            if (run.code()) {
                found_code_run = true;
                float code_font_size = 0.0f;
                DWRITE_TEXT_RANGE range{};
                cache[0].text_layout->GetFontSize(run.start, &code_font_size, &range);
                EXPECT_FLOAT_EQ(code_font_size, theme_.font_size_h[level - 1])
                    << "H" << level << " 内のインラインコードのフォントサイズが不一致";
            }
        }
        EXPECT_TRUE(found_code_run)
            << "H" << level << " のインラインコード TextRun が見つからない";
    }
}

TEST_F(LayoutTest, InlineCodeInParagraphUsesCodeFontSize)
{
    // 段落内のインラインコードは従来通り font_size_code を使うこと
    auto nodes = ParseMarkdown("Hello `code` world").nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].type, NodeType::Paragraph);
    ASSERT_NE(cache[0].text_layout.Get(), nullptr);

    for (const auto& run : nodes[0].runs) {
        if (run.code()) {
            float code_font_size = 0.0f;
            DWRITE_TEXT_RANGE range{};
            cache[0].text_layout->GetFontSize(run.start, &code_font_size, &range);
            EXPECT_FLOAT_EQ(code_font_size, theme_.font_size_code)
                << "段落内のインラインコードは font_size_code を使うべき";
        }
    }
}

TEST_F(LayoutTest, InlineCodeInHeadingUsesMonospaceFont)
{
    // 見出し内のインラインコードはフォントサイズは見出しと同じだが、フォントファミリーはモノスペースであること
    auto nodes = ParseMarkdown("# Hello `code`").nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_NE(cache[0].text_layout.Get(), nullptr);

    for (const auto& run : nodes[0].runs) {
        if (run.code()) {
            WCHAR font_name[256] = {};
            DWRITE_TEXT_RANGE range{};
            HRESULT hr = cache[0].text_layout->GetFontFamilyName(
                run.start, font_name, 256, &range);
            ASSERT_TRUE(SUCCEEDED(hr));
            EXPECT_EQ(std::wstring(font_name), theme_.monospace_font)
                << "見出し内のインラインコードはモノスペースフォントを使うべき";
        }
    }
}

// ========================================================
// EstimateNodeHeights テスト
// ========================================================

TEST(EstimateNodeHeightsTest, EmptyNodes)
{
    std::pmr::vector<Node> nodes;
    LayoutCache cache;
    Theme theme = GetLightTheme();
    EstimateNodeHeights(nodes, cache, theme);
    // 空でもクラッシュしないこと
}

TEST(EstimateNodeHeightsTest, SingleParagraph)
{
    Node node;
    node.type = NodeType::Paragraph;
    node.SetTextWithLineCount(std::string_view{ "Hello world" }, 0);
    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(node));
    LayoutCache cache;
    cache.Resize(nodes.size());
    Theme theme = GetLightTheme();

    EstimateNodeHeights(nodes, cache, theme);

    EXPECT_GT(cache[0].height, 0.0f);
    EXPECT_GE(cache[0].text_top, theme.margin_top);
}

TEST(EstimateNodeHeightsTest, YPositionsIncreaseMonotonically)
{
    auto nodes = ParseMarkdown("# A\n\nB\n\nC\n\nD").nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    Theme theme = GetLightTheme();

    EstimateNodeHeights(nodes, cache, theme);

    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(cache[i].text_top, cache[i - 1].text_top)
            << "ノード " << i << " のy_positionがノード " << (i - 1) << " より大きいこと";
    }
}

TEST(EstimateNodeHeightsTest, NodesDoNotOverlap)
{
    auto nodes = ParseMarkdown("# Heading\n\nParagraph\n\n---\n\n- List").nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    Theme theme = GetLightTheme();

    EstimateNodeHeights(nodes, cache, theme);

    for (size_t i = 1; i < nodes.size(); i++) {
        float prev_bottom = cache[i - 1].text_top + cache[i - 1].height;
        EXPECT_LE(prev_bottom, cache[i].text_top)
            << "ノード " << (i - 1) << " がノード " << i << " と重なっている";
    }
}

TEST(EstimateNodeHeightsTest, HeadingHeightScalesWithLevel)
{
    Theme theme = GetLightTheme();

    // H1とH3を推定して、H1の方が高いことを確認
    Node h1;
    h1.type = NodeType::Heading;
    h1.ensure_heading()->heading_level = 1;
    h1.SetTextWithLineCount(std::string_view{ "Title" }, 0);

    Node h3;
    h3.type = NodeType::Heading;
    h3.ensure_heading()->heading_level = 3;
    h3.SetTextWithLineCount(std::string_view{ "Title" }, 0);

    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(h1));
    nodes.emplace_back(std::move(h3));
    LayoutCache cache;
    cache.Resize(nodes.size());

    EstimateNodeHeights(nodes, cache, theme);

    EXPECT_GT(cache[0].height, cache[1].height);
}

TEST(EstimateNodeHeightsTest, CodeBlockScalesWithLineCount)
{
    Theme theme = GetLightTheme();

    Node short_code;
    short_code.type = NodeType::CodeBlock;
    short_code.SetTextWithLineCount(std::string_view{ "line1" }, 0);

    Node long_code;
    long_code.type = NodeType::CodeBlock;
    long_code.SetTextWithLineCount(std::string_view{ "line1\nline2\nline3\nline4\nline5" }, 4);

    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(short_code));
    nodes.emplace_back(std::move(long_code));
    LayoutCache cache;
    cache.Resize(nodes.size());

    EstimateNodeHeights(nodes, cache, theme);

    EXPECT_GT(cache[1].height, cache[0].height);
}

TEST(EstimateNodeHeightsTest, HorizontalRuleHasFixedHeight)
{
    Theme theme = GetLightTheme();
    Node hr;
    hr.type = NodeType::HorizontalRule;

    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(hr));
    LayoutCache cache;
    cache.Resize(nodes.size());

    EstimateNodeHeights(nodes, cache, theme);

    EXPECT_FLOAT_EQ(cache[0].height, theme.paragraph_spacing + theme.hr_thickness);
}

TEST(EstimateNodeHeightsTest, ImageHasMinimumHeight)
{
    Theme theme = GetLightTheme();
    Node img;
    img.type = NodeType::Image;

    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(img));
    LayoutCache cache;
    cache.Resize(nodes.size());

    EstimateNodeHeights(nodes, cache, theme);

    EXPECT_GE(cache[0].height, 60.0f);
}

TEST(EstimateNodeHeightsTest, TableScalesWithRowCount)
{
    Theme theme = GetLightTheme();

    Node table1;
    table1.type = NodeType::Table;
    table1.ensure_table();
    table1.table_data()->row_count = 1;

    Node table3;
    table3.type = NodeType::Table;
    table3.ensure_table();
    table3.table_data()->row_count = 3;

    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(table1));
    nodes.emplace_back(std::move(table3));
    LayoutCache cache;
    cache.Resize(nodes.size());

    EstimateNodeHeights(nodes, cache, theme);

    EXPECT_GT(cache[1].height, cache[0].height);
}

TEST(EstimateNodeHeightsTest, EmptyTextNodeUsesSpacing)
{
    Theme theme = GetLightTheme();
    Node node;
    node.type = NodeType::Paragraph;
    // textは空

    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(node));
    LayoutCache cache;
    cache.Resize(nodes.size());

    EstimateNodeHeights(nodes, cache, theme);

    EXPECT_FLOAT_EQ(cache[0].height, theme.paragraph_spacing);
}

TEST(EstimateNodeHeightsTest, MultilineParagraphScalesWithLines)
{
    Theme theme = GetLightTheme();

    Node single;
    single.type = NodeType::Paragraph;
    single.SetTextWithLineCount(std::string_view{ "one line" }, 0);

    Node multi;
    multi.type = NodeType::Paragraph;
    multi.SetTextWithLineCount(std::string_view{ "line1\nline2\nline3" }, 2);

    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(single));
    nodes.emplace_back(std::move(multi));
    LayoutCache cache;
    cache.Resize(nodes.size());

    EstimateNodeHeights(nodes, cache, theme);

    EXPECT_GT(cache[1].height, cache[0].height);
}

TEST(EstimateNodeHeightsTest, LayoutDirtyNotChanged)
{
    Theme theme = GetLightTheme();
    Node node;
    node.type = NodeType::Paragraph;
    node.SetTextWithLineCount(std::string_view{ "test" }, 0);

    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(node));
    LayoutCache cache;
    cache.Resize(nodes.size());

    // layout_dirty はデフォルトで true
    ASSERT_TRUE(cache[0].layout_dirty);

    EstimateNodeHeights(nodes, cache, theme);

    // EstimateNodeHeights は layout_dirty を変更しないこと
    EXPECT_TRUE(cache[0].layout_dirty);
}

TEST(EstimateNodeHeightsTest, AllNodeTypesProducePositiveHeight)
{
    Theme theme = GetLightTheme();
    std::pmr::vector<Node> nodes;

    auto add_node = [&](NodeType type, const char* text = "content") {
        Node n;
        n.type = type;
        SetNodeTextCounted(n, std::string_view{ text });
        if (type == NodeType::Heading) {
            n.ensure_heading()->heading_level = 2;
        }
        if (type == NodeType::Table) {
            n.ensure_table();
            n.table_data()->row_count = 1;
        }
        nodes.emplace_back(std::move(n));
    };

    add_node(NodeType::Paragraph);
    add_node(NodeType::Heading);
    add_node(NodeType::CodeBlock);
    add_node(NodeType::HorizontalRule, "");
    add_node(NodeType::ListItem);
    add_node(NodeType::BlockQuote);
    add_node(NodeType::Table);
    add_node(NodeType::TaskListItem);
    add_node(NodeType::Image, "");

    LayoutCache cache;
    cache.Resize(nodes.size());

    EstimateNodeHeights(nodes, cache, theme);

    for (size_t i = 0; i < nodes.size(); i++) {
        EXPECT_GT(cache[i].height, 0.0f)
            << "ノードタイプ " << static_cast<int>(nodes[i].type) << " の高さが正であること";
    }
}

TEST_F(LayoutTest, EstimateVsActualHeightReasonableRange)
{
    // 推定値がDirectWrite実測値と比べて極端に乖離しないことを確認する
    auto nodes = ParseMarkdown("# Heading\n\nParagraph text\n\n```\ncode\n```\n\n---").nodes;
    LayoutCache est_cache;
    est_cache.Resize(nodes.size());

    LayoutCache actual_cache;
    actual_cache.Resize(nodes.size());

    EstimateNodeHeights(nodes, est_cache, theme_);
    engine_.ComputeLayout(nodes, actual_cache, 800.0f);

    for (size_t i = 0; i < nodes.size(); i++) {
        // 推定値は実測値の0.2倍〜5倍の範囲内であること
        if (actual_cache[i].height > 0.0f) {
            float ratio = est_cache[i].height / actual_cache[i].height;
            EXPECT_GT(ratio, 0.2f) << "ノード " << i << " の推定値が実測値に対して小さすぎる";
            EXPECT_LT(ratio, 5.0f) << "ノード " << i << " の推定値が実測値に対して大きすぎる";
        }
    }
}

TEST_F(LayoutTest, UnorderedListBulletCenteredWithRealLayout)
{
    auto nodes = ParseMarkdown("- Item text here").nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    ASSERT_NE(cache[0].text_layout.Get(), nullptr);
    DWRITE_LINE_METRICS lm;
    UINT32 lc;
    ASSERT_TRUE(SUCCEEDED(cache[0].text_layout->GetLineMetrics(&lm, 1, &lc)));
    ASSERT_GT(lc, 0u);

    // bullet 中心は物理ピクセル境界へスナップされるため、期待値も同じ規則でスナップする。
    float expected_y = SnapToPhysicalPixel(cache[0].text_top + lm.height * 0.5f, 1.0f);

    CommandGenerator gen;
    gen.SetTheme(&theme_);
    gen.SetFormats({ nullptr, nullptr, nullptr });
    PaneRect md_pane{ 0, 0, 800.0f, 2000.0f };
    auto cmds = gen.GenerateMdPane(nodes, cache, md_pane, 0.0f, TextSelection{});

    for (const auto& cmd : cmds) {
        if (auto* e = std::get_if<FillEllipseCmd>(&cmd)) {
            EXPECT_NEAR(e->center.y, expected_y, 0.01f)
                << "箇条書き記号は1行目の中央に配置されるべき";
            return;
        }
    }
    FAIL() << "FillEllipseCmd が見つからない";
}

// issue#237: loose list の LI (空) と直下 Paragraph の text_top は一致すべき。
TEST_F(LayoutTest, LooseListBulletAlignsWithFollowingParagraphText)
{
    auto nodes = ParseMarkdown("- a\n\n- b").nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    ASSERT_EQ(nodes.size(), 4u);
    ASSERT_EQ(nodes[0].type, NodeType::ListItem);
    ASSERT_EQ(nodes[1].type, NodeType::Paragraph);
    ASSERT_EQ(nodes[2].type, NodeType::ListItem);
    ASSERT_EQ(nodes[3].type, NodeType::Paragraph);

    EXPECT_NEAR(cache[0].text_top, cache[1].text_top, 0.01f);
    EXPECT_NEAR(cache[2].text_top, cache[3].text_top, 0.01f);
}

// 空 LI の first_line_height はフォールバック (font_size*FALLBACK_LINE_HEIGHT_FACTOR) なので
// 実 line metrics と完全一致しない → epsilon 3px で許容。
TEST_F(LayoutTest, LooseListBulletCenteredOnFollowingParagraphLine)
{
    auto nodes = ParseMarkdown("- a\n\n- b").nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    ASSERT_EQ(nodes.size(), 4u);
    ASSERT_NE(cache[1].text_layout.Get(), nullptr);
    DWRITE_LINE_METRICS lm;
    UINT32 lc;
    ASSERT_TRUE(SUCCEEDED(cache[1].text_layout->GetLineMetrics(&lm, 1, &lc)));
    ASSERT_GT(lc, 0u);

    CommandGenerator gen;
    gen.SetTheme(&theme_);
    gen.SetFormats({ nullptr, nullptr, nullptr });
    PaneRect md_pane{ 0, 0, 800.0f, 2000.0f };
    auto cmds = gen.GenerateMdPane(nodes, cache, md_pane, 0.0f, TextSelection{});

    const float expected_y = cache[1].text_top + lm.height * 0.5f;
    bool found = false;
    for (const auto& cmd : cmds) {
        if (auto* e = std::get_if<FillEllipseCmd>(&cmd)) {
            EXPECT_NEAR(e->center.y, expected_y, 3.0f);
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "FillEllipseCmd が見つからない";
}

// 旧実装は checkbox 描画を GenNodeTextDecorations に置いており、loose の空 TaskListItem
// (text_layout=nullptr) で early return され checkbox ごと消えていた。
TEST_F(LayoutTest, LooseTaskListCheckboxIsEmitted)
{
    auto nodes = ParseMarkdown("- [ ] a\n\n- [x] b").nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    ASSERT_EQ(nodes.size(), 4u);
    EXPECT_EQ(nodes[0].type, NodeType::TaskListItem);
    EXPECT_FALSE(nodes[0].HasText());
    EXPECT_EQ(nodes[2].type, NodeType::TaskListItem);
    EXPECT_FALSE(nodes[2].HasText());

    // checkbox 描画には formats_.icon_font (非 null) が必要。
    Microsoft::WRL::ComPtr<IDWriteTextFormat> icon_fmt;
    ASSERT_TRUE(SUCCEEDED(dwrite_factory_->CreateTextFormat(
        L"Segoe UI Symbol", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        theme_.font_size_body, L"", &icon_fmt)));

    CommandGenerator gen;
    gen.SetTheme(&theme_);
    gen.SetFormats({ nullptr, icon_fmt.Get(), nullptr });
    PaneRect md_pane{ 0, 0, 800.0f, 2000.0f };
    auto cmds = gen.GenerateMdPane(nodes, cache, md_pane, 0.0f, TextSelection{});

    int unchecked = 0;
    int checked = 0;
    for (const auto& cmd : cmds) {
        if (auto* t = std::get_if<DrawTextCmd>(&cmd); t && t->text_len == 1) {
            const wchar_t ch = t->text()[0];
            if (ch == L'☐') {
                ++unchecked;
            }
            else if (ch == L'☑') {
                ++checked;
            }
        }
    }
    EXPECT_EQ(unchecked, 1);
    EXPECT_EQ(checked, 1);
}

// 22000 ノード × 200 iter で RecomputeYPositions のフルパス (from_index=0) 経過時間を測る。
// 通常の test 走行から外すため DISABLED_ プレフィックス。実行は次のコマンドで:
//   build/tests/Release/mendo_tests.exe --gtest_filter='RecomputeYPositionsTest.DISABLED_BenchLargeDocument' --gtest_also_run_disabled_tests
TEST(RecomputeYPositionsTest, DISABLED_BenchLargeDocument)
{
    using namespace mendo::layout;
    constexpr int N = 22000;
    constexpr int ITER = 200;

    std::pmr::vector<Node> nodes;
    nodes.resize(N);
    for (int i = 0; i < N; i++) {
        switch (i % 5) {
        case 0:
            nodes[i].type = NodeType::Heading;
            nodes[i].ensure_heading()->heading_level = 2;
            break;
        case 1:
            nodes[i].type = NodeType::Paragraph;
            break;
        case 2:
            nodes[i].type = NodeType::CodeBlock;
            break;
        case 3:
            nodes[i].type = NodeType::ListItem;
            break;
        case 4:
            nodes[i].type = NodeType::HorizontalRule;
            break;
        }
    }

    Theme theme = MakeLayoutTestTheme();

    LayoutCache cache;
    cache.Resize(N);
    EstimateNodeHeights(nodes, cache, theme);

    // ウォームアップ
    for (int i = 0; i < 5; i++) {
        RecomputeYPositions(nodes, cache, theme);
    }

    auto start = std::chrono::high_resolution_clock::now();
    for (int iter = 0; iter < ITER; iter++) {
        RecomputeYPositions(nodes, cache, theme);
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    std::cout << "[BENCH] RecomputeYPositions N=" << N
              << " ITER=" << ITER
              << " total=" << elapsed_us << "us"
              << " avg=" << (static_cast<double>(elapsed_us) / ITER) << "us/iter\n";

    // EnsureVisibleLayout の実ユースケース: 可視範囲 (5 ノード) だけ height を弄り、
    // [from_index=100, safe_exit_after=104] で呼ぶ。tail [105, N) は shift-only パスを通る。
    constexpr size_t kFromIndex = 100;
    constexpr size_t kSafeExitAfter = 104;
    for (int i = 0; i < 5; i++) {
        cache[kFromIndex + static_cast<size_t>(i)].height += 1.0f; // 高さを揺らして delta != 0 にする
        RecomputeYPositions(nodes, cache, theme, kFromIndex, false, kSafeExitAfter);
    }
    auto start2 = std::chrono::high_resolution_clock::now();
    for (int iter = 0; iter < ITER; iter++) {
        cache[kFromIndex].height += (iter % 2 == 0) ? 0.5f : -0.5f;
        RecomputeYPositions(nodes, cache, theme, kFromIndex, false, kSafeExitAfter);
    }
    auto end2 = std::chrono::high_resolution_clock::now();
    auto elapsed2_us = std::chrono::duration_cast<std::chrono::microseconds>(end2 - start2).count();
    std::cout << "[BENCH] RecomputeYPositions (tail-shift) N=" << N
              << " from=" << kFromIndex << " safe_exit=" << kSafeExitAfter
              << " ITER=" << ITER
              << " total=" << elapsed2_us << "us"
              << " avg=" << (static_cast<double>(elapsed2_us) / ITER) << "us/iter\n";
}
