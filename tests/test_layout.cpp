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
