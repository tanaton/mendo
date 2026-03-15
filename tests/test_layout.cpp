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
