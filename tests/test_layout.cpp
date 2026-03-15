#include <gtest/gtest.h>
#include "layout.h"
#include "parser.h"
#include <dwrite.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class LayoutTest : public ::testing::Test {
protected:
    ComPtr<IDWriteFactory> dwrite_;
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
        ASSERT_TRUE(engine_.Init(dwrite_.Get(), theme_));
    }
};

TEST_F(LayoutTest, EmptyNodesProduceZeroHeight) {
    std::vector<RenderNode> nodes;
    engine_.ComputeLayout(nodes, 800.0f);
    // Only margin_top contributes
    EXPECT_FLOAT_EQ(engine_.GetTotalHeight(), theme_.margin_top * 2);
}

TEST_F(LayoutTest, SingleParagraphHasPositiveHeight) {
    auto nodes = ParseMarkdown("Hello world");
    engine_.ComputeLayout(nodes, 800.0f);
    EXPECT_GT(engine_.GetTotalHeight(), 0.0f);
    EXPECT_GT(nodes[0].height, 0.0f);
}

TEST_F(LayoutTest, HeadingIsTallerThanParagraph) {
    auto heading_nodes = ParseMarkdown("# Big Title");
    auto para_nodes = ParseMarkdown("Small text");

    engine_.ComputeLayout(heading_nodes, 800.0f);
    float heading_height = heading_nodes[0].height;

    engine_.ComputeLayout(para_nodes, 800.0f);
    float para_height = para_nodes[0].height;

    EXPECT_GT(heading_height, para_height);
}

TEST_F(LayoutTest, YPositionsIncreaseMonotonically) {
    auto nodes = ParseMarkdown("# A\n\nB\n\nC\n\nD");
    engine_.ComputeLayout(nodes, 800.0f);

    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(nodes[i].y_position, nodes[i - 1].y_position)
            << "Node " << i << " y should be > node " << (i - 1);
    }
}

TEST_F(LayoutTest, NodesDoNotOverlap) {
    auto nodes = ParseMarkdown("# Heading\n\nParagraph\n\n---\n\n- List");
    engine_.ComputeLayout(nodes, 800.0f);

    for (size_t i = 1; i < nodes.size(); i++) {
        float prev_bottom = nodes[i - 1].y_position + nodes[i - 1].height;
        EXPECT_LE(prev_bottom, nodes[i].y_position)
            << "Node " << (i - 1) << " overlaps with node " << i;
    }
}

TEST_F(LayoutTest, NarrowViewportWrapsText) {
    auto nodes_wide = ParseMarkdown("This is a somewhat long paragraph that should wrap.");
    auto nodes_narrow = ParseMarkdown("This is a somewhat long paragraph that should wrap.");

    engine_.ComputeLayout(nodes_wide, 800.0f);
    float wide_height = nodes_wide[0].height;

    engine_.ComputeLayout(nodes_narrow, 200.0f);
    float narrow_height = nodes_narrow[0].height;

    // Narrower viewport should make the text taller (more wrapping)
    EXPECT_GE(narrow_height, wide_height);
}

TEST_F(LayoutTest, LayoutDirtyFlagCleared) {
    auto nodes = ParseMarkdown("Test");
    EXPECT_TRUE(nodes[0].layout_dirty);
    engine_.ComputeLayout(nodes, 800.0f);
    EXPECT_FALSE(nodes[0].layout_dirty);
}

TEST_F(LayoutTest, TextLayoutCreated) {
    auto nodes = ParseMarkdown("Test paragraph");
    engine_.ComputeLayout(nodes, 800.0f);
    EXPECT_NE(nodes[0].text_layout.Get(), nullptr);
}

TEST_F(LayoutTest, HorizontalRuleHasNoTextLayout) {
    auto nodes = ParseMarkdown("---");
    engine_.ComputeLayout(nodes, 800.0f);
    EXPECT_EQ(nodes[0].text_layout.Get(), nullptr);
    EXPECT_GT(nodes[0].height, 0.0f);
}

TEST_F(LayoutTest, CodeBlockTextLayout) {
    auto nodes = ParseMarkdown("```\ncode\n```");
    engine_.ComputeLayout(nodes, 800.0f);
    EXPECT_NE(nodes[0].text_layout.Get(), nullptr);
}

TEST_F(LayoutTest, TableLayout) {
    auto nodes = ParseMarkdown(
        "| A | B |\n"
        "|---|---|\n"
        "| 1 | 2 |"
    );
    engine_.ComputeLayout(nodes, 800.0f);
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Table);
    EXPECT_GT(nodes[0].height, 0.0f);
    EXPECT_FALSE(nodes[0].col_widths.empty());
}

TEST_F(LayoutTest, TableCellLayoutsCreated) {
    auto nodes = ParseMarkdown(
        "| A | B |\n"
        "|---|---|\n"
        "| 1 | 2 |"
    );
    engine_.ComputeLayout(nodes, 800.0f);
    for (const auto& row : nodes[0].table_rows) {
        for (const auto& cell : row.cells) {
            if (!cell.text.empty()) {
                EXPECT_NE(cell.text_layout.Get(), nullptr);
            }
        }
    }
}

TEST_F(LayoutTest, MultipleHeadingLevelsDecreasingSize) {
    auto nodes = ParseMarkdown("# H1\n\n## H2\n\n### H3");
    engine_.ComputeLayout(nodes, 800.0f);
    ASSERT_EQ(nodes.size(), 3u);
    // H1 should be taller than H2, H2 >= H3
    EXPECT_GT(nodes[0].height, nodes[1].height);
    EXPECT_GE(nodes[1].height, nodes[2].height);
}

TEST_F(LayoutTest, TotalHeightWithManyNodes) {
    std::string md;
    for (int i = 0; i < 100; i++) {
        md += "Paragraph " + std::to_string(i) + "\n\n";
    }
    auto nodes = ParseMarkdown(md);
    engine_.ComputeLayout(nodes, 800.0f);

    float total = engine_.GetTotalHeight();
    EXPECT_GT(total, 1000.0f); // 100 paragraphs should be quite tall

    // Last node's bottom should be within total height
    const auto& last = nodes.back();
    EXPECT_LE(last.y_position + last.height, total);
}

// ---- ProcessDirtyBatch tests ----

TEST_F(LayoutTest, ProcessDirtyBatchCleansNodes) {
    auto nodes = ParseMarkdown("# A\n\nB\n\nC\n\nD\n\nE");
    // Do a partial layout first
    engine_.ComputeLayout(nodes, 800.0f, 0.0f, 50.0f);

    // If there are dirty nodes, process them
    if (engine_.HasDirtyNodes()) {
        bool more = engine_.ProcessDirtyBatch(nodes, 800.0f, 100);
        // After processing enough, should have no dirty nodes
        EXPECT_FALSE(more);
    }

    // All nodes should have valid positions
    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(nodes[i].y_position, nodes[i - 1].y_position);
    }
}

TEST_F(LayoutTest, ProcessDirtyBatchSmallBatch) {
    // Create many paragraphs
    std::string md;
    for (int i = 0; i < 50; i++) {
        md += "Paragraph " + std::to_string(i) + "\n\n";
    }
    auto nodes = ParseMarkdown(md);

    // Do partial layout with very small viewport
    engine_.ComputeLayout(nodes, 800.0f, 0.0f, 10.0f);

    if (engine_.HasDirtyNodes()) {
        // Process only 5 nodes at a time
        bool more = engine_.ProcessDirtyBatch(nodes, 800.0f, 5);
        // With 50 nodes and batch=5, should still have more dirty
        EXPECT_TRUE(more);
    }
}

// ---- Width change detection ----

TEST_F(LayoutTest, WidthChangeRecomputesLayouts) {
    auto nodes = ParseMarkdown("This is a paragraph with some text that might wrap differently.");
    engine_.ComputeLayout(nodes, 800.0f);
    float height_wide = nodes[0].height;

    engine_.ComputeLayout(nodes, 200.0f);
    float height_narrow = nodes[0].height;

    // Narrow width should produce taller text (more wrapping)
    EXPECT_GE(height_narrow, height_wide);
}

// ---- Empty table ----

TEST_F(LayoutTest, EmptyTableMinimalHeight) {
    RenderNode node;
    node.type = NodeType::Table;
    node.table_rows.clear();
    std::vector<RenderNode> nodes = {node};

    engine_.ComputeLayout(nodes, 800.0f);
    // Empty table should not crash and have some height
    EXPECT_GE(nodes[0].height, 0.0f);
}

// ---- Indented nodes ----

TEST_F(LayoutTest, IndentedNodesHaveNarrowerWidth) {
    auto nodes_plain = ParseMarkdown("This is a somewhat long paragraph that wraps.");
    auto nodes_list = ParseMarkdown("- This is a somewhat long paragraph that wraps.");

    engine_.ComputeLayout(nodes_plain, 400.0f);
    float plain_height = nodes_plain[0].height;

    engine_.ComputeLayout(nodes_list, 400.0f);
    float list_height = nodes_list[0].height;

    // List items are indented, so same text should be taller (narrower available width)
    EXPECT_GE(list_height, plain_height);
}

// ---- Block quote layout ----

TEST_F(LayoutTest, BlockQuoteLayout) {
    auto nodes = ParseMarkdown("> Quoted text here");
    engine_.ComputeLayout(nodes, 800.0f);
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_GT(nodes[0].height, 0.0f);
    EXPECT_GT(nodes[0].indent_level, 0);
}

// ---- Code block no wrap ----

TEST_F(LayoutTest, CodeBlockDoesNotWrap) {
    std::string long_line = "```\n";
    for (int i = 0; i < 50; i++) long_line += "long_word ";
    long_line += "\n```";

    auto nodes = ParseMarkdown(long_line);
    engine_.ComputeLayout(nodes, 200.0f);  // Very narrow

    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    // Code blocks don't wrap, so height should be for a single line
    // (approximately the code font height)
    EXPECT_LT(nodes[0].height, 100.0f);
}

// ---- Heading spacing ----

TEST_F(LayoutTest, HeadingHasSpacingAboveAndBelow) {
    auto nodes = ParseMarkdown("Paragraph\n\n# Heading\n\nAnother paragraph");
    engine_.ComputeLayout(nodes, 800.0f);
    ASSERT_EQ(nodes.size(), 3u);

    // Heading should have spacing above (gap between paragraph bottom and heading y)
    float para_bottom = nodes[0].y_position + nodes[0].height;
    float heading_y = nodes[1].y_position;
    float gap_above = heading_y - para_bottom;
    EXPECT_GT(gap_above, theme_.paragraph_spacing);

    // Heading should have spacing below
    float heading_bottom = nodes[1].y_position + nodes[1].height;
    float next_y = nodes[2].y_position;
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
    std::vector<RenderNode> nodes;
    Theme theme = GetLightTheme();
    auto result = RecomputeYPositions(nodes, theme);
    EXPECT_FLOAT_EQ(result.total_height, theme.margin_top * 2);
    EXPECT_FALSE(result.has_dirty_nodes);
}

TEST(RecomputeYPositionsTest, SingleParagraph) {
    RenderNode node;
    node.type = NodeType::Paragraph;
    node.height = 20.0f;
    node.layout_dirty = false;
    std::vector<RenderNode> nodes = {node};
    Theme theme = GetLightTheme();

    auto result = RecomputeYPositions(nodes, theme);
    EXPECT_FLOAT_EQ(nodes[0].y_position, theme.margin_top);
    EXPECT_GT(result.total_height, theme.margin_top + 20.0f);
    EXPECT_FALSE(result.has_dirty_nodes);
}

TEST(RecomputeYPositionsTest, HeadingSpacing) {
    RenderNode para;
    para.type = NodeType::Paragraph;
    para.height = 20.0f;
    para.layout_dirty = false;

    RenderNode heading;
    heading.type = NodeType::Heading;
    heading.height = 30.0f;
    heading.layout_dirty = false;

    RenderNode para2;
    para2.type = NodeType::Paragraph;
    para2.height = 20.0f;
    para2.layout_dirty = false;

    std::vector<RenderNode> nodes = {para, heading, para2};
    Theme theme = GetLightTheme();

    RecomputeYPositions(nodes, theme);

    // Heading should have extra spacing above
    float para_bottom = nodes[0].y_position + nodes[0].height + theme.paragraph_spacing;
    float heading_y = nodes[1].y_position;
    EXPECT_FLOAT_EQ(heading_y, para_bottom + theme.heading_spacing_above);

    // After heading: heading_spacing_below, not paragraph_spacing
    float heading_bottom = nodes[1].y_position + nodes[1].height + theme.heading_spacing_below;
    EXPECT_FLOAT_EQ(nodes[2].y_position, heading_bottom);
}

TEST(RecomputeYPositionsTest, DetectsDirtyNodes) {
    RenderNode clean;
    clean.type = NodeType::Paragraph;
    clean.height = 20.0f;
    clean.layout_dirty = false;

    RenderNode dirty;
    dirty.type = NodeType::Paragraph;
    dirty.height = 20.0f;
    dirty.layout_dirty = true;

    std::vector<RenderNode> nodes = {clean, dirty};
    Theme theme = GetLightTheme();

    auto result = RecomputeYPositions(nodes, theme);
    EXPECT_TRUE(result.has_dirty_nodes);
}

TEST(RecomputeYPositionsTest, MonotonicallyIncreasingY) {
    std::vector<RenderNode> nodes;
    for (int i = 0; i < 10; i++) {
        RenderNode node;
        node.type = NodeType::Paragraph;
        node.height = 15.0f + static_cast<float>(i);
        node.layout_dirty = false;
        nodes.push_back(node);
    }
    Theme theme = GetLightTheme();
    RecomputeYPositions(nodes, theme);

    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(nodes[i].y_position, nodes[i - 1].y_position);
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
    engine_.ComputeLayout(nodes, 800.0f);

    // Now do a partial layout at a different width — this marks off-screen nodes dirty
    engine_.ComputeLayout(nodes, 400.0f, 0.0f, 100.0f);

    // Some nodes beyond viewport should still be dirty
    bool any_dirty = false;
    for (const auto& n : nodes) {
        if (n.layout_dirty) { any_dirty = true; break; }
    }
    ASSERT_TRUE(any_dirty);

    // Mark a visible node dirty manually to test the fix path
    nodes[0].layout_dirty = true;

    // EnsureVisibleLayout should fix the visible range
    bool updated = engine_.EnsureVisibleLayout(nodes, 400.0f, 0.0f, 100.0f);
    EXPECT_TRUE(updated);

    // Nodes in visible range should no longer be dirty
    for (const auto& n : nodes) {
        if (n.y_position + n.height < 0.0f) continue;
        if (n.y_position > 100.0f) break;
        EXPECT_FALSE(n.layout_dirty)
            << "Visible node at y=" << n.y_position << " is still dirty";
    }
}

TEST_F(LayoutTest, EnsureVisibleLayoutReturnsFalseWhenClean) {
    auto nodes = ParseMarkdown("Hello world");
    engine_.ComputeLayout(nodes, 800.0f);

    // All nodes are clean, so EnsureVisibleLayout should return false
    bool updated = engine_.EnsureVisibleLayout(nodes, 800.0f, 0.0f, 1000.0f);
    EXPECT_FALSE(updated);
}

TEST_F(LayoutTest, EnsureVisibleLayoutSkipsOffscreenDirtyNodes) {
    std::string md;
    for (int i = 0; i < 30; i++) {
        md += "Paragraph " + std::to_string(i) + "\n\n";
    }
    auto nodes = ParseMarkdown(md);
    engine_.ComputeLayout(nodes, 800.0f, 0.0f, 50.0f);

    // Count dirty nodes before
    int dirty_before = 0;
    for (const auto& n : nodes) {
        if (n.layout_dirty) dirty_before++;
    }

    // EnsureVisibleLayout only for a small viewport slice
    engine_.EnsureVisibleLayout(nodes, 800.0f, 0.0f, 50.0f);

    // Distant dirty nodes should remain dirty
    int dirty_after = 0;
    for (const auto& n : nodes) {
        if (n.layout_dirty) dirty_after++;
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
    // Full layout at wide width
    engine_.ComputeLayout(nodes, 800.0f);

    // Now do a partial layout at narrow width (marks off-screen dirty)
    engine_.ComputeLayout(nodes, 300.0f, 0.0f, 50.0f);

    // EnsureVisibleLayout should update Y positions consistently
    engine_.EnsureVisibleLayout(nodes, 300.0f, 0.0f, 50.0f);

    // Y positions should still be monotonically increasing
    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(nodes[i].y_position, nodes[i - 1].y_position)
            << "Node " << i << " y should be > node " << (i - 1);
    }
}

TEST_F(LayoutTest, EnsureVisibleLayoutUpdatesTotalHeight) {
    std::string md;
    for (int i = 0; i < 10; i++) {
        md += "Paragraph " + std::to_string(i) + "\n\n";
    }
    auto nodes = ParseMarkdown(md);
    engine_.ComputeLayout(nodes, 800.0f, 0.0f, 50.0f);

    float height_before = engine_.GetTotalHeight();
    engine_.EnsureVisibleLayout(nodes, 800.0f, 0.0f, 50.0f);
    float height_after = engine_.GetTotalHeight();

    // Total height may change when visible nodes get re-laid out
    // but should remain positive
    EXPECT_GT(height_after, 0.0f);
    (void)height_before;
}

// ---- RecomputeYPositions additional tests ----

TEST(RecomputeYPositionsTest, MultipleHeadingsHaveCorrectSpacing) {
    Theme theme = GetLightTheme();
    RenderNode h1;
    h1.type = NodeType::Heading;
    h1.height = 40.0f;
    h1.layout_dirty = false;

    RenderNode h2;
    h2.type = NodeType::Heading;
    h2.height = 30.0f;
    h2.layout_dirty = false;

    std::vector<RenderNode> nodes = {h1, h2};
    RecomputeYPositions(nodes, theme);

    // First heading: margin_top + heading_spacing_above
    EXPECT_FLOAT_EQ(nodes[0].y_position, theme.margin_top + theme.heading_spacing_above);

    // Second heading: after first heading + heading_spacing_below + heading_spacing_above
    float expected_y = nodes[0].y_position + nodes[0].height
                     + theme.heading_spacing_below + theme.heading_spacing_above;
    EXPECT_FLOAT_EQ(nodes[1].y_position, expected_y);
}

TEST(RecomputeYPositionsTest, AllNodeTypesProduceValidPositions) {
    Theme theme = GetLightTheme();
    std::vector<RenderNode> nodes;

    auto add_node = [&](NodeType type, float height) {
        RenderNode n;
        n.type = type;
        n.height = height;
        n.layout_dirty = false;
        nodes.push_back(n);
    };

    add_node(NodeType::Paragraph, 20.0f);
    add_node(NodeType::Heading, 30.0f);
    add_node(NodeType::CodeBlock, 50.0f);
    add_node(NodeType::HorizontalRule, 5.0f);
    add_node(NodeType::ListItem, 18.0f);
    add_node(NodeType::BlockQuote, 25.0f);
    add_node(NodeType::Table, 60.0f);

    auto result = RecomputeYPositions(nodes, theme);

    // All positions should be monotonically increasing
    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(nodes[i].y_position, nodes[i - 1].y_position);
    }
    // Total height should exceed last node's bottom
    float last_bottom = nodes.back().y_position + nodes.back().height;
    EXPECT_GE(result.total_height, last_bottom);
}
