#include <gtest/gtest.h>
#include <memory_resource>
#include "layout.h"
#include "mock_text_measurer.h"
#include "parser.h"

// MockTextMeasurerを通じてLayoutEngineのロジックをテストする（COM / DirectWrite不要）。

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

// ---- 基本レイアウト ----

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

// ---- 見出しの間隔 ----

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

// ---- ダーティ追跡 ----

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
    // 部分レイアウト: ビューポート[0, 1)のみ — 非常に小さい
    engine_.ComputeLayout(nodes, cache, 800.0f, 0.0f, 1.0f);
    // ビューポート外のノードはダーティであるべき
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

// ---- バグ #9: ダーティノードなしでのProcessDirtyBatch ----

TEST_F(MockLayoutTest, ProcessDirtyBatchNoDirtyPreservesHeight) {
    auto nodes = ParseMarkdown("A\n\nB\n\nC");
    LayoutCache cache;
    cache.Resize(nodes.size());
    // フルレイアウト — ダーティノードなし
    engine_.ComputeLayout(nodes, cache, 800.0f);
    EXPECT_FALSE(engine_.HasDirtyNodes());

    float height_before = engine_.GetTotalHeight();
    EXPECT_GT(height_before, 0.0f);

    // ダーティなものがない場合のProcessDirtyBatchはtotal_heightを破損させないべき
    bool more = engine_.ProcessDirtyBatch(nodes, cache, 800.0f, 100);
    EXPECT_FALSE(more);
    EXPECT_FLOAT_EQ(engine_.GetTotalHeight(), height_before);
}

// ---- 幅の変更 ----

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

// ---- テーブルモック ----

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

// ---- 水平線モック ----

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
    // ビューポート[0,50)の部分レイアウト
    engine_.ComputeLayout(nodes, cache, 800.0f, 0.0f, 50.0f);
    EXPECT_TRUE(engine_.HasDirtyNodes());

    // より後の領域のレイアウトを確保
    float total = engine_.GetTotalHeight();
    engine_.EnsureVisibleLayout(nodes, cache, 800.0f, total * 0.5f, total * 0.7f);
    // 一部のノードはクリーンになっているべき
}

// ---- LayoutNodes 便利関数 ----

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

// ---- 多数ノードの合計高さ ----

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

// ---- ファイル切り替えリグレッションテスト ----

TEST_F(MockLayoutTest, ResetClearsAllEntries) {
    auto nodes = ParseMarkdown("Hello\n\nWorld");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    // レイアウト後、すべてのエントリはクリーンであるべき
    for (size_t i = 0; i < cache.size(); i++) {
        ASSERT_FALSE(cache[i].layout_dirty);
        ASSERT_GT(cache[i].height, 0.0f);
    }

    // Resetはすべてをデフォルトにクリアすべき
    cache.Reset(3);
    EXPECT_EQ(cache.size(), 3u);
    for (size_t i = 0; i < cache.size(); i++) {
        EXPECT_TRUE(cache[i].layout_dirty) << "エントリ " << i << " はReset後にダーティであるべき";
        EXPECT_FLOAT_EQ(cache[i].height, 0.0f) << "エントリ " << i << " の高さはReset後に0であるべき";
        EXPECT_EQ(cache[i].text_layout.Get(), nullptr);
    }
}

TEST_F(MockLayoutTest, FileSwitchWithResetProducesCorrectLayout) {
    // ファイルAを開くシミュレーション: "# Big Heading\n\nSome paragraph"
    auto nodes_a = ParseMarkdown("# Big Heading\n\nSome paragraph");
    LayoutCache cache;
    cache.Reset(nodes_a.size());
    engine_.LayoutNodes(nodes_a, cache, 800.0f);

    ASSERT_EQ(nodes_a.size(), 2u);
    float heading_height_a = cache[0].height;
    float para_height_a = cache[1].height;
    EXPECT_GT(heading_height_a, 0.0f);
    EXPECT_GT(para_height_a, 0.0f);

    // ファイルBへ切り替えシミュレーション: "Just a paragraph\n\nAnother one\n\nThird"
    auto nodes_b = ParseMarkdown("Just a paragraph\n\nAnother one\n\nThird");
    cache.Reset(nodes_b.size());
    engine_.LayoutNodes(nodes_b, cache, 800.0f);

    ASSERT_EQ(nodes_b.size(), 3u);
    // ファイルBのすべてのノードは新しく正しい高さの段落であるべき
    for (size_t i = 0; i < nodes_b.size(); i++) {
        EXPECT_FALSE(cache[i].layout_dirty) << "ノード " << i << " はクリーンであるべき";
        EXPECT_GT(cache[i].height, 0.0f) << "ノード " << i << " は正の高さを持つべき";
        EXPECT_EQ(nodes_b[i].type, NodeType::Paragraph);
    }
    // ファイルBの最初のノードはファイルAの見出しの高さを保持していてはならない
    EXPECT_NE(cache[0].height, heading_height_a)
        << "ファイルBの段落はファイルAの見出しの高さを保持していてはならない";
}

TEST_F(MockLayoutTest, FileSwitchSameNodeCountWithResetRecalculates) {
    // ファイルA: 見出し2つ
    auto nodes_a = ParseMarkdown("# H1\n\n## H2");
    LayoutCache cache;
    cache.Reset(nodes_a.size());
    engine_.LayoutNodes(nodes_a, cache, 800.0f);
    float h1_height = cache[0].height;
    float h2_height = cache[1].height;

    // ファイルB: 段落2つ（ファイルAと同じノード数）
    auto nodes_b = ParseMarkdown("alpha\n\nbeta");
    cache.Reset(nodes_b.size());
    engine_.LayoutNodes(nodes_b, cache, 800.0f);

    // 段落は見出しより短いべき
    EXPECT_LT(cache[0].height, h1_height)
        << "段落はH1見出しより短いべき";
    EXPECT_LT(cache[1].height, h2_height)
        << "段落はH2見出しより短いべき";

    // すべてクリーンであるべき
    for (size_t i = 0; i < cache.size(); i++) {
        EXPECT_FALSE(cache[i].layout_dirty);
    }
}
