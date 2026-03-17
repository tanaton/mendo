#include <gtest/gtest.h>
#include "layout.h"
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

    static void SetUpTestSuite() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    }

    static void TearDownTestSuite() {
        CoUninitialize();
    }

    void SetUp() override {
        HRESULT hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
        ASSERT_TRUE(SUCCEEDED(hr)) << "Failed to create DirectWrite factory";

        theme_ = GetLightTheme();
        measurer_.SetFactory(dwrite_.Get());
        ASSERT_TRUE(engine_.Init(&measurer_, theme_));
    }
};

TEST_F(LayoutTest, EmptyNodesProduceZeroHeight) {
    std::vector<Node> nodes;
    LayoutCache cache;
    engine_.ComputeLayout(nodes, cache, 800.0f);
    // Only margin_top contributes
    EXPECT_FLOAT_EQ(engine_.GetTotalHeight(), theme_.margin_top * 2);
}

TEST_F(LayoutTest, SingleParagraphHasPositiveHeight) {
    auto nodes = ParseMarkdown("Hello world");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    EXPECT_GT(engine_.GetTotalHeight(), 0.0f);
    EXPECT_GT(cache[0].height, 0.0f);
}

TEST_F(LayoutTest, HeadingIsTallerThanParagraph) {
    auto heading_nodes = ParseMarkdown("# Big Title");
    LayoutCache heading_cache;
    heading_cache.Resize(heading_nodes.size());

    auto para_nodes = ParseMarkdown("Small text");
    LayoutCache para_cache;
    para_cache.Resize(para_nodes.size());

    engine_.ComputeLayout(heading_nodes, heading_cache, 800.0f);
    float heading_height = heading_cache[0].height;

    engine_.ComputeLayout(para_nodes, para_cache, 800.0f);
    float para_height = para_cache[0].height;

    EXPECT_GT(heading_height, para_height);
}

TEST_F(LayoutTest, YPositionsIncreaseMonotonically) {
    auto nodes = ParseMarkdown("# A\n\nB\n\nC\n\nD");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(cache[i].y_position, cache[i - 1].y_position)
            << "Node " << i << " y should be > node " << (i - 1);
    }
}

TEST_F(LayoutTest, NodesDoNotOverlap) {
    auto nodes = ParseMarkdown("# Heading\n\nParagraph\n\n---\n\n- List");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    for (size_t i = 1; i < nodes.size(); i++) {
        float prev_bottom = cache[i - 1].y_position + cache[i - 1].height;
        EXPECT_LE(prev_bottom, cache[i].y_position)
            << "Node " << (i - 1) << " overlaps with node " << i;
    }
}

TEST_F(LayoutTest, NarrowViewportWrapsText) {
    auto nodes_wide = ParseMarkdown("This is a somewhat long paragraph that should wrap.");
    LayoutCache cache_wide;
    cache_wide.Resize(nodes_wide.size());

    auto nodes_narrow = ParseMarkdown("This is a somewhat long paragraph that should wrap.");
    LayoutCache cache_narrow;
    cache_narrow.Resize(nodes_narrow.size());

    engine_.ComputeLayout(nodes_wide, cache_wide, 800.0f);
    float wide_height = cache_wide[0].height;

    engine_.ComputeLayout(nodes_narrow, cache_narrow, 200.0f);
    float narrow_height = cache_narrow[0].height;

    // Narrower viewport should make the text taller (more wrapping)
    EXPECT_GE(narrow_height, wide_height);
}

TEST_F(LayoutTest, LayoutDirtyFlagCleared) {
    auto nodes = ParseMarkdown("Test");
    LayoutCache cache;
    cache.Resize(nodes.size());
    EXPECT_TRUE(cache[0].layout_dirty);
    engine_.ComputeLayout(nodes, cache, 800.0f);
    EXPECT_FALSE(cache[0].layout_dirty);
}

TEST_F(LayoutTest, TextLayoutCreated) {
    auto nodes = ParseMarkdown("Test paragraph");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    EXPECT_NE(cache[0].text_layout.Get(), nullptr);
}

TEST_F(LayoutTest, HorizontalRuleHasNoTextLayout) {
    auto nodes = ParseMarkdown("---");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    EXPECT_EQ(cache[0].text_layout.Get(), nullptr);
    EXPECT_GT(cache[0].height, 0.0f);
}

TEST_F(LayoutTest, CodeBlockTextLayout) {
    auto nodes = ParseMarkdown("```\ncode\n```");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    EXPECT_NE(cache[0].text_layout.Get(), nullptr);
}

TEST_F(LayoutTest, TableLayout) {
    auto nodes = ParseMarkdown(
        "| A | B |\n"
        "|---|---|\n"
        "| 1 | 2 |"
    );
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Table);
    EXPECT_GT(cache[0].height, 0.0f);
    EXPECT_FALSE(cache[0].col_widths.empty());
}

TEST_F(LayoutTest, TableCellLayoutsCreated) {
    auto nodes = ParseMarkdown(
        "| A | B |\n"
        "|---|---|\n"
        "| 1 | 2 |"
    );
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    const auto& cell_layouts = cache[0].cell_layouts;
    for (size_t r = 0; r < cell_layouts.size(); r++) {
        for (size_t c = 0; c < cell_layouts[r].size(); c++) {
            if (!nodes[0].table_rows[r].cells[c].text.empty()) {
                EXPECT_NE(cell_layouts[r][c].Get(), nullptr);
            }
        }
    }
}

TEST_F(LayoutTest, TableCellLinkHasUnderline) {
    auto nodes = ParseMarkdown(
        "| Text | Link |\n"
        "|------|------|\n"
        "| hello | [click](https://example.com) |"
    );
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_GE(nodes[0].table_rows.size(), 2u);

    // The data row's second cell should have a link run with underline applied
    const auto& cell = nodes[0].table_rows[1].cells[1];
    auto& cell_layout = cache[0].cell_layouts[1][1];
    ASSERT_NE(cell_layout.Get(), nullptr);

    // Verify link run exists in cell
    bool has_link_run = false;
    for (const auto& run : cell.runs) {
        if (run.link_url.has_value()) {
            has_link_run = true;

            // Check that underline was applied to the text layout
            BOOL underline = FALSE;
            cell_layout->GetUnderline(run.start, &underline);
            EXPECT_TRUE(underline) << "Link run in table cell should have underline";
        }
    }
    EXPECT_TRUE(has_link_run);
}

TEST_F(LayoutTest, MultipleHeadingLevelsDecreasingSize) {
    auto nodes = ParseMarkdown("# H1\n\n## H2\n\n### H3");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    ASSERT_EQ(nodes.size(), 3u);
    // H1 should be taller than H2, H2 >= H3
    EXPECT_GT(cache[0].height, cache[1].height);
    EXPECT_GE(cache[1].height, cache[2].height);
}

TEST_F(LayoutTest, TotalHeightWithManyNodes) {
    std::string md;
    for (int i = 0; i < 100; i++) {
        md += "Paragraph " + std::to_string(i) + "\n\n";
    }
    auto nodes = ParseMarkdown(md);
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    float total = engine_.GetTotalHeight();
    EXPECT_GT(total, 1000.0f); // 100 paragraphs should be quite tall

    // Last node's bottom should be within total height
    size_t last = nodes.size() - 1;
    EXPECT_LE(cache[last].y_position + cache[last].height, total);
}

// ---- ProcessDirtyBatch tests ----

TEST_F(LayoutTest, ProcessDirtyBatchCleansNodes) {
    auto nodes = ParseMarkdown("# A\n\nB\n\nC\n\nD\n\nE");
    LayoutCache cache;
    cache.Resize(nodes.size());
    // Do a partial layout first
    engine_.ComputeLayout(nodes, cache, 800.0f, 0.0f, 50.0f);

    // If there are dirty nodes, process them
    if (engine_.HasDirtyNodes()) {
        bool more = engine_.ProcessDirtyBatch(nodes, cache, 800.0f, 100);
        // After processing enough, should have no dirty nodes
        EXPECT_FALSE(more);
    }

    // All nodes should have valid positions
    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(cache[i].y_position, cache[i - 1].y_position);
    }
}

TEST_F(LayoutTest, ProcessDirtyBatchSmallBatch) {
    // Create many paragraphs
    std::string md;
    for (int i = 0; i < 50; i++) {
        md += "Paragraph " + std::to_string(i) + "\n\n";
    }
    auto nodes = ParseMarkdown(md);
    LayoutCache cache;
    cache.Resize(nodes.size());

    // Do partial layout with very small viewport
    engine_.ComputeLayout(nodes, cache, 800.0f, 0.0f, 10.0f);

    if (engine_.HasDirtyNodes()) {
        // Process only 5 nodes at a time
        bool more = engine_.ProcessDirtyBatch(nodes, cache, 800.0f, 5);
        // With 50 nodes and batch=5, should still have more dirty
        EXPECT_TRUE(more);
    }
}

// ---- Width change detection ----

TEST_F(LayoutTest, WidthChangeRecomputesLayouts) {
    auto nodes = ParseMarkdown("This is a paragraph with some text that might wrap differently.");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    float height_wide = cache[0].height;

    engine_.ComputeLayout(nodes, cache, 200.0f);
    float height_narrow = cache[0].height;

    // Narrow width should produce taller text (more wrapping)
    EXPECT_GE(height_narrow, height_wide);
}

// ---- Empty table ----

TEST_F(LayoutTest, EmptyTableMinimalHeight) {
    Node node;
    node.type = NodeType::Table;
    node.table_rows.clear();
    std::vector<Node> nodes = {node};
    LayoutCache cache;
    cache.Resize(nodes.size());

    engine_.ComputeLayout(nodes, cache, 800.0f);
    // Empty table should not crash and have some height
    EXPECT_GE(cache[0].height, 0.0f);
}

// ---- Indented nodes ----

TEST_F(LayoutTest, IndentedNodesHaveNarrowerWidth) {
    auto nodes_plain = ParseMarkdown("This is a somewhat long paragraph that wraps.");
    LayoutCache cache_plain;
    cache_plain.Resize(nodes_plain.size());

    auto nodes_list = ParseMarkdown("- This is a somewhat long paragraph that wraps.");
    LayoutCache cache_list;
    cache_list.Resize(nodes_list.size());

    engine_.ComputeLayout(nodes_plain, cache_plain, 400.0f);
    float plain_height = cache_plain[0].height;

    engine_.ComputeLayout(nodes_list, cache_list, 400.0f);
    float list_height = cache_list[0].height;

    // List items are indented, so same text should be taller (narrower available width)
    EXPECT_GE(list_height, plain_height);
}

// ---- Block quote layout ----

TEST_F(LayoutTest, BlockQuoteLayout) {
    auto nodes = ParseMarkdown("> Quoted text here");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_GT(cache[0].height, 0.0f);
    EXPECT_GT(nodes[0].indent_level, 0);
}

// ---- Code block no wrap ----

TEST_F(LayoutTest, CodeBlockDoesNotWrap) {
    std::string long_line = "```\n";
    for (int i = 0; i < 50; i++) long_line += "long_word ";
    long_line += "\n```";

    auto nodes = ParseMarkdown(long_line);
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 200.0f);  // Very narrow

    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    // Code blocks don't wrap, so height should be for a single line
    // (approximately the code font height)
    EXPECT_LT(cache[0].height, 100.0f);
}

// ---- Heading spacing ----

TEST_F(LayoutTest, HeadingHasSpacingAboveAndBelow) {
    auto nodes = ParseMarkdown("Paragraph\n\n# Heading\n\nAnother paragraph");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    ASSERT_EQ(nodes.size(), 3u);

    // Heading should have spacing above (gap between paragraph bottom and heading y)
    float para_bottom = cache[0].y_position + cache[0].height;
    float heading_y = cache[1].y_position;
    float gap_above = heading_y - para_bottom;
    EXPECT_GT(gap_above, theme_.paragraph_spacing);

    // Heading should have spacing below
    float heading_bottom = cache[1].y_position + cache[1].height;
    float next_y = cache[2].y_position;
    float gap_below = next_y - heading_bottom;
    EXPECT_GT(gap_below, 0.0f);
}

// ========================================================
// Tests for extracted free functions
// ========================================================

// ---- ComputeColumnWidths ----

TEST(ComputeColumnWidthsTest, ProportionalDistributionWhenTooWide) {
    // natural widths total 300, available only 150 -> proportional
    std::vector<float> natural = {100.0f, 100.0f, 100.0f};
    auto widths = ComputeColumnWidths(natural, 150.0f, 3);
    ASSERT_EQ(widths.size(), 3u);
    // All columns should get equal share since natural widths are equal
    EXPECT_NEAR(widths[0], widths[1], 0.01f);
    EXPECT_NEAR(widths[1], widths[2], 0.01f);
    // Total should approximate available width
    float total = widths[0] + widths[1] + widths[2];
    EXPECT_NEAR(total, 150.0f, 1.0f);
}

TEST(ComputeColumnWidthsTest, EvenDistributionWhenFits) {
    // natural widths total 30, available 300 -> even distribution
    std::vector<float> natural = {10.0f, 10.0f, 10.0f};
    auto widths = ComputeColumnWidths(natural, 300.0f, 3);
    ASSERT_EQ(widths.size(), 3u);
    // Even distribution: each should be at least 100
    float even = 300.0f / 3.0f;
    for (auto w : widths) {
        EXPECT_GE(w, even - 0.01f);
    }
}

TEST(ComputeColumnWidthsTest, MinimumWidthEnforced) {
    // Very small available space
    std::vector<float> natural = {200.0f, 200.0f};
    auto widths = ComputeColumnWidths(natural, 40.0f, 2);
    ASSERT_EQ(widths.size(), 2u);
    // Minimum width is 30
    for (auto w : widths) {
        EXPECT_GE(w, 30.0f);
    }
}

TEST(ComputeColumnWidthsTest, UnequalNaturalWidths) {
    // Column A is much wider than column B
    std::vector<float> natural = {300.0f, 100.0f};
    auto widths = ComputeColumnWidths(natural, 200.0f, 2);
    ASSERT_EQ(widths.size(), 2u);
    // Column A should get a larger share than B
    EXPECT_GT(widths[0], widths[1]);
}

TEST(ComputeColumnWidthsTest, SingleColumn) {
    std::vector<float> natural = {50.0f};
    auto widths = ComputeColumnWidths(natural, 200.0f, 1);
    ASSERT_EQ(widths.size(), 1u);
    EXPECT_GE(widths[0], 50.0f);
}

TEST(ComputeColumnWidthsTest, ZeroNaturalWidths) {
    std::vector<float> natural = {0.0f, 0.0f};
    auto widths = ComputeColumnWidths(natural, 200.0f, 2);
    ASSERT_EQ(widths.size(), 2u);
    // Should still produce valid widths
    for (auto w : widths) {
        EXPECT_GT(w, 0.0f);
    }
}

// ---- BuildLinearizedTableText ----

TEST(BuildLinearizedTableTextTest, EmptyRows) {
    std::vector<TableRow> rows;
    auto text = BuildLinearizedTableText(rows);
    EXPECT_TRUE(text.empty());
}

TEST(BuildLinearizedTableTextTest, SingleCell) {
    TableRow row;
    row.cells.push_back(TableCell{L"hello"});
    auto text = BuildLinearizedTableText({row});
    EXPECT_EQ(text, L"hello");
}

TEST(BuildLinearizedTableTextTest, TabSeparatedCells) {
    TableRow row;
    row.cells.push_back(TableCell{L"A"});
    row.cells.push_back(TableCell{L"B"});
    row.cells.push_back(TableCell{L"C"});
    auto text = BuildLinearizedTableText({row});
    EXPECT_EQ(text, L"A\tB\tC");
}

TEST(BuildLinearizedTableTextTest, NewlineSeparatedRows) {
    TableRow row1;
    row1.cells.push_back(TableCell{L"A"});
    row1.cells.push_back(TableCell{L"B"});
    TableRow row2;
    row2.cells.push_back(TableCell{L"1"});
    row2.cells.push_back(TableCell{L"2"});
    auto text = BuildLinearizedTableText({row1, row2});
    EXPECT_EQ(text, L"A\tB\n1\t2");
}

TEST(BuildLinearizedTableTextTest, NoTrailingNewline) {
    TableRow row;
    row.cells.push_back(TableCell{L"x"});
    auto text = BuildLinearizedTableText({row});
    EXPECT_FALSE(text.empty());
    EXPECT_NE(text.back(), L'\n');
}

TEST(BuildLinearizedTableTextTest, EmptyCells) {
    TableRow row;
    row.cells.push_back(TableCell{L""});
    row.cells.push_back(TableCell{L"B"});
    row.cells.push_back(TableCell{L""});
    auto text = BuildLinearizedTableText({row});
    EXPECT_EQ(text, L"\tB\t");
}

// ---- RecomputeYPositions ----

TEST(RecomputeYPositionsTest, EmptyNodes) {
    std::vector<Node> nodes;
    LayoutCache cache;
    Theme theme = GetLightTheme();
    auto result = RecomputeYPositions(nodes, cache, theme);
    EXPECT_FLOAT_EQ(result.total_height, theme.margin_top * 2);
    EXPECT_FALSE(result.has_dirty_nodes);
}

// ---- ComputeTotalContentHeight ----

TEST(ComputeTotalContentHeightTest, EmptyNodesReturnsZero) {
    LayoutCache cache;
    // node_count == 0 must not underflow size_t; should return 0.
    EXPECT_FLOAT_EQ(ComputeTotalContentHeight(cache, 0, 10.0f), 0.0f);
}

TEST(ComputeTotalContentHeightTest, SingleNode) {
    LayoutCache cache;
    cache.Resize(1);
    cache[0].y_position = 15.0f;
    cache[0].height = 50.0f;
    EXPECT_FLOAT_EQ(ComputeTotalContentHeight(cache, 1, 15.0f), 80.0f);
}

TEST(ComputeTotalContentHeightTest, MultipleNodes) {
    LayoutCache cache;
    cache.Resize(3);
    cache[0].y_position = 10.0f;  cache[0].height = 20.0f;
    cache[1].y_position = 40.0f;  cache[1].height = 30.0f;
    cache[2].y_position = 80.0f;  cache[2].height = 25.0f;
    // Only the last node matters: 80 + 25 + 10 = 115
    EXPECT_FLOAT_EQ(ComputeTotalContentHeight(cache, 3, 10.0f), 115.0f);
}

TEST(RecomputeYPositionsTest, SingleParagraph) {
    Node node;
    node.type = NodeType::Paragraph;
    std::vector<Node> nodes = {node};
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

TEST(RecomputeYPositionsTest, HeadingSpacing) {
    Node para;
    para.type = NodeType::Paragraph;

    Node heading;
    heading.type = NodeType::Heading;

    Node para2;
    para2.type = NodeType::Paragraph;

    std::vector<Node> nodes = {para, heading, para2};
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

    // Heading should have extra spacing above
    float para_bottom = cache[0].y_position + cache[0].height + theme.paragraph_spacing;
    float heading_y = cache[1].y_position;
    EXPECT_FLOAT_EQ(heading_y, para_bottom + theme.heading_spacing_above);

    // After heading: heading_spacing_below, not paragraph_spacing
    float heading_bottom = cache[1].y_position + cache[1].height + theme.heading_spacing_below;
    EXPECT_FLOAT_EQ(cache[2].y_position, heading_bottom);
}

TEST(RecomputeYPositionsTest, DetectsDirtyNodes) {
    Node clean;
    clean.type = NodeType::Paragraph;

    Node dirty;
    dirty.type = NodeType::Paragraph;

    std::vector<Node> nodes = {clean, dirty};
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

TEST(RecomputeYPositionsTest, MonotonicallyIncreasingY) {
    std::vector<Node> nodes;
    for (int i = 0; i < 10; i++) {
        Node node;
        node.type = NodeType::Paragraph;
        nodes.push_back(node);
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

// ---- EnsureVisibleLayout tests ----

TEST_F(LayoutTest, EnsureVisibleLayoutFixesDirtyVisibleNodes) {
    // Create several paragraphs and do a full layout at one width
    std::string md;
    for (int i = 0; i < 20; i++) {
        md += "Paragraph " + std::to_string(i) + "\n\n";
    }
    auto nodes = ParseMarkdown(md);
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    // Now do a partial layout at a different width — this marks off-screen nodes dirty
    engine_.ComputeLayout(nodes, cache, 400.0f, 0.0f, 100.0f);

    // Some nodes beyond viewport should still be dirty
    bool any_dirty = false;
    for (size_t i = 0; i < nodes.size(); i++) {
        if (cache[i].layout_dirty) { any_dirty = true; break; }
    }
    ASSERT_TRUE(any_dirty);

    // Mark a visible node dirty manually to test the fix path
    cache[0].layout_dirty = true;

    // EnsureVisibleLayout should fix the visible range
    bool updated = engine_.EnsureVisibleLayout(nodes, cache, 400.0f, 0.0f, 100.0f);
    EXPECT_TRUE(updated);

    // Nodes in visible range should no longer be dirty
    for (size_t i = 0; i < nodes.size(); i++) {
        if (cache[i].y_position + cache[i].height < 0.0f) continue;
        if (cache[i].y_position > 100.0f) break;
        EXPECT_FALSE(cache[i].layout_dirty)
            << "Visible node at y=" << cache[i].y_position << " is still dirty";
    }
}

TEST_F(LayoutTest, EnsureVisibleLayoutReturnsFalseWhenClean) {
    auto nodes = ParseMarkdown("Hello world");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    // All nodes are clean, so EnsureVisibleLayout should return false
    bool updated = engine_.EnsureVisibleLayout(nodes, cache, 800.0f, 0.0f, 1000.0f);
    EXPECT_FALSE(updated);
}

TEST_F(LayoutTest, EnsureVisibleLayoutSkipsOffscreenDirtyNodes) {
    std::string md;
    for (int i = 0; i < 30; i++) {
        md += "Paragraph " + std::to_string(i) + "\n\n";
    }
    auto nodes = ParseMarkdown(md);
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f, 0.0f, 50.0f);

    // Count dirty nodes before
    int dirty_before = 0;
    for (size_t i = 0; i < nodes.size(); i++) {
        if (cache[i].layout_dirty) dirty_before++;
    }

    // EnsureVisibleLayout only for a small viewport slice
    engine_.EnsureVisibleLayout(nodes, cache, 800.0f, 0.0f, 50.0f);

    // Distant dirty nodes should remain dirty
    int dirty_after = 0;
    for (size_t i = 0; i < nodes.size(); i++) {
        if (cache[i].layout_dirty) dirty_after++;
    }
    // Some nodes should still be dirty (the off-screen ones)
    EXPECT_GT(dirty_after, 0);
    EXPECT_LE(dirty_after, dirty_before);
}

TEST_F(LayoutTest, EnsureVisibleLayoutRecomputesYPositions) {
    std::string md;
    for (int i = 0; i < 10; i++) {
        md += "Paragraph " + std::to_string(i) + "\n\n";
    }
    auto nodes = ParseMarkdown(md);
    LayoutCache cache;
    cache.Resize(nodes.size());
    // Full layout at wide width
    engine_.ComputeLayout(nodes, cache, 800.0f);

    // Now do a partial layout at narrow width (marks off-screen dirty)
    engine_.ComputeLayout(nodes, cache, 300.0f, 0.0f, 50.0f);

    // EnsureVisibleLayout should update Y positions consistently
    engine_.EnsureVisibleLayout(nodes, cache, 300.0f, 0.0f, 50.0f);

    // Y positions should still be monotonically increasing
    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(cache[i].y_position, cache[i - 1].y_position)
            << "Node " << i << " y should be > node " << (i - 1);
    }
}

TEST_F(LayoutTest, EnsureVisibleLayoutUpdatesTotalHeight) {
    std::string md;
    for (int i = 0; i < 10; i++) {
        md += "Paragraph " + std::to_string(i) + "\n\n";
    }
    auto nodes = ParseMarkdown(md);
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f, 0.0f, 50.0f);

    float height_before = engine_.GetTotalHeight();
    engine_.EnsureVisibleLayout(nodes, cache, 800.0f, 0.0f, 50.0f);
    float height_after = engine_.GetTotalHeight();

    // Total height may change when visible nodes get re-laid out
    // but should remain positive
    EXPECT_GT(height_after, 0.0f);
    (void)height_before;
}

// ---- RecomputeYPositions additional tests ----

TEST(RecomputeYPositionsTest, MultipleHeadingsHaveCorrectSpacing) {
    Theme theme = GetLightTheme();
    Node h1;
    h1.type = NodeType::Heading;

    Node h2;
    h2.type = NodeType::Heading;

    std::vector<Node> nodes = {h1, h2};
    LayoutCache cache;
    cache.Resize(nodes.size());
    cache[0].height = 40.0f;
    cache[0].layout_dirty = false;
    cache[1].height = 30.0f;
    cache[1].layout_dirty = false;

    RecomputeYPositions(nodes, cache, theme);

    // First heading: margin_top + heading_spacing_above
    EXPECT_FLOAT_EQ(cache[0].y_position, theme.margin_top + theme.heading_spacing_above);

    // Second heading: after first heading + heading_spacing_below + heading_spacing_above
    float expected_y = cache[0].y_position + cache[0].height
                     + theme.heading_spacing_below + theme.heading_spacing_above;
    EXPECT_FLOAT_EQ(cache[1].y_position, expected_y);
}

TEST(RecomputeYPositionsTest, AllNodeTypesProduceValidPositions) {
    Theme theme = GetLightTheme();
    std::vector<Node> nodes;

    auto add_node = [&](NodeType type) {
        Node n;
        n.type = type;
        nodes.push_back(n);
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
    float heights[] = {20.0f, 30.0f, 50.0f, 5.0f, 18.0f, 25.0f, 60.0f};
    for (size_t i = 0; i < nodes.size(); i++) {
        cache[i].height = heights[i];
        cache[i].layout_dirty = false;
    }

    auto result = RecomputeYPositions(nodes, cache, theme);

    // All positions should be monotonically increasing
    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(cache[i].y_position, cache[i - 1].y_position);
    }
    // Total height should exceed last node's bottom
    size_t last = nodes.size() - 1;
    float last_bottom = cache[last].y_position + cache[last].height;
    EXPECT_GE(result.total_height, last_bottom);
}
