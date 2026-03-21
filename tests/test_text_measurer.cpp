#include <gtest/gtest.h>
#include <memory_resource>
#include "layout.h"
#include "mock_text_measurer.h"
#include "parser.h"

// Tests LayoutEngine logic through MockTextMeasurer (no COM / DirectWrite required).

class MockLayoutTest : public ::testing::Test {
protected:
    MockTextMeasurer mock_;
    LayoutEngine engine_;
    Theme theme_;

    void SetUp() override {
        theme_ = GetLightTheme();
        ASSERT_TRUE(engine_.Init(&mock_, theme_));
    }
};

// ---- Basic layout ----

TEST_F(MockLayoutTest, EmptyNodesGiveMarginHeight) {
    std::pmr::vector<Node> nodes;
    LayoutCache cache;
    engine_.ComputeLayout(nodes, cache, 800.0f);
    EXPECT_FLOAT_EQ(engine_.GetTotalHeight(), theme_.margin_top * 2);
}

TEST_F(MockLayoutTest, SingleParagraphPositiveHeight) {
    auto nodes = ParseMarkdown("Hello world");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    EXPECT_GT(engine_.GetTotalHeight(), 0.0f);
    EXPECT_GT(cache[0].height, 0.0f);
}

TEST_F(MockLayoutTest, YPositionsAreMonotonicallyIncreasing) {
    auto nodes = ParseMarkdown("A\n\nB\n\nC\n\nD");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(cache[i].y_position, cache[i - 1].y_position);
    }
}

TEST_F(MockLayoutTest, NoOverlapBetweenNodes) {
    auto nodes = ParseMarkdown("First\n\nSecond\n\nThird");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    for (size_t i = 1; i < nodes.size(); i++) {
        float prev_bottom = cache[i - 1].y_position + cache[i - 1].height;
        EXPECT_GE(cache[i].y_position, prev_bottom);
    }
}

// ---- Heading spacing ----

TEST_F(MockLayoutTest, HeadingHasExtraSpacing) {
    auto nodes = ParseMarkdown("Paragraph\n\n# Heading\n\nAnother");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    ASSERT_EQ(nodes.size(), 3u);

    float para_bottom = cache[0].y_position + cache[0].height;
    float heading_y = cache[1].y_position;
    EXPECT_GT(heading_y - para_bottom, theme_.paragraph_spacing);
}

TEST_F(MockLayoutTest, HeadingTallerThanParagraph) {
    auto nodes = ParseMarkdown("# Heading\n\nParagraph");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    ASSERT_EQ(nodes.size(), 2u);
    EXPECT_GT(cache[0].height, cache[1].height);
}

// ---- Dirty tracking ----

TEST_F(MockLayoutTest, NoDirtyAfterFullLayout) {
    auto nodes = ParseMarkdown("A\n\nB\n\nC");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    EXPECT_FALSE(engine_.HasDirtyNodes());
}

TEST_F(MockLayoutTest, PartialLayoutLeavesDirtyNodes) {
    auto nodes = ParseMarkdown("A\n\nB\n\nC\n\nD\n\nE");
    LayoutCache cache;
    cache.Resize(nodes.size());
    // Partial layout: only viewport [0, 1) — very small
    engine_.ComputeLayout(nodes, cache, 800.0f, 0.0f, 1.0f);
    // Some nodes outside viewport should be dirty
    EXPECT_TRUE(engine_.HasDirtyNodes());
}

TEST_F(MockLayoutTest, ProcessDirtyBatchResolvesDirty) {
    auto nodes = ParseMarkdown("A\n\nB\n\nC\n\nD\n\nE");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f, 0.0f, 1.0f);

    if (engine_.HasDirtyNodes()) {
        bool more = engine_.ProcessDirtyBatch(nodes, cache, 800.0f, 100);
        EXPECT_FALSE(more);
    }

    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(cache[i].y_position, cache[i - 1].y_position);
    }
}

TEST_F(MockLayoutTest, ProcessDirtyBatchSmallBatch) {
    std::string md;
    for (int i = 0; i < 50; i++) md += "P" + std::to_string(i) + "\n\n";
    auto nodes = ParseMarkdown(md);
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f, 0.0f, 10.0f);

    if (engine_.HasDirtyNodes()) {
        bool more = engine_.ProcessDirtyBatch(nodes, cache, 800.0f, 5);
        EXPECT_TRUE(more); // 50 nodes, batch=5
    }
}

// ---- Bug #9: ProcessDirtyBatch with no dirty nodes ----

TEST_F(MockLayoutTest, ProcessDirtyBatchNoDirtyPreservesHeight) {
    auto nodes = ParseMarkdown("A\n\nB\n\nC");
    LayoutCache cache;
    cache.Resize(nodes.size());
    // Full layout — no dirty nodes
    engine_.ComputeLayout(nodes, cache, 800.0f);
    EXPECT_FALSE(engine_.HasDirtyNodes());

    float height_before = engine_.GetTotalHeight();
    EXPECT_GT(height_before, 0.0f);

    // ProcessDirtyBatch with nothing dirty should not corrupt total_height
    bool more = engine_.ProcessDirtyBatch(nodes, cache, 800.0f, 100);
    EXPECT_FALSE(more);
    EXPECT_FLOAT_EQ(engine_.GetTotalHeight(), height_before);
}

// ---- Width change ----

TEST_F(MockLayoutTest, WidthChangeRecalculates) {
    auto nodes = ParseMarkdown("Some text that could wrap when narrower");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    float h_wide = cache[0].height;
    engine_.ComputeLayout(nodes, cache, 200.0f);
    float h_narrow = cache[0].height;
    EXPECT_GE(h_narrow, h_wide);
}

// ---- Table mock ----

TEST_F(MockLayoutTest, TableHasPositiveHeight) {
    auto nodes = ParseMarkdown("| A | B |\n|---|---|\n| 1 | 2 |");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    bool found_table = false;
    for (size_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].type == NodeType::Table) {
            EXPECT_GT(cache[i].height, 0.0f);
            found_table = true;
        }
    }
    EXPECT_TRUE(found_table);
}

// ---- HorizontalRule mock ----

TEST_F(MockLayoutTest, HorizontalRuleHasHeight) {
    auto nodes = ParseMarkdown("Above\n\n---\n\nBelow");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    for (size_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].type == NodeType::HorizontalRule) {
            EXPECT_GT(cache[i].height, 0.0f);
        }
    }
}

// ---- EnsureVisibleLayout ----

TEST_F(MockLayoutTest, EnsureVisibleLayoutUpdatesViewport) {
    std::string md;
    for (int i = 0; i < 20; i++) md += "Paragraph " + std::to_string(i) + "\n\n";
    auto nodes = ParseMarkdown(md);
    LayoutCache cache;
    cache.Resize(nodes.size());
    // Partial layout for viewport [0,50)
    engine_.ComputeLayout(nodes, cache, 800.0f, 0.0f, 50.0f);
    EXPECT_TRUE(engine_.HasDirtyNodes());

    // Now ensure layout for a later region
    float total = engine_.GetTotalHeight();
    engine_.EnsureVisibleLayout(nodes, cache, 800.0f, total * 0.5f, total * 0.7f);
    // Some nodes should now be clean
}

// ---- LayoutNodes convenience ----

TEST_F(MockLayoutTest, LayoutNodesFullLayout) {
    auto nodes = ParseMarkdown("A\n\nB");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.LayoutNodes(nodes, cache, 800.0f);
    EXPECT_GT(engine_.GetTotalHeight(), 0.0f);
    EXPECT_FALSE(engine_.HasDirtyNodes());
}

// ---- RecreateFormats ----

TEST_F(MockLayoutTest, RecreateFormatsSucceeds) {
    EXPECT_TRUE(engine_.RecreateFormats());
}

// ---- Many nodes total height ----

TEST_F(MockLayoutTest, ManyNodesProduceLargeHeight) {
    std::string md;
    for (int i = 0; i < 100; i++) md += "Paragraph " + std::to_string(i) + "\n\n";
    auto nodes = ParseMarkdown(md);
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    EXPECT_GT(engine_.GetTotalHeight(), 500.0f);
    size_t last = nodes.size() - 1;
    EXPECT_LE(cache[last].y_position + cache[last].height, engine_.GetTotalHeight());
}

// ---- File switch regression tests ----

TEST_F(MockLayoutTest, ResetClearsAllEntries) {
    auto nodes = ParseMarkdown("Hello\n\nWorld");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    // All entries should be clean after layout
    for (size_t i = 0; i < cache.size(); i++) {
        ASSERT_FALSE(cache[i].layout_dirty);
        ASSERT_GT(cache[i].height, 0.0f);
    }

    // Reset should clear everything back to defaults
    cache.Reset(3);
    EXPECT_EQ(cache.size(), 3u);
    for (size_t i = 0; i < cache.size(); i++) {
        EXPECT_TRUE(cache[i].layout_dirty) << "Entry " << i << " should be dirty after Reset";
        EXPECT_FLOAT_EQ(cache[i].height, 0.0f) << "Entry " << i << " height should be 0 after Reset";
        EXPECT_EQ(cache[i].text_layout.Get(), nullptr);
    }
}

TEST_F(MockLayoutTest, FileSwitchWithResetProducesCorrectLayout) {
    // Simulate opening file A: "# Big Heading\n\nSome paragraph"
    auto nodes_a = ParseMarkdown("# Big Heading\n\nSome paragraph");
    LayoutCache cache;
    cache.Reset(nodes_a.size());
    engine_.LayoutNodes(nodes_a, cache, 800.0f);

    ASSERT_EQ(nodes_a.size(), 2u);
    float heading_height_a = cache[0].height;
    float para_height_a = cache[1].height;
    EXPECT_GT(heading_height_a, 0.0f);
    EXPECT_GT(para_height_a, 0.0f);

    // Simulate switching to file B: "Just a paragraph\n\nAnother one\n\nThird"
    auto nodes_b = ParseMarkdown("Just a paragraph\n\nAnother one\n\nThird");
    cache.Reset(nodes_b.size());
    engine_.LayoutNodes(nodes_b, cache, 800.0f);

    ASSERT_EQ(nodes_b.size(), 3u);
    // All nodes in file B should be paragraphs with fresh, correct heights
    for (size_t i = 0; i < nodes_b.size(); i++) {
        EXPECT_FALSE(cache[i].layout_dirty) << "Node " << i << " should be clean";
        EXPECT_GT(cache[i].height, 0.0f) << "Node " << i << " should have positive height";
        EXPECT_EQ(nodes_b[i].type, NodeType::Paragraph);
    }
    // First node of file B should NOT have old heading height from file A
    EXPECT_NE(cache[0].height, heading_height_a)
        << "File B paragraph should not retain file A heading height";
}

TEST_F(MockLayoutTest, FileSwitchSameNodeCountWithResetRecalculates) {
    // File A: 2 headings
    auto nodes_a = ParseMarkdown("# H1\n\n## H2");
    LayoutCache cache;
    cache.Reset(nodes_a.size());
    engine_.LayoutNodes(nodes_a, cache, 800.0f);
    float h1_height = cache[0].height;
    float h2_height = cache[1].height;

    // File B: 2 paragraphs (same node count as file A)
    auto nodes_b = ParseMarkdown("alpha\n\nbeta");
    cache.Reset(nodes_b.size());
    engine_.LayoutNodes(nodes_b, cache, 800.0f);

    // Paragraphs should be shorter than headings
    EXPECT_LT(cache[0].height, h1_height)
        << "Paragraph should be shorter than H1 heading";
    EXPECT_LT(cache[1].height, h2_height)
        << "Paragraph should be shorter than H2 heading";

    // All should be clean
    for (size_t i = 0; i < cache.size(); i++) {
        EXPECT_FALSE(cache[i].layout_dirty);
    }
}
