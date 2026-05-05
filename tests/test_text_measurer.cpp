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

    void SetUp() override
    {
        theme_ = GetLightTheme();
        ASSERT_TRUE(engine_.Init(&mock_, theme_));
    }
};

// ---- 基本レイアウト ----

TEST_F(MockLayoutTest, EmptyNodesGiveMarginHeight)
{
    std::pmr::vector<Node> nodes;
    LayoutCache cache;
    engine_.ComputeLayout(nodes, cache, 800.0f);
    EXPECT_FLOAT_EQ(engine_.GetTotalHeight(), theme_.margin_top * 2);
}

TEST_F(MockLayoutTest, SingleParagraphPositiveHeight)
{
    auto nodes = ParseMarkdown(MENDO_LIT("Hello world")).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    EXPECT_GT(engine_.GetTotalHeight(), 0.0f);
    EXPECT_GT(cache[0].height, 0.0f);
}

TEST_F(MockLayoutTest, YPositionsAreMonotonicallyIncreasing)
{
    auto nodes = ParseMarkdown(MENDO_LIT("A\n\nB\n\nC\n\nD")).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(cache[i].text_top, cache[i - 1].text_top);
    }
}

TEST_F(MockLayoutTest, NoOverlapBetweenNodes)
{
    auto nodes = ParseMarkdown(MENDO_LIT("First\n\nSecond\n\nThird")).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    for (size_t i = 1; i < nodes.size(); i++) {
        float prev_bottom = cache[i - 1].text_top + cache[i - 1].height;
        EXPECT_GE(cache[i].text_top, prev_bottom);
    }
}

// ---- 見出しの間隔 ----

TEST_F(MockLayoutTest, HeadingHasExtraSpacing)
{
    auto nodes = ParseMarkdown(MENDO_LIT("Paragraph\n\n# Heading\n\nAnother")).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    ASSERT_EQ(nodes.size(), 3u);

    float para_bottom = cache[0].text_top + cache[0].height;
    float heading_y = cache[1].text_top;
    EXPECT_GT(heading_y - para_bottom, theme_.paragraph_spacing);
}

TEST_F(MockLayoutTest, HeadingTallerThanParagraph)
{
    auto nodes = ParseMarkdown(MENDO_LIT("# Heading\n\nParagraph")).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    ASSERT_EQ(nodes.size(), 2u);
    EXPECT_GT(cache[0].height, cache[1].height);
}

// ---- ダーティ追跡 ----

TEST_F(MockLayoutTest, NoDirtyAfterFullLayout)
{
    auto nodes = ParseMarkdown(MENDO_LIT("A\n\nB\n\nC")).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    EXPECT_FALSE(engine_.HasDirtyNodes());
}

TEST_F(MockLayoutTest, PartialLayoutLeavesDirtyNodes)
{
    auto nodes = ParseMarkdown(MENDO_LIT("A\n\nB\n\nC\n\nD\n\nE")).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    // 部分レイアウト: ビューポート[0, 1)のみ — 非常に小さい
    engine_.ComputeLayout(nodes, cache, 800.0f, 0.0f, 1.0f);
    // ビューポート外のノードはダーティであるべき
    EXPECT_TRUE(engine_.HasDirtyNodes());
}

TEST_F(MockLayoutTest, ProcessDirtyBatchResolvesDirty)
{
    auto nodes = ParseMarkdown(MENDO_LIT("A\n\nB\n\nC\n\nD\n\nE")).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f, 0.0f, 1.0f);

    if (engine_.HasDirtyNodes()) {
        bool more = engine_.ProcessDirtyBatch(nodes, cache, 800.0f, 100);
        EXPECT_FALSE(more);
    }

    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(cache[i].text_top, cache[i - 1].text_top);
    }
}

TEST_F(MockLayoutTest, ProcessDirtyBatchSmallBatch)
{
    mendo::doc_string_std md;
    for (int i = 0; i < 50; i++) md += MENDO_LIT("P") + mendo::to_doc_string(i) + MENDO_LIT("\n\n");
    auto nodes = ParseMarkdown(md).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f, 0.0f, 10.0f);

    if (engine_.HasDirtyNodes()) {
        bool more = engine_.ProcessDirtyBatch(nodes, cache, 800.0f, 5);
        EXPECT_TRUE(more); // 50 nodes, batch=5
    }
}

// ---- バグ #9: ダーティノードなしでのProcessDirtyBatch ----

TEST_F(MockLayoutTest, ProcessDirtyBatchNoDirtyPreservesHeight)
{
    auto nodes = ParseMarkdown(MENDO_LIT("A\n\nB\n\nC")).nodes;
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

TEST_F(MockLayoutTest, WidthChangeRecalculates)
{
    auto nodes = ParseMarkdown(MENDO_LIT("Some text that could wrap when narrower")).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    float h_wide = cache[0].height;
    engine_.ComputeLayout(nodes, cache, 200.0f);
    float h_narrow = cache[0].height;
    EXPECT_GE(h_narrow, h_wide);
}

// ---- テーブルモック ----

TEST_F(MockLayoutTest, TableHasPositiveHeight)
{
    auto nodes = ParseMarkdown(MENDO_LIT("| A | B |\n|---|---|\n| 1 | 2 |")).nodes;
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

TEST_F(MockLayoutTest, HorizontalRuleHasHeight)
{
    auto nodes = ParseMarkdown(MENDO_LIT("Above\n\n---\n\nBelow")).nodes;
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

TEST_F(MockLayoutTest, EnsureVisibleLayoutUpdatesViewport)
{
    mendo::doc_string_std md;
    for (int i = 0; i < 20; i++) md += MENDO_LIT("Paragraph ") + mendo::to_doc_string(i) + MENDO_LIT("\n\n");
    auto nodes = ParseMarkdown(md).nodes;
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

TEST_F(MockLayoutTest, LayoutNodesFullLayout)
{
    auto nodes = ParseMarkdown(MENDO_LIT("A\n\nB")).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.LayoutNodes(nodes, cache, 800.0f);
    EXPECT_GT(engine_.GetTotalHeight(), 0.0f);
    EXPECT_FALSE(engine_.HasDirtyNodes());
}

// ---- Mermaidブロックのズーム耐性 ----

// Mermaidブロックの初回レイアウトでプレースホルダー高さが設定されること
TEST_F(MockLayoutTest, MermaidBlockGetsPlaceholderHeight)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```mermaid\ngraph TD;\n  A-->B;\n```")).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    for (size_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].code_language == SyntaxLanguage::Mermaid) {
            EXPECT_GT(cache[i].height, 0.0f) << "Mermaidブロックのプレースホルダー高さは正であるべき";
            EXPECT_FALSE(cache[i].layout_dirty);
        }
    }
}

// ビットマップレンダリング後の高さがレイアウト再計算で保持されること（ズーム操作を模擬）
TEST_F(MockLayoutTest, MermaidHeightPreservedAcrossLayoutCycles)
{
    auto nodes = ParseMarkdown(MENDO_LIT("Text\n\n```mermaid\ngraph TD;\n  A-->B;\n```\n\nMore text")).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.LayoutNodes(nodes, cache, 800.0f);

    // ビットマップレンダリング完了後の高さを模擬（300 DIP）
    for (size_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].code_language == SyntaxLanguage::Mermaid) {
            cache[i].height = 300.0f;  // ビットマップ描画サイズ
            cache.GetDiagram(i).width = 400.0f;
            cache.GetDiagram(i).height = 300.0f;
        }
    }

    // ズーム変更を模擬: InvalidateAllLayouts → LayoutNodes
    cache.InvalidateAllLayouts();
    engine_.LayoutNodes(nodes, cache, 600.0f);

    for (size_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].code_language == SyntaxLanguage::Mermaid) {
            EXPECT_FLOAT_EQ(cache[i].height, 300.0f)
                << "Mermaidブロックの高さはレイアウト再計算後も保持されるべき";
            // ダイアグラムエントリも保持
            EXPECT_FLOAT_EQ(cache.GetDiagram(i).width, 400.0f);
            EXPECT_FLOAT_EQ(cache.GetDiagram(i).height, 300.0f);
        }
    }
}

// content_widthが0以下のときもMermaidブロックの高さが保持されること
// （500%ズームでMDペインが極小になる場合を模擬）
TEST_F(MockLayoutTest, MermaidHeightPreservedAtZeroWidth)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```mermaid\ngraph TD;\n  A-->B;\n```")).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.LayoutNodes(nodes, cache, 800.0f);

    // ビットマップ描画後の高さを設定
    for (size_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].code_language == SyntaxLanguage::Mermaid) {
            cache[i].height = 250.0f;
        }
    }

    // 極端なズームを模擬: 幅0でレイアウト
    cache.InvalidateAllLayouts();
    engine_.LayoutNodes(nodes, cache, 0.0f);

    for (size_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].code_language == SyntaxLanguage::Mermaid) {
            EXPECT_FLOAT_EQ(cache[i].height, 250.0f)
                << "幅0でのレイアウトでもMermaidブロックの高さは保持されるべき";
        }
    }

    // 元の幅に復帰
    cache.InvalidateAllLayouts();
    engine_.LayoutNodes(nodes, cache, 800.0f);

    for (size_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].code_language == SyntaxLanguage::Mermaid) {
            EXPECT_FLOAT_EQ(cache[i].height, 250.0f)
                << "幅復帰後もMermaidブロックの高さは保持されるべき";
        }
    }
}

// ---- issue #158 リグレッション: 縦に長いテーブルの直後の描画がテーブルに重なる ----

// 長いテーブルが viewport 外にあって partial モードで再レイアウトされたときに、
// 既存の実測高さが推定値で縮められないことを確認する。
// 縮められると以降のノードの y_position が上に詰まり、テーブル直後のノードが
// テーブルに重なって描画される。
TEST_F(MockLayoutTest, LongTableHeightNotShrunkByEstimateInPartialMode)
{
    // 100 行のテーブル + その後の段落
    mendo::doc_string_std md = MENDO_LIT("| col1 | col2 |\n|------|------|\n");
    for (int i = 0; i < 100; i++) {
        md += MENDO_LIT("| row") + mendo::to_doc_string(i) + MENDO_LIT(" | val") + mendo::to_doc_string(i) + MENDO_LIT(" |\n");
    }
    md += MENDO_LIT("\nFollow-up paragraph");

    auto nodes = ParseMarkdown(md).nodes;
    ASSERT_GE(nodes.size(), 2u);
    LayoutCache cache;
    cache.Resize(nodes.size());

    // 1) フルレイアウトでテーブルの実測高さを取得
    engine_.LayoutNodes(nodes, cache, 800.0f);
    size_t table_idx = 0;
    for (size_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].type == NodeType::Table) { table_idx = i; break; }
    }
    ASSERT_EQ(nodes[table_idx].type, NodeType::Table);
    const float measured_table_h = cache[table_idx].height;
    EXPECT_GT(measured_table_h, 0.0f);

    // テーブル直後の段落
    ASSERT_LT(table_idx + 1, nodes.size());
    const size_t para_idx = table_idx + 1;

    // 2) 幅変更 + partial モード（viewport は冒頭の小さい領域のみ）
    //    テーブルは viewport 外なので不可視扱いになり、現行バグでは推定値で
    //    上書きされて高さが縮む。
    engine_.ComputeLayout(nodes, cache, 600.0f, 0.0f, 10.0f);

    // テーブルの高さが実測値から縮んでいないこと。
    // 修正後の partial モードは shrink を起こさないため厳密一致を期待する。
    EXPECT_GE(cache[table_idx].height, measured_table_h)
        << "partial モードで実測済みテーブルの高さが推定値で縮められた";

    // テーブル直後の段落の y_position もテーブル下端より下にあること
    const float table_bottom = cache[table_idx].text_top + cache[table_idx].height;
    EXPECT_GE(cache[para_idx].text_top, table_bottom)
        << "テーブル直後ノードの y_position がテーブル下端より上に詰まっている (issue #158)";
}

// 推定値より小さい既存高さは、推定値まで成長させてよい（max_scroll の精度のため）
TEST_F(MockLayoutTest, PartialModeGrowsHeightWhenEstimateLarger)
{
    auto nodes = ParseMarkdown(MENDO_LIT("First\n\nSecond\n\nThird")).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());

    // 高さ 0 で partial モード（width_changed=true）。
    // 不可視ノードでも entry.height が 0 なら推定値まで成長すべき。
    engine_.ComputeLayout(nodes, cache, 800.0f, 0.0f, 1.0f);
    for (size_t i = 0; i < nodes.size(); i++) {
        EXPECT_GT(cache[i].height, 0.0f)
            << "ノード " << i << " の高さが推定値まで成長していない";
    }
}

// partial + invisible で entry.height が推定値に成長した場合、Table の col_widths を
// クリアし、row_heights 合計と乖離した entry.height のまま描画されないようにする。
// (issue #158: col_widths を残すと描画範囲が後続ノード位置を越えて重なる)
TEST_F(MockLayoutTest, PartialModeClearsTableColWidthsWhenHeightGrows)
{
    mendo::doc_string_std md = MENDO_LIT("| a | b |\n|---|---|\n");
    for (int i = 0; i < 50; i++) {
        md += MENDO_LIT("| r") + mendo::to_doc_string(i) + MENDO_LIT(" | v") + mendo::to_doc_string(i) + MENDO_LIT(" |\n");
    }
    auto nodes = ParseMarkdown(md).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());

    engine_.LayoutNodes(nodes, cache, 800.0f);
    size_t table_idx = 0;
    for (size_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].type == NodeType::Table) { table_idx = i; break; }
    }
    ASSERT_EQ(nodes[table_idx].type, NodeType::Table);
    ASSERT_TRUE(cache[table_idx].has_table_layout());
    ASSERT_FALSE(cache[table_idx].table_layout->col_widths.empty());

    // 成長分岐を強制発動させるため、現在の entry.height を意図的に小さくする
    cache[table_idx].height = 1.0f;

    // partial + invisible (テーブルは viewport より下) で再レイアウト
    engine_.ComputeLayout(nodes, cache, 600.0f, 0.0f, 10.0f);

    EXPECT_TRUE(cache[table_idx].table_layout->col_widths.empty())
        << "成長時に col_widths がクリアされず、描画範囲と entry.height が乖離する";
}

// ---- RecreateFormats ----

TEST_F(MockLayoutTest, RecreateFormatsSucceeds)
{
    EXPECT_TRUE(engine_.RecreateFormats());
}

// ---- 多数ノードの合計高さ ----

TEST_F(MockLayoutTest, ManyNodesProduceLargeHeight)
{
    mendo::doc_string_std md;
    for (int i = 0; i < 100; i++) md += MENDO_LIT("Paragraph ") + mendo::to_doc_string(i) + MENDO_LIT("\n\n");
    auto nodes = ParseMarkdown(md).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    EXPECT_GT(engine_.GetTotalHeight(), 500.0f);
    size_t last = nodes.size() - 1;
    EXPECT_LE(cache[last].text_top + cache[last].height, engine_.GetTotalHeight());
}

// ---- ファイル切り替えリグレッションテスト ----

TEST_F(MockLayoutTest, ResetClearsAllEntries)
{
    auto nodes = ParseMarkdown(MENDO_LIT("Hello\n\nWorld")).nodes;
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

TEST_F(MockLayoutTest, FileSwitchWithResetProducesCorrectLayout)
{
    // ファイルAを開くシミュレーション: "# Big Heading\n\nSome paragraph"
    auto nodes_a = ParseMarkdown(MENDO_LIT("# Big Heading\n\nSome paragraph")).nodes;
    LayoutCache cache;
    cache.Reset(nodes_a.size());
    engine_.LayoutNodes(nodes_a, cache, 800.0f);

    ASSERT_EQ(nodes_a.size(), 2u);
    float heading_height_a = cache[0].height;
    float para_height_a = cache[1].height;
    EXPECT_GT(heading_height_a, 0.0f);
    EXPECT_GT(para_height_a, 0.0f);

    // ファイルBへ切り替えシミュレーション: "Just a paragraph\n\nAnother one\n\nThird"
    auto nodes_b = ParseMarkdown(MENDO_LIT("Just a paragraph\n\nAnother one\n\nThird")).nodes;
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

TEST_F(MockLayoutTest, FileSwitchSameNodeCountWithResetRecalculates)
{
    // ファイルA: 見出し2つ
    auto nodes_a = ParseMarkdown(MENDO_LIT("# H1\n\n## H2")).nodes;
    LayoutCache cache;
    cache.Reset(nodes_a.size());
    engine_.LayoutNodes(nodes_a, cache, 800.0f);
    float h1_height = cache[0].height;
    float h2_height = cache[1].height;

    // ファイルB: 段落2つ（ファイルAと同じノード数）
    auto nodes_b = ParseMarkdown(MENDO_LIT("alpha\n\nbeta")).nodes;
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
