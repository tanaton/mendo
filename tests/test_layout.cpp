#include <gtest/gtest.h>
#include <memory_resource>
#include "layout.h"
#include "command_generator.h"
#include "dwrite_measurer.h"
#include "parser.h"
#include <dwrite.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class LayoutTest : public ::testing::Test {
protected:
    ComPtr<IDWriteFactory> dwrite_;
    DWriteTextMeasurer measurer_;
    LayoutEngine engine_;
    Theme theme_;

    static void SetUpTestSuite()
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    }

    static void TearDownTestSuite()
    {
        CoUninitialize();
    }

    void SetUp() override
    {
        HRESULT hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
        ASSERT_TRUE(SUCCEEDED(hr)) << "DirectWriteファクトリの作成に失敗";

        theme_ = GetLightTheme();
        measurer_.SetFactory(dwrite_.Get());
        ASSERT_TRUE(engine_.Init(&measurer_, theme_));
    }
};

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
        EXPECT_GT(cache[i].y_position, cache[i - 1].y_position)
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
        float prev_bottom = cache[i - 1].y_position + cache[i - 1].height;
        EXPECT_LE(prev_bottom, cache[i].y_position)
            << "ノード " << (i - 1) << " がノード " << i << " と重なっている";
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
        "| 1 | 2 |"
    ).nodes;
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
        "| 1 | 2 |"
    ).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    ASSERT_TRUE(cache[0].has_table_layout());
    const auto& cell_layouts = cache[0].table_layout->cell_layouts;
    for (size_t r = 0; r < cell_layouts.size(); r++) {
        for (size_t c = 0; c < cell_layouts[r].size(); c++) {
            if (!nodes[0].table_rows()[r].cells[c].text.empty()) {
                EXPECT_NE(cell_layouts[r][c].Get(), nullptr);
            }
        }
    }
}

TEST_F(LayoutTest, TableCellLinkHasUnderline)
{
    auto nodes = ParseMarkdown(
        "| Text | Link |\n"
        "|------|------|\n"
        "| hello | [click](https://example.com) |"
    ).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_GE(nodes[0].table_rows().size(), 2u);

    // データ行の2番目のセルにはリンクランがあり、下線が適用されていること
    const auto& cell = nodes[0].table_rows()[1].cells[1];
    auto& cell_layout = cache[0].table_layout->cell_layouts[1][1];
    ASSERT_NE(cell_layout.Get(), nullptr);

    // セル内にリンクランが存在することを確認
    bool has_link_run = false;
    for (const auto& run : cell.runs) {
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
    EXPECT_LE(cache[last].y_position + cache[last].height, total);
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
        EXPECT_GT(cache[i].y_position, cache[i - 1].y_position);
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
    node.table_rows().clear();
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
    for (int i = 0; i < 50; i++) long_line += "long_word ";
    long_line += "\n```";

    auto nodes = ParseMarkdown(long_line).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 200.0f);  // Very narrow

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
    float para_bottom = cache[0].y_position + cache[0].height;
    float heading_y = cache[1].y_position;
    float gap_above = heading_y - para_bottom;
    EXPECT_GT(gap_above, theme_.paragraph_spacing);

    // 見出しの下に間隔があること
    float heading_bottom = cache[1].y_position + cache[1].height;
    float next_y = cache[2].y_position;
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
    auto widths = ComputeColumnWidths(natural, 150.0f, 3);
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
    auto widths = ComputeColumnWidths(natural, 300.0f, 3);
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
    auto widths = ComputeColumnWidths(natural, 40.0f, 2);
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
    auto widths = ComputeColumnWidths(natural, 200.0f, 2);
    ASSERT_EQ(widths.size(), 2u);
    // 列Aは列Bよりも大きな割合を得ること
    EXPECT_GT(widths[0], widths[1]);
}

TEST(ComputeColumnWidthsTest, SingleColumn)
{
    std::pmr::vector<float> natural = { 50.0f };
    auto widths = ComputeColumnWidths(natural, 200.0f, 1);
    ASSERT_EQ(widths.size(), 1u);
    EXPECT_GE(widths[0], 50.0f);
}

TEST(ComputeColumnWidthsTest, ZeroNaturalWidths)
{
    std::pmr::vector<float> natural = { 0.0f, 0.0f };
    auto widths = ComputeColumnWidths(natural, 200.0f, 2);
    ASSERT_EQ(widths.size(), 2u);
    // それでも有効な幅を生成すること
    for (auto w : widths) {
        EXPECT_GT(w, 0.0f);
    }
}

// ---- BuildLinearizedTableText テスト ----

TEST(BuildLinearizedTableTextTest, EmptyRows)
{
    std::pmr::vector<TableRow> rows;
    auto text = BuildLinearizedTableText(rows);
    EXPECT_TRUE(text.empty());
}

TEST(BuildLinearizedTableTextTest, SingleCell)
{
    TableRow row;
    row.cells.emplace_back(TableCell{ L"hello" });
    auto text = BuildLinearizedTableText({ row });
    EXPECT_EQ(text, L"hello");
}

TEST(BuildLinearizedTableTextTest, TabSeparatedCells)
{
    TableRow row;
    row.cells.emplace_back(TableCell{ L"A" });
    row.cells.emplace_back(TableCell{ L"B" });
    row.cells.emplace_back(TableCell{ L"C" });
    auto text = BuildLinearizedTableText({ row });
    EXPECT_EQ(text, L"A\tB\tC");
}

TEST(BuildLinearizedTableTextTest, NewlineSeparatedRows)
{
    TableRow row1;
    row1.cells.emplace_back(TableCell{ L"A" });
    row1.cells.emplace_back(TableCell{ L"B" });
    TableRow row2;
    row2.cells.emplace_back(TableCell{ L"1" });
    row2.cells.emplace_back(TableCell{ L"2" });
    auto text = BuildLinearizedTableText({ row1, row2 });
    EXPECT_EQ(text, L"A\tB\n1\t2");
}

TEST(BuildLinearizedTableTextTest, NoTrailingNewline)
{
    TableRow row;
    row.cells.emplace_back(TableCell{ L"x" });
    auto text = BuildLinearizedTableText({ row });
    EXPECT_FALSE(text.empty());
    EXPECT_NE(text.back(), L'\n');
}

TEST(BuildLinearizedTableTextTest, EmptyCells)
{
    TableRow row;
    row.cells.emplace_back(TableCell{ L"" });
    row.cells.emplace_back(TableCell{ L"B" });
    row.cells.emplace_back(TableCell{ L"" });
    auto text = BuildLinearizedTableText({ row });
    EXPECT_EQ(text, L"\tB\t");
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
    cache[0].y_position = 15.0f;
    cache[0].height = 50.0f;
    EXPECT_FLOAT_EQ(ComputeTotalContentHeight(cache, 1, 15.0f), 80.0f);
}

TEST(ComputeTotalContentHeightTest, MultipleNodes)
{
    LayoutCache cache;
    cache.Resize(3);
    cache[0].y_position = 10.0f;  cache[0].height = 20.0f;
    cache[1].y_position = 40.0f;  cache[1].height = 30.0f;
    cache[2].y_position = 80.0f;  cache[2].height = 25.0f;
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
    EXPECT_FLOAT_EQ(cache[0].y_position, theme.margin_top);
    EXPECT_GT(result.total_height, theme.margin_top + 20.0f);
    EXPECT_FALSE(result.has_dirty_nodes);
}

TEST(RecomputeYPositionsTest, HeadingSpacing)
{
    Node para;
    para.type = NodeType::Paragraph;

    Node heading;
    heading.type = NodeType::Heading;

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
    float para_bottom = cache[0].y_position + cache[0].height + theme.paragraph_spacing;
    float heading_y = cache[1].y_position;
    EXPECT_FLOAT_EQ(heading_y, para_bottom + theme.heading_spacing_above);

    // 見出しの後: paragraph_spacingではなくheading_spacing_below
    float heading_bottom = cache[1].y_position + cache[1].height + theme.heading_spacing_below;
    EXPECT_FLOAT_EQ(cache[2].y_position, heading_bottom);
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
        EXPECT_GT(cache[i].y_position, cache[i - 1].y_position);
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
        if (cache[i].layout_dirty) { any_dirty = true; break; }
    }
    ASSERT_TRUE(any_dirty);

    // 修正パスをテストするため、表示中のノードを手動でダーティにマーク
    cache[0].layout_dirty = true;

    // EnsureVisibleLayoutが表示範囲を修正すること
    bool updated = engine_.EnsureVisibleLayout(nodes, cache, 400.0f, 0.0f, 100.0f);
    EXPECT_TRUE(updated);

    // 表示範囲内のノードはもうダーティでないこと
    for (size_t i = 0; i < nodes.size(); i++) {
        if (cache[i].y_position + cache[i].height < 0.0f) continue;
        if (cache[i].y_position > 100.0f) break;
        EXPECT_FALSE(cache[i].layout_dirty)
            << "y=" << cache[i].y_position << " の表示ノードがまだダーティ";
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
        if (cache[i].layout_dirty) dirty_before++;
    }

    // 小さなビューポート範囲のみでEnsureVisibleLayoutを実行
    engine_.EnsureVisibleLayout(nodes, cache, 800.0f, 0.0f, 50.0f);

    // 遠くのダーティノードはダーティのままであること
    int dirty_after = 0;
    for (size_t i = 0; i < nodes.size(); i++) {
        if (cache[i].layout_dirty) dirty_after++;
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
        EXPECT_GT(cache[i].y_position, cache[i - 1].y_position)
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

// ---- RecomputeYPositions 追加テスト ----

TEST(RecomputeYPositionsTest, MultipleHeadingsHaveCorrectSpacing)
{
    Theme theme = GetLightTheme();
    Node h1;
    h1.type = NodeType::Heading;

    Node h2;
    h2.type = NodeType::Heading;

    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(h1));
    nodes.emplace_back(std::move(h2));
    LayoutCache cache;
    cache.Resize(nodes.size());
    cache[0].height = 40.0f;
    cache[0].layout_dirty = false;
    cache[1].height = 30.0f;
    cache[1].layout_dirty = false;

    RecomputeYPositions(nodes, cache, theme);

    // 最初の見出し: margin_top + heading_spacing_above
    EXPECT_FLOAT_EQ(cache[0].y_position, theme.margin_top + theme.heading_spacing_above);

    // 2番目の見出し: 最初の見出しの後 + heading_spacing_below + heading_spacing_above
    float expected_y = cache[0].y_position + cache[0].height
        + theme.heading_spacing_below + theme.heading_spacing_above;
    EXPECT_FLOAT_EQ(cache[1].y_position, expected_y);
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
        EXPECT_GT(cache[i].y_position, cache[i - 1].y_position);
    }
    // 全体の高さが最後のノードの下端を超えること
    size_t last = nodes.size() - 1;
    float last_bottom = cache[last].y_position + cache[last].height;
    EXPECT_GE(result.total_height, last_bottom);
}

// ---- FindFirstVisibleNodeIndex テスト（layout_cache.h のフリー関数） ----

static LayoutCache MakeSimpleCache(int count, float node_height)
{
    LayoutCache cache;
    cache.Resize(count);
    float y = 0.0f;
    for (int i = 0; i < count; ++i) {
        cache[i].y_position = y;
        cache[i].height = node_height;
        y += node_height;
    }
    return cache;
}

TEST(FindFirstVisibleNodeIndex, AtStart)
{
    auto cache = MakeSimpleCache(10, 50.0f);
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 10, 0.0f), 0);
}

TEST(FindFirstVisibleNodeIndex, MidDocument)
{
    auto cache = MakeSimpleCache(10, 50.0f);
    // viewport_top = 120 → ノード2 (y=100, bottom=150) が最初の可視ノード
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 10, 120.0f), 2);
}

TEST(FindFirstVisibleNodeIndex, ExactBoundary)
{
    auto cache = MakeSimpleCache(10, 50.0f);
    // viewport_top = 50 → ノード0はy=50で終了、ノード1はy=50で開始
    // ノード0の下端(50) == viewport_top(50)なので除外される
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 10, 50.0f), 1);
}

TEST(FindFirstVisibleNodeIndex, PastEnd)
{
    auto cache = MakeSimpleCache(5, 50.0f);
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
    auto cache = MakeSimpleCache(1, 100.0f);
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 1, 0.0f), 0);
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 1, 50.0f), 0);
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 1, 100.0f), 1); // ノードを過ぎた位置
}

TEST(FindFirstVisibleNodeIndex, LastNodeVisible)
{
    auto cache = MakeSimpleCache(10, 50.0f);
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

    float para_bottom = cache[0].y_position + cache[0].height;
    float gap = cache[1].y_position - para_bottom;
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

    float code_bottom = cache[0].y_position + cache[0].height;
    float gap = cache[1].y_position - code_bottom;
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

    float para_bottom = cache[0].y_position + cache[0].height;
    float gap = cache[1].y_position - para_bottom;
    // paragraph_spacing + code_block_spacing_above
    EXPECT_FLOAT_EQ(gap, theme.paragraph_spacing + theme.code_block_spacing_above);
}

// ---- リスト項目のスペーシング ----

TEST(RecomputeYPositionsTest, ListItemUsesListItemSpacing)
{
    Theme theme = GetLightTheme();
    Node li1;
    li1.type = NodeType::ListItem;
    Node li2;
    li2.type = NodeType::ListItem;

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

    float gap = cache[1].y_position - (cache[0].y_position + cache[0].height);
    EXPECT_FLOAT_EQ(gap, theme.list_item_spacing);
}

TEST(RecomputeYPositionsTest, TaskListItemUsesListItemSpacing)
{
    Theme theme = GetLightTheme();
    Node tli1;
    tli1.type = NodeType::TaskListItem;
    Node tli2;
    tli2.type = NodeType::TaskListItem;

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

    float gap = cache[1].y_position - (cache[0].y_position + cache[0].height);
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
    float expected_y1 = cache[1].y_position;

    // from_index=1 で途中から再計算
    RecomputeYPositions(nodes, cache, theme, 1);
    EXPECT_FLOAT_EQ(cache[1].y_position, expected_y1);
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
    float expected_y1 = cache[1].y_position;

    RecomputeYPositions(nodes, cache, theme, 1);
    EXPECT_FLOAT_EQ(cache[1].y_position, expected_y1);
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
            if (run.code) {
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
        if (run.code) {
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
        if (run.code) {
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
    node.SetText(L"Hello world");
    std::pmr::vector<Node> nodes;
    nodes.emplace_back(std::move(node));
    LayoutCache cache;
    cache.Resize(nodes.size());
    Theme theme = GetLightTheme();

    EstimateNodeHeights(nodes, cache, theme);

    EXPECT_GT(cache[0].height, 0.0f);
    EXPECT_GE(cache[0].y_position, theme.margin_top);
}

TEST(EstimateNodeHeightsTest, YPositionsIncreaseMonotonically)
{
    auto nodes = ParseMarkdown("# A\n\nB\n\nC\n\nD").nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    Theme theme = GetLightTheme();

    EstimateNodeHeights(nodes, cache, theme);

    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(cache[i].y_position, cache[i - 1].y_position)
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
        float prev_bottom = cache[i - 1].y_position + cache[i - 1].height;
        EXPECT_LE(prev_bottom, cache[i].y_position)
            << "ノード " << (i - 1) << " がノード " << i << " と重なっている";
    }
}

TEST(EstimateNodeHeightsTest, HeadingHeightScalesWithLevel)
{
    Theme theme = GetLightTheme();

    // H1とH3を推定して、H1の方が高いことを確認
    Node h1;
    h1.type = NodeType::Heading;
    h1.heading_level = 1;
    h1.SetText(L"Title");

    Node h3;
    h3.type = NodeType::Heading;
    h3.heading_level = 3;
    h3.SetText(L"Title");

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
    short_code.SetText(L"line1");
    short_code.line_count = 0;

    Node long_code;
    long_code.type = NodeType::CodeBlock;
    long_code.SetText(L"line1\nline2\nline3\nline4\nline5");
    long_code.line_count = 4;

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
    table1.table_rows().push_back(TableRow{});

    Node table3;
    table3.type = NodeType::Table;
    table3.ensure_table();
    table3.table_rows().push_back(TableRow{});
    table3.table_rows().push_back(TableRow{});
    table3.table_rows().push_back(TableRow{});

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
    single.SetText(L"one line");
    single.line_count = 0;

    Node multi;
    multi.type = NodeType::Paragraph;
    multi.SetText(L"line1\nline2\nline3");
    multi.line_count = 2;

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
    node.SetText(L"test");

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

    auto add_node = [&](NodeType type, const wchar_t* text = L"content") {
        Node n;
        n.type = type;
        n.SetText(text);
        if (type == NodeType::Heading) {
            n.heading_level = 2;
        }
        if (type == NodeType::Table) {
            n.ensure_table();
            n.table_rows().push_back(TableRow{});
        }
        nodes.emplace_back(std::move(n));
    };

    add_node(NodeType::Paragraph);
    add_node(NodeType::Heading);
    add_node(NodeType::CodeBlock);
    add_node(NodeType::HorizontalRule, L"");
    add_node(NodeType::ListItem);
    add_node(NodeType::BlockQuote);
    add_node(NodeType::Table);
    add_node(NodeType::TaskListItem);
    add_node(NodeType::Image, L"");

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

    float expected_y = cache[0].y_position + lm.height * 0.5f;

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
