#include <gtest/gtest.h>
#include <memory_resource>
#include "document_test_helpers.h"
#include "mock_text_measurer.h"
#include "test_helpers.h"
#include "test_nodes.h"

// MockTextMeasurerを通じてLayoutEngineのロジックをテストする（COM / DirectWrite不要）。

class MockLayoutTest : public MockLayoutTestBase {
protected:
    // content 幅が content_width になるビューポート幅。
    float ViewportWidthFor(float content_width) const
    {
        return content_width + theme_.margin_left + theme_.margin_right;
    }
    // content 幅で全ノードを計測する。
    void LayoutAll(std::pmr::vector<Node>& nodes, LayoutCache& cache, float content_width)
    {
        ComputeLayout(engine_, nodes, cache, ViewportWidthFor(content_width));
    }
};

// ---- 基本レイアウト ----

TEST_F(MockLayoutTest, EmptyNodesGiveMarginHeight)
{
    std::pmr::vector<Node> nodes;
    LayoutCache cache;
    ComputeLayout(engine_, nodes, cache, 800.0f);
    EXPECT_FLOAT_EQ(ComputeTotalContentHeight(cache, theme_.margin_bottom), 0.0f);
}

TEST_F(MockLayoutTest, SingleParagraphPositiveHeight)
{
    auto [nodes, cache] = ParseAndLayout("Hello world");
    EXPECT_GT(ComputeTotalContentHeight(cache, theme_.margin_bottom), 0.0f);
    EXPECT_GT(cache[0].height, 0.0f);
}

TEST_F(MockLayoutTest, YPositionsAreMonotonicallyIncreasing)
{
    auto [nodes, cache] = ParseAndLayout("A\n\nB\n\nC\n\nD");
    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(cache.Top(i), cache.Top(i - 1));
    }
}

TEST_F(MockLayoutTest, NoOverlapBetweenNodes)
{
    auto [nodes, cache] = ParseAndLayout("First\n\nSecond\n\nThird");
    for (size_t i = 1; i < nodes.size(); i++) {
        float prev_bottom = cache.Top(i - 1) + cache[i - 1].height;
        EXPECT_GE(cache.Top(i), prev_bottom);
    }
}

// ---- 見出しの間隔 ----

TEST_F(MockLayoutTest, HeadingHasExtraSpacing)
{
    auto [nodes, cache] = ParseAndLayout("Paragraph\n\n# Heading\n\nAnother");
    ASSERT_EQ(nodes.size(), 3u);

    float para_bottom = cache.Top(0) + cache[0].height;
    float heading_y = cache.Top(1);
    EXPECT_GT(heading_y - para_bottom, theme_.paragraph_spacing);
}

TEST_F(MockLayoutTest, HeadingTallerThanParagraph)
{
    auto [nodes, cache] = ParseAndLayout("# Heading\n\nParagraph");
    ASSERT_EQ(nodes.size(), 2u);
    EXPECT_GT(cache[0].height, cache[1].height);
}

// ---- ダーティ追跡 ----

TEST_F(MockLayoutTest, NoDirtyAfterFullLayout)
{
    auto [nodes, cache] = ParseAndLayout("A\n\nB\n\nC");
    EXPECT_FALSE(cache.HasDirtyNodes());
}

TEST_F(MockLayoutTest, PartialLayoutLeavesDirtyNodes)
{
    // 部分レイアウト: ビューポート[0, 1)のみ — 非常に小さい
    auto [nodes, cache] = ParseAndLayout("A\n\nB\n\nC\n\nD\n\nE", 800.0f, 0.0f, 1.0f);
    // ビューポート外のノードはダーティであるべき
    EXPECT_TRUE(cache.HasDirtyNodes());
}

TEST_F(MockLayoutTest, ProcessDirtyBatchResolvesDirty)
{
    auto [nodes, cache] = ParseAndLayout("A\n\nB\n\nC\n\nD\n\nE", 800.0f, 0.0f, 1.0f);

    if (cache.HasDirtyNodes()) {
        bool more = engine_.ProcessDirtyBatch(nodes, cache, 800.0f, kNoLayoutDeadline, YRange::All());
        EXPECT_FALSE(more);
    }

    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(cache.Top(i), cache.Top(i - 1));
    }
}

// ---- バグ #9: ダーティノードなしでのProcessDirtyBatch ----

TEST_F(MockLayoutTest, ProcessDirtyBatchNoDirtyPreservesHeight)
{
    auto [nodes, cache] = ParseAndLayout("A\n\nB\n\nC");
    EXPECT_FALSE(cache.HasDirtyNodes());

    float height_before = ComputeTotalContentHeight(cache, theme_.margin_bottom);
    EXPECT_GT(height_before, 0.0f);

    // ダーティなものがない場合のProcessDirtyBatchはtotal_heightを破損させないべき
    bool more = engine_.ProcessDirtyBatch(nodes, cache, 800.0f, kNoLayoutDeadline, YRange::All());
    EXPECT_FALSE(more);
    EXPECT_FLOAT_EQ(ComputeTotalContentHeight(cache, theme_.margin_bottom), height_before);
}

// ---- 幅の変更 ----

TEST_F(MockLayoutTest, WidthChangeRecalculates)
{
    auto [nodes, cache] = ParseAndLayout("Some text that could wrap when narrower");
    float h_wide = cache[0].height;
    ComputeLayout(engine_, nodes, cache, 200.0f);
    float h_narrow = cache[0].height;
    EXPECT_GE(h_narrow, h_wide);
}

// ---- テーブルモック ----

TEST_F(MockLayoutTest, TableHasPositiveHeight)
{
    auto [nodes, cache] = ParseAndLayout("| A | B |\n|---|---|\n| 1 | 2 |");
    const int idx = FindFirstNodeIndexByType(nodes, NodeType::Table);
    ASSERT_GE(idx, 0);
    EXPECT_GT(cache[idx].height, 0.0f);
}

// ---- 水平線モック ----

TEST_F(MockLayoutTest, HorizontalRuleHasHeight)
{
    auto [nodes, cache] = ParseAndLayout("Above\n\n---\n\nBelow");
    const int idx = FindFirstNodeIndexByType(nodes, NodeType::HorizontalRule);
    ASSERT_GE(idx, 0);
    EXPECT_GT(cache[idx].height, 0.0f);
}

// ---- EnsureVisibleLayout ----

TEST_F(MockLayoutTest, EnsureVisibleLayoutUpdatesViewport)
{
    // ビューポート[0,50)の部分レイアウト
    auto [nodes, cache] = ParseAndLayout(MakeParagraphs(20), 800.0f, 0.0f, 50.0f);
    EXPECT_TRUE(cache.HasDirtyNodes());

    const auto dirty_before = CountDirty(cache);

    // より後の領域のレイアウトを確保
    float total = ComputeTotalContentHeight(cache, theme_.margin_bottom);
    EXPECT_TRUE(engine_.EnsureVisibleLayout(nodes, cache, 800.0f, total * 0.5f, total * 0.7f));
    EXPECT_LT(CountDirty(cache), dirty_before) << "可視範囲のダーティノードがクリーンになるべき";
}

// ---- 範囲省略時の全件レイアウト ----

TEST_F(MockLayoutTest, FullLayoutMeasuresAllNodes)
{
    auto [nodes, cache] = ParseAndLayout("A\n\nB", ViewportWidthFor(800.0f));
    EXPECT_GT(ComputeTotalContentHeight(cache, theme_.margin_bottom), 0.0f);
    EXPECT_FALSE(cache.HasDirtyNodes());
}

// ---- Mermaidブロックのズーム耐性 ----

// Mermaidブロックの初回レイアウトでプレースホルダー高さが設定されること
TEST_F(MockLayoutTest, MermaidBlockGetsPlaceholderHeight)
{
    auto [nodes, cache] = ParseAndLayout("```mermaid\ngraph TD;\n  A-->B;\n```");

    for (size_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].code_language() == SyntaxLanguage::Mermaid) {
            EXPECT_GT(cache[i].height, 0.0f) << "Mermaidブロックのプレースホルダー高さは正であるべき";
            EXPECT_FALSE(cache[i].layout_dirty);
        }
    }
}

// ビットマップレンダリング後の高さがレイアウト再計算で保持されること（ズーム操作を模擬）
TEST_F(MockLayoutTest, MermaidHeightPreservedAcrossLayoutCycles)
{
    auto [nodes, cache] = ParseAndLayout("Text\n\n```mermaid\ngraph TD;\n  A-->B;\n```\n\nMore text", ViewportWidthFor(800.0f));

    // ビットマップレンダリング完了後の高さを模擬（300 DIP）
    for (size_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].code_language() == SyntaxLanguage::Mermaid) {
            cache[i].height = 300.0f; // ビットマップ描画サイズ
            cache.EnsureDiagram(i).width = 400.0f;
            cache.EnsureDiagram(i).height = 300.0f;
        }
    }

    // ズーム変更を模擬: InvalidateAllLayouts → 全件レイアウト
    cache.InvalidateAllLayouts(nodes);
    LayoutAll(nodes, cache, 600.0f);

    for (size_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].code_language() == SyntaxLanguage::Mermaid) {
            EXPECT_FLOAT_EQ(cache[i].height, 300.0f)
                << "Mermaidブロックの高さはレイアウト再計算後も保持されるべき";
            // ダイアグラムエントリも保持
            EXPECT_FLOAT_EQ(cache.FindDiagram(i)->width, 400.0f);
            EXPECT_FLOAT_EQ(cache.FindDiagram(i)->height, 300.0f);
        }
    }
}

// content_widthが0以下のときもMermaidブロックの高さが保持されること
// （500%ズームでMDペインが極小になる場合を模擬）
TEST_F(MockLayoutTest, MermaidHeightPreservedAtZeroWidth)
{
    auto [nodes, cache] = ParseAndLayout("```mermaid\ngraph TD;\n  A-->B;\n```", ViewportWidthFor(800.0f));

    // ビットマップ描画後の高さを設定
    for (size_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].code_language() == SyntaxLanguage::Mermaid) {
            cache[i].height = 250.0f;
        }
    }

    // 極端なズームを模擬: 幅0でレイアウト
    cache.InvalidateAllLayouts(nodes);
    LayoutAll(nodes, cache, 0.0f);

    for (size_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].code_language() == SyntaxLanguage::Mermaid) {
            EXPECT_FLOAT_EQ(cache[i].height, 250.0f)
                << "幅0でのレイアウトでもMermaidブロックの高さは保持されるべき";
        }
    }

    // 元の幅に復帰
    cache.InvalidateAllLayouts(nodes);
    LayoutAll(nodes, cache, 800.0f);

    for (size_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].code_language() == SyntaxLanguage::Mermaid) {
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
    std::string md = "| col1 | col2 |\n|------|------|\n";
    for (int i = 0; i < 100; i++) {
        md += "| row" + std::to_string(i) + " | val" + std::to_string(i) + " |\n";
    }
    md += "\nFollow-up paragraph";

    // 1) フルレイアウトでテーブルの実測高さを取得
    auto [nodes, cache] = ParseAndLayout(md, ViewportWidthFor(800.0f));
    ASSERT_GE(nodes.size(), 2u);
    const int table_idx = FindFirstNodeIndexByType(nodes, NodeType::Table);
    ASSERT_GE(table_idx, 0);
    const float measured_table_h = cache[table_idx].height;
    EXPECT_GT(measured_table_h, 0.0f);

    // テーブル直後の段落
    const size_t para_idx = static_cast<size_t>(table_idx) + 1;
    ASSERT_LT(para_idx, nodes.size());

    // 2) 幅変更 + partial モード（viewport は冒頭の小さい領域のみ）
    //    テーブルは viewport 外なので不可視扱いになり、現行バグでは推定値で
    //    上書きされて高さが縮む。
    ComputeLayout(engine_, nodes, cache, 600.0f, 0.0f, 10.0f);

    // テーブルの高さが実測値から縮んでいないこと。
    // 修正後の partial モードは shrink を起こさないため厳密一致を期待する。
    EXPECT_GE(cache[table_idx].height, measured_table_h)
        << "partial モードで実測済みテーブルの高さが推定値で縮められた";

    // テーブル直後の段落の y_position もテーブル下端より下にあること
    const float table_bottom = cache.Top(table_idx) + cache[table_idx].height;
    EXPECT_GE(cache.Top(para_idx), table_bottom)
        << "テーブル直後ノードの y_position がテーブル下端より上に詰まっている (issue #158)";
}

// 推定値より小さい既存高さは、推定値まで成長させてよい（max_scroll の精度のため）
TEST_F(MockLayoutTest, PartialModeGrowsHeightWhenEstimateLarger)
{
    // 高さ 0 で partial モード（width_changed=true）。
    // 不可視ノードでも entry.height が 0 なら推定値まで成長すべき。
    auto [nodes, cache] = ParseAndLayout("First\n\nSecond\n\nThird", 800.0f, 0.0f, 1.0f);
    for (size_t i = 0; i < nodes.size(); i++) {
        EXPECT_GT(cache[i].height, 0.0f)
            << "ノード " << i << " の高さが推定値まで成長していない";
    }
}

// partial + invisible で entry.height が推定値に成長した場合、Table の幾何を
// 無効にし、row_heights 合計と乖離した entry.height のまま描画されないようにする。
// (issue #158: 幾何を残すと描画範囲が後続ノード位置を越えて重なる)
TEST_F(MockLayoutTest, PartialModeInvalidatesTableGeometryWhenHeightGrows)
{
    std::string md = "| a | b |\n|---|---|\n";
    for (int i = 0; i < 50; i++) {
        md += "| r" + std::to_string(i) + " | v" + std::to_string(i) + " |\n";
    }
    auto [nodes, cache] = ParseAndLayout(md, ViewportWidthFor(800.0f));
    const int table_idx = FindFirstNodeIndexByType(nodes, NodeType::Table);
    ASSERT_GE(table_idx, 0);
    ASSERT_NE(cache[table_idx].table_layout(), nullptr);
    ASSERT_TRUE(cache[table_idx].table_layout()->HasGeometry());

    // 成長分岐を強制発動させるため、現在の entry.height を意図的に小さくする
    cache[table_idx].height = 1.0f;

    // partial + invisible (テーブルは viewport より下) で再レイアウト。viewport は先頭余白内に収める。
    ComputeLayout(engine_, nodes, cache, 600.0f, 0.0f, theme_.margin_top * 0.5f);

    EXPECT_FALSE(cache[table_idx].table_layout()->HasGeometry())
        << "成長時に幾何が無効にならず、描画範囲と entry.height が乖離する";
}

// ---- RecreateFormats ----

// ズームで書式を作り直したら、幅が同じでも全ノードを再計測させる。
TEST_F(MockLayoutTest, InvalidateAllLayoutsForcesRemeasureAtSameWidth)
{
    auto [nodes, cache] = ParseAndLayout("Hello");
    const float h = cache[0].height;

    mock_.line_height *= 2.0f;
    ComputeLayout(engine_, nodes, cache, 800.0f);
    ASSERT_FLOAT_EQ(cache[0].height, h) << "幅不変かつ dirty なしなら再計測しない前提";

    EXPECT_TRUE(mock_.RecreateFormats());
    cache.InvalidateAllLayouts(nodes);
    EXPECT_TRUE(cache.HasDirtyNodes());
    ComputeLayout(engine_, nodes, cache, 800.0f);
    EXPECT_FLOAT_EQ(cache[0].height, h * 2.0f);
}

// 組み直すかは本文幅で決める。本文幅が下限に張り付いている間はビューポート幅が変わっても組み直さない。
TEST_F(MockLayoutTest, ReestimateFollowsContentWidth)
{
    auto [nodes, cache] = ParseAndLayout("Hello");
    const float clamped_narrow = MIN_NODE_CONTENT_WIDTH + 1.0f;
    const float clamped_wide = ViewportWidthFor(MIN_NODE_CONTENT_WIDTH) - 1.0f;
    ASSERT_FLOAT_EQ(theme_.ContentWidth(clamped_narrow), theme_.ContentWidth(clamped_wide));

    ComputeLayout(engine_, nodes, cache, clamped_narrow);
    EXPECT_FALSE(engine_.Reestimate(nodes, cache, clamped_wide));
    EXPECT_FALSE(cache[0].layout_dirty);

    EXPECT_TRUE(engine_.Reestimate(nodes, cache, ViewportWidthFor(MIN_NODE_CONTENT_WIDTH + LayoutEngine::kWidthChangeThreshold * 2.0f)));
    EXPECT_TRUE(cache[0].layout_dirty);
}

// ---- 多数ノードの合計高さ ----

TEST_F(MockLayoutTest, ManyNodesProduceLargeHeight)
{
    auto [nodes, cache] = ParseAndLayout(MakeParagraphs(100));
    const float total = ComputeTotalContentHeight(cache, theme_.margin_bottom);
    EXPECT_GT(total, 500.0f);
    size_t last = nodes.size() - 1;
    EXPECT_LE(cache.Top(last) + cache[last].height, total);
}

// ---- ファイル切り替えリグレッションテスト ----

TEST_F(MockLayoutTest, FileSwitchWithFreshCacheProducesCorrectLayout)
{
    // ファイルAを開くシミュレーション: "# Big Heading\n\nSome paragraph"
    auto nodes_a = ParseNodes("# Big Heading\n\nSome paragraph");
    LayoutCache cache(nodes_a.size());
    LayoutAll(nodes_a, cache, 800.0f);

    ASSERT_EQ(nodes_a.size(), 2u);
    float heading_height_a = cache[0].height;
    float para_height_a = cache[1].height;
    EXPECT_GT(heading_height_a, 0.0f);
    EXPECT_GT(para_height_a, 0.0f);

    // ファイルBへ切り替えシミュレーション: "Just a paragraph\n\nAnother one\n\nThird"
    auto nodes_b = ParseNodes("Just a paragraph\n\nAnother one\n\nThird");
    cache = LayoutCache(nodes_b.size());
    LayoutAll(nodes_b, cache, 800.0f);

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

TEST_F(MockLayoutTest, FileSwitchSameNodeCountWithFreshCacheRecalculates)
{
    // ファイルA: 見出し2つ
    auto nodes_a = ParseNodes("# H1\n\n## H2");
    LayoutCache cache(nodes_a.size());
    LayoutAll(nodes_a, cache, 800.0f);
    float h1_height = cache[0].height;
    float h2_height = cache[1].height;

    // ファイルB: 段落2つ（ファイルAと同じノード数）
    auto nodes_b = ParseNodes("alpha\n\nbeta");
    cache = LayoutCache(nodes_b.size());
    LayoutAll(nodes_b, cache, 800.0f);

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

// ---- 文書差し替え直後 (空のキャッシュ) の各レイアウト入口 ----
// アプリの文書切り替えは同じ幅のまま可視範囲だけ部分レイアウトする。

namespace {

constexpr float kSwitchWidth = 800.0f;
constexpr float kSwitchViewportBottom = 600.0f;

void LayoutPreviousDocument(LayoutEngine& engine, LayoutCache& cache)
{
    auto prev = ParseNodes("# Prev\n\nbody");
    cache = LayoutCache(prev.size());
    ComputeLayout(engine, prev, cache, kSwitchWidth, 0.0f, kSwitchViewportBottom);
}

} // namespace

// 未推定のキャッシュでも可視ノードが計測されること (ヘルプ表示で本文が空になっていた回帰の安全網)。
TEST_F(MockLayoutTest, FileSwitchPartialLayoutAtSameWidthMeasuresVisibleNodes)
{
    LayoutCache cache;
    LayoutPreviousDocument(engine_, cache);

    const auto md = MakeParagraphs(5);
    auto nodes = ParseNodes(md);
    cache = LayoutCache(nodes.size());
    ComputeLayout(engine_, nodes, cache, kSwitchWidth, 0.0f, kSwitchViewportBottom);

    ASSERT_EQ(nodes.size(), 5u);
    for (size_t i = 0; i < nodes.size(); i++) {
        EXPECT_FALSE(cache[i].layout_dirty) << "ノード " << i;
        EXPECT_GT(cache[i].height, 0.0f) << "ノード " << i;
    }
}

// 遅延レイアウトはビューポート付近しか計測しないため、推定なしでは遠方のノードが高さ 0 のまま残り
// スクロール範囲が極端に短くなる。ロード時の推定で最初から全体の高さが概ね正しいこと。
TEST_F(MockLayoutTest, EstimateOnSwitchGivesNearFullContentHeight)
{
    LayoutCache cache;
    LayoutPreviousDocument(engine_, cache);

    const auto md = MakeParagraphs(2000);
    auto nodes = ParseNodes(md);
    cache = layout::MakeEstimatedLayoutCache(nodes, theme_);
    ComputeLayout(engine_, nodes, cache, kSwitchWidth, 0.0f, kSwitchViewportBottom);
    while (engine_.ProcessDirtyBatch(nodes, cache, kSwitchWidth, kNoLayoutDeadline, layout::BufferedRange(0.0f, kSwitchViewportBottom, 5.0f))) {
    }

    LayoutCache full(nodes.size());
    LayoutAll(nodes, full, kSwitchWidth);
    const float expected = ComputeTotalContentHeight(full, theme_.margin_bottom);
    const float actual = ComputeTotalContentHeight(cache, theme_.margin_bottom);
    EXPECT_GT(actual, expected * 0.8f);
    EXPECT_LT(actual, expected * 1.25f);
}

// 遅延レイアウトタイマー (OnDeferredLayout) はビューポート制限付きで ProcessDirtyBatch を呼ぶ。
TEST_F(MockLayoutTest, ProcessDirtyBatchWithClipOnFreshCacheMeasuresTopNodes)
{
    LayoutCache cache;
    LayoutPreviousDocument(engine_, cache);

    const auto md = MakeParagraphs(10);
    auto nodes = ParseNodes(md);
    cache = LayoutCache(nodes.size());
    engine_.ProcessDirtyBatch(nodes, cache, kSwitchWidth, kNoLayoutDeadline, layout::BufferedRange(0.0f, kSwitchViewportBottom, 1.0f));

    EXPECT_FALSE(cache[0].layout_dirty);
    EXPECT_GT(cache[0].height, 0.0f);
    EXPECT_GE(cache.Top(0), theme_.margin_top);
}

// OnPaint は毎フレーム EnsureVisibleLayout を呼ぶ。ViewportLayout 後なら可視ノードは計測済みで、
// 後続フレームで高さが変わらない (無駄な再計測をしない) こと。
TEST_F(MockLayoutTest, EnsureVisibleLayoutAfterSwitchKeepsMeasuredNodes)
{
    LayoutCache cache;
    LayoutPreviousDocument(engine_, cache);

    const auto md = MakeParagraphs(10);
    auto nodes = ParseNodes(md);
    cache = LayoutCache(nodes.size());
    ComputeLayout(engine_, nodes, cache, kSwitchWidth, 0.0f, kSwitchViewportBottom);
    const float top0 = cache.Top(0);
    const float height0 = cache[0].height;

    EXPECT_FALSE(engine_.EnsureVisibleLayout(nodes, cache, kSwitchWidth, 0.0f, kSwitchViewportBottom));
    EXPECT_FLOAT_EQ(cache.Top(0), top0);
    EXPECT_FLOAT_EQ(cache[0].height, height0);
}

// 戻る/進むでのスクロール復元: 空のキャッシュ → 推定高さ → 復元位置を可視範囲として部分レイアウト。
// 推定で位置が決まっているので、復元先の可視ノードが計測されること。
TEST_F(MockLayoutTest, RestoreAfterSwitchMeasuresNodesAtRestoredPosition)
{
    LayoutCache cache;
    LayoutPreviousDocument(engine_, cache);

    const auto md = MakeParagraphs(200);
    auto nodes = ParseNodes(md);
    cache = layout::MakeEstimatedLayoutCache(nodes, theme_);

    constexpr size_t kTarget = 150;
    const float scroll_y = cache.Top(kTarget);
    ComputeLayout(engine_, nodes, cache, kSwitchWidth, scroll_y, scroll_y + kSwitchViewportBottom);

    const auto [first, last_plus_1] = ComputeVisibleNodeRange(cache, scroll_y, scroll_y + kSwitchViewportBottom);
    ASSERT_LT(first, last_plus_1);
    for (size_t i = first; i < last_plus_1; i++) {
        EXPECT_FALSE(cache[i].layout_dirty) << "ノード " << i;
    }
    EXPECT_TRUE(cache.HasDirtyNodes()) << "可視範囲外の残りは遅延レイアウトに回る";
}
