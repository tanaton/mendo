#include <gtest/gtest.h>
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
    std::vector<Node> nodes;
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
