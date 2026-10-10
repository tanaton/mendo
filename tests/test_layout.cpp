#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <memory_resource>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include "command_generator.h"
#include "document_test_helpers.h"
#include "dwrite_test_base.h"
#include "parser.h"
#include "test_helpers.h"

class LayoutTest : public DWriteTestBase {};

namespace {

// RecomputeYPositions / EstimateNodeHeights に渡す手組みノードの仕様。
struct NodeSpec {
    NodeType type = NodeType::Paragraph;
    std::string_view text;
    int8_t heading_level = 0;
    uint32_t table_rows = 0;
};

std::pmr::vector<Node> MakeNodes(std::initializer_list<NodeSpec> specs)
{
    std::pmr::vector<Node> nodes;
    for (const auto& spec : specs) {
        Node& n = nodes.emplace_back();
        n.type = spec.type;
        if (!spec.text.empty()) {
            SetNodeTextCounted(n, spec.text);
        }
        if (spec.heading_level > 0) {
            n.set_heading_level(spec.heading_level);
        }
        if (spec.table_rows > 0) {
            n.ensure_table();
            n.table_data()->row_count = spec.table_rows;
        }
    }
    return nodes;
}

// 計測済み (layout_dirty=false) の高さだけを持つ cache。RecomputeYPositions の入力用。
LayoutCache MakeMeasuredCache(std::initializer_list<float> heights)
{
    LayoutCache cache;
    cache.Resize(heights.size());
    size_t i = 0;
    for (const float h : heights) {
        cache[i].height = h;
        cache[i].layout_dirty = false;
        ++i;
    }
    return cache;
}

LayoutCache EstimateHeights(const std::pmr::vector<Node>& nodes, const Theme& theme)
{
    LayoutCache cache;
    cache.Resize(nodes.size());
    EstimateNodeHeights(nodes, cache, theme);
    return cache;
}

float GapAfter(const LayoutCache& cache, size_t i)
{
    return cache.Top(i + 1) - (cache.Top(i) + cache[i].height);
}

} // namespace

TEST_F(LayoutTest, EmptyNodesProduceZeroHeight)
{
    std::pmr::vector<Node> nodes;
    LayoutCache cache;
    engine_.ComputeLayout(nodes, cache, 800.0f);
    EXPECT_FLOAT_EQ(ComputeTotalContentHeight(cache, nodes.size(), theme_.margin_bottom), 0.0f);
}

TEST_F(LayoutTest, SingleParagraphHasPositiveHeight)
{
    auto [nodes, cache] = ParseAndLayout("Hello world");
    EXPECT_GT(ComputeTotalContentHeight(cache, nodes.size(), theme_.margin_bottom), 0.0f);
    EXPECT_GT(cache[0].height, 0.0f);
}

TEST_F(LayoutTest, HeadingIsTallerThanParagraph)
{
    const auto heading = ParseAndLayout("# Big Title");
    const auto para = ParseAndLayout("Small text");
    EXPECT_GT(heading.cache[0].height, para.cache[0].height);
}

TEST_F(LayoutTest, YPositionsIncreaseMonotonically)
{
    auto [nodes, cache] = ParseAndLayout("# A\n\nB\n\nC\n\nD");

    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(cache.Top(i), cache.Top(i - 1))
            << "ノード " << i << " のyはノード " << (i - 1) << " より大きいこと";
    }
}

TEST_F(LayoutTest, NodesDoNotOverlap)
{
    auto [nodes, cache] = ParseAndLayout("# Heading\n\nParagraph\n\n---\n\n- List");

    for (size_t i = 1; i < nodes.size(); i++) {
        float prev_bottom = cache.Top(i - 1) + cache[i - 1].height;
        EXPECT_LE(prev_bottom, cache.Top(i))
            << "ノード " << (i - 1) << " がノード " << i << " と重なっている";
    }
}

TEST_F(LayoutTest, NarrowViewportWrapsText)
{
    constexpr std::string_view md = "This is a somewhat long paragraph that should wrap.";
    const auto wide = ParseAndLayout(md, 800.0f);
    const auto narrow = ParseAndLayout(md, 200.0f);

    // ビューポートが狭いほどテキストが高くなる（折り返しが増える）
    EXPECT_GE(narrow.cache[0].height, wide.cache[0].height);
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
    auto [nodes, cache] = ParseAndLayout("Test paragraph");
    EXPECT_NE(cache[0].text_layout.Get(), nullptr);
}

TEST_F(LayoutTest, HorizontalRuleHasNoTextLayout)
{
    auto [nodes, cache] = ParseAndLayout("---");
    EXPECT_EQ(cache[0].text_layout.Get(), nullptr);
    EXPECT_GT(cache[0].height, 0.0f);
}

TEST_F(LayoutTest, CodeBlockTextLayout)
{
    auto [nodes, cache] = ParseAndLayout("```\ncode\n```");
    EXPECT_NE(cache[0].text_layout.Get(), nullptr);
}

TEST_F(LayoutTest, TableLayout)
{
    auto [nodes, cache] = ParseAndLayout(
        "| A | B |\n"
        "|---|---|\n"
        "| 1 | 2 |");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Table);
    EXPECT_GT(cache[0].height, 0.0f);
    ASSERT_TRUE(cache[0].has_table_layout());
    EXPECT_FALSE(cache[0].table_layout->col_widths.empty());
}

TEST_F(LayoutTest, TableCellLayoutsCreated)
{
    auto [nodes, cache] = ParseAndLayout(
        "| A | B |\n"
        "|---|---|\n"
        "| 1 | 2 |");
    ASSERT_TRUE(cache[0].has_table_layout());
    const auto& tl = *cache[0].table_layout;
    const auto* tbl = nodes[0].table_data();
    for (size_t r = 0; r < tbl->row_count; r++) {
        for (size_t c = 0; c < tbl->col_count; c++) {
            if (!tbl->GetCellText(r, c).empty()) {
                EXPECT_NE(tl.GetCellLayout(r, c), nullptr);
            }
        }
    }
}

TEST_F(LayoutTest, TableCellLinkHasUnderline)
{
    auto [nodes, cache] = ParseAndLayout(
        "| Text | Link |\n"
        "|------|------|\n"
        "| hello | [click](https://example.com) |");
    ASSERT_EQ(nodes.size(), 1u);
    const auto* tbl = nodes[0].table_data();
    ASSERT_GE(tbl->row_count, 2u);

    // データ行の2番目のセルにはリンクランがあり、下線が適用されていること
    ASSERT_TRUE(cache[0].has_table_layout());
    IDWriteTextLayout* cell_layout = cache[0].table_layout->GetCellLayout(1, 1);
    ASSERT_NE(cell_layout, nullptr);

    // セル内にリンクランが存在することを確認
    bool has_link_run = false;
    for (const auto& run : tbl->GetCellRuns(1, 1)) {
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
    auto [nodes, cache] = ParseAndLayout("# H1\n\n## H2\n\n### H3");
    ASSERT_EQ(nodes.size(), 3u);
    // H1はH2より高く、H2はH3以上であること
    EXPECT_GT(cache[0].height, cache[1].height);
    EXPECT_GE(cache[1].height, cache[2].height);
}

TEST_F(LayoutTest, TotalHeightWithManyNodes)
{
    auto [nodes, cache] = ParseAndLayout(MakeParagraphs(100));

    float total = ComputeTotalContentHeight(cache, nodes.size(), theme_.margin_bottom);
    EXPECT_GT(total, 1000.0f); // 100段落あればかなり高くなるはず

    // 最後のノードの下端が全体の高さ以内であること
    size_t last = nodes.size() - 1;
    EXPECT_LE(cache.Top(last) + cache[last].height, total);
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
        EXPECT_GT(cache.Top(i), cache.Top(i - 1));
    }
}

TEST_F(LayoutTest, ProcessDirtyBatchSmallBatch)
{
    // 多数の段落を作成
    const auto md = MakeParagraphs(50);
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
    auto [nodes, cache] = ParseAndLayout("This is a paragraph with some text that might wrap differently.");
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
    // 0 行 0 列の空テーブル (新方式は row_count/col_count 既定で 0)
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
    const auto plain = ParseAndLayout("This is a somewhat long paragraph that wraps.", 400.0f);
    const auto list = ParseAndLayout("- This is a somewhat long paragraph that wraps.", 400.0f);

    // リスト項目はインデントされるため、同じテキストでも高くなる（利用可能な幅が狭い）
    EXPECT_GE(list.cache[0].height, plain.cache[0].height);
}

// ---- ブロック引用のレイアウト ----

TEST_F(LayoutTest, BlockQuoteLayout)
{
    auto [nodes, cache] = ParseAndLayout("> Quoted text here");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_GT(cache[0].height, 0.0f);
    EXPECT_GT(nodes[0].indent_level, 0);
}

// ---- コードブロックの折り返し無効 ----

TEST_F(LayoutTest, CodeBlockDoesNotWrap)
{
    std::string long_line = "```\n";
    for (int i = 0; i < 50; i++) {
        long_line += "long_word ";
    }
    long_line += "\n```";

    auto [nodes, cache] = ParseAndLayout(long_line, 200.0f); // Very narrow

    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    // コードブロックは折り返さないため、高さは1行分になるはず
    // （おおよそコードフォントの高さ）
    EXPECT_LT(cache[0].height, 100.0f);
}

// ---- 見出しの間隔 ----

TEST_F(LayoutTest, HeadingHasSpacingAboveAndBelow)
{
    auto [nodes, cache] = ParseAndLayout("Paragraph\n\n# Heading\n\nAnother paragraph");
    ASSERT_EQ(nodes.size(), 3u);

    // 見出しの上に間隔があること（段落の下端と見出しのyの間隔）
    float para_bottom = cache.Top(0) + cache[0].height;
    float heading_y = cache.Top(1);
    float gap_above = heading_y - para_bottom;
    EXPECT_GT(gap_above, theme_.paragraph_spacing);

    // 見出しの下に間隔があること
    float heading_bottom = cache.Top(1) + cache[1].height;
    float next_y = cache.Top(2);
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
    std::pmr::vector<float> widths;
    ComputeColumnWidths(widths, natural, 150.0f, 3);
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
    std::pmr::vector<float> widths;
    ComputeColumnWidths(widths, natural, 300.0f, 3);
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
    std::pmr::vector<float> widths;
    ComputeColumnWidths(widths, natural, 40.0f, 2);
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
    std::pmr::vector<float> widths;
    ComputeColumnWidths(widths, natural, 200.0f, 2);
    ASSERT_EQ(widths.size(), 2u);
    // 列Aは列Bよりも大きな割合を得ること
    EXPECT_GT(widths[0], widths[1]);
}

TEST(ComputeColumnWidthsTest, SingleColumn)
{
    std::pmr::vector<float> natural = { 50.0f };
    std::pmr::vector<float> widths;
    ComputeColumnWidths(widths, natural, 200.0f, 1);
    ASSERT_EQ(widths.size(), 1u);
    EXPECT_GE(widths[0], 50.0f);
}

TEST(ComputeColumnWidthsTest, ZeroNaturalWidths)
{
    std::pmr::vector<float> natural = { 0.0f, 0.0f };
    std::pmr::vector<float> widths;
    ComputeColumnWidths(widths, natural, 200.0f, 2);
    ASSERT_EQ(widths.size(), 2u);
    // それでも有効な幅を生成すること
    for (auto w : widths) {
        EXPECT_GT(w, 0.0f);
    }
}

// 合計が使える幅を超えてよいのは「自然幅のまま横スクロールに任せる」分岐だけ。
// それ以外で超えると natural_total_width 基準の横スクロールも出ず、表がペイン外へはみ出す。
// 自然幅が収まる場合は、どの列も自然幅を割らず、合計は使える幅ちょうどになること。
TEST(ComputeColumnWidthsTest, TotalFitsAvailableUnlessLeftAtNaturalForScroll)
{
    constexpr float kMinColumnWidth = 30.0f;
    constexpr float kEps = 0.05f;
    for (const uint32_t seed : { 1u, 2u, 3u, 4u, 5u }) {
        std::mt19937 rng(seed);
        std::uniform_int_distribution<size_t> col_dist(1, 8);
        std::uniform_real_distribution<float> narrow_dist(0.0f, 60.0f);
        std::uniform_real_distribution<float> wide_dist(0.0f, 1000.0f);
        std::uniform_real_distribution<float> avail_dist(0.0f, 2000.0f);
        std::bernoulli_distribution pick_narrow(0.5);
        for (int step = 0; step < 2000; ++step) {
            const size_t n = col_dist(rng);
            std::pmr::vector<float> natural(n);
            for (auto& w : natural) {
                w = pick_narrow(rng) ? narrow_dist(rng) : wide_dist(rng);
            }
            const float available = avail_dist(rng);
            std::pmr::vector<float> out;
            ComputeColumnWidths(out, natural, available, n);

            // 失敗時だけ評価されるメッセージで使い、成功ケースで文字列を組み立てない。
            const auto input = [&] {
                std::string s = "seed=" + std::to_string(seed) + " step=" + std::to_string(step) + " available=" + std::to_string(available) + " natural=";
                for (const float w : natural) {
                    s += std::to_string(w) + ",";
                }
                return s;
            };
            ASSERT_EQ(out.size(), n) << input();

            const float effective = std::max(available, static_cast<float>(n) * kMinColumnWidth);
            const float natural_total = std::reduce(natural.begin(), natural.end(), 0.0f);
            const float out_total = std::reduce(out.begin(), out.end(), 0.0f);
            if (natural_total > effective && std::ranges::equal(out, natural)) {
                continue;
            }
            ASSERT_LE(out_total, effective + kEps) << input();
            if (natural_total <= effective) {
                ASSERT_NEAR(out_total, effective, kEps) << input();
                for (size_t c = 0; c < n; ++c) {
                    ASSERT_GE(out[c], natural[c] - kEps) << "c=" << c << " " << input();
                }
            }
        }
    }
}

// DWrite 実測でも、表の描画幅はノード幅か横スクロール上限 (natural_total_width) のいずれかに収まる。
TEST_F(LayoutTest, TableWidthFitsNodeOrScrollableNaturalWidth)
{
    // 短い列と長い列 (均等幅を超えるが合計は収まる) の組み合わせ。
    const std::string md =
        "| a | " + std::string(40, 'w') + " |\n"
        "|---|---|\n"
        "| b | c |\n";
    for (const float viewport_w : { 500.0f, 600.0f, 700.0f, 800.0f, 1200.0f }) {
        SCOPED_TRACE("viewport_w=" + std::to_string(viewport_w));
        auto [nodes, cache] = ParseAndLayout(md, viewport_w);
        ASSERT_EQ(nodes.size(), 1u);
        ASSERT_TRUE(cache[0].has_table_layout());
        const auto& tl = *cache[0].table_layout;
        const float node_width = theme_.ContentWidth(viewport_w) - NodeIndent(nodes[0], theme_);
        EXPECT_LE(tl.cached_table_width, std::max(node_width, tl.natural_total_width) + 0.05f)
            << "natural_total_width=" << tl.natural_total_width << " node_width=" << node_width;
    }
}

// ---- RecomputeYPositions テスト ----

TEST(RecomputeYPositionsTest, EmptyNodes)
{
    std::pmr::vector<Node> nodes;
    LayoutCache cache;
    Theme theme = GetLightTheme();
    const bool has_dirty = RecomputeYPositions(nodes, cache, theme);
    EXPECT_FALSE(has_dirty);
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
    cache.SetTop(0, 15.0f);
    cache[0].height = 50.0f;
    EXPECT_FLOAT_EQ(ComputeTotalContentHeight(cache, 1, 15.0f), 80.0f);
}

TEST(ComputeTotalContentHeightTest, MultipleNodes)
{
    LayoutCache cache;
    cache.Resize(3);
    cache.SetTop(0, 10.0f);
    cache[0].height = 20.0f;
    cache.SetTop(1, 40.0f);
    cache[1].height = 30.0f;
    cache.SetTop(2, 80.0f);
    cache[2].height = 25.0f;
    // 最後のノードのみが関係: 80 + 25 + 10 = 115
    EXPECT_FLOAT_EQ(ComputeTotalContentHeight(cache, 3, 10.0f), 115.0f);
}

TEST(RecomputeYPositionsTest, SingleParagraph)
{
    auto nodes = MakeNodes({ { .type = NodeType::Paragraph } });
    auto cache = MakeMeasuredCache({ 20.0f });
    const Theme theme = GetLightTheme();

    const bool has_dirty = RecomputeYPositions(nodes, cache, theme);
    EXPECT_FLOAT_EQ(cache.Top(0), theme.margin_top);
    EXPECT_FALSE(has_dirty);
}

TEST(RecomputeYPositionsTest, HeadingSpacing)
{
    auto nodes = MakeNodes({
        { .type = NodeType::Paragraph },
        { .type = NodeType::Heading, .heading_level = 3 }, // h3はheading_spacing_below（下線なし）を使う
        { .type = NodeType::Paragraph },
    });
    auto cache = MakeMeasuredCache({ 20.0f, 30.0f, 20.0f });
    const Theme theme = GetLightTheme();

    RecomputeYPositions(nodes, cache, theme);

    // 見出しの上に追加の間隔があること
    float para_bottom = cache.Top(0) + cache[0].height + theme.paragraph_spacing;
    float heading_y = cache.Top(1);
    EXPECT_FLOAT_EQ(heading_y, para_bottom + theme.heading_spacing_above);

    // 見出しの後: paragraph_spacingではなくheading_spacing_below
    float heading_bottom = cache.Top(1) + cache[1].height + theme.heading_spacing_below;
    EXPECT_FLOAT_EQ(cache.Top(2), heading_bottom);
}

TEST(RecomputeYPositionsTest, H1H2UseLargerSpacingBelow)
{
    // h1/h2 は下線を描くため heading_spacing_below_h1h2 が使われ、
    // h3以降は heading_spacing_below が使われることを検証する。
    auto nodes = MakeNodes({
        { .type = NodeType::Heading, .heading_level = 1 },
        { .type = NodeType::Paragraph },
        { .type = NodeType::Heading, .heading_level = 3 },
        { .type = NodeType::Paragraph },
    });
    auto cache = MakeMeasuredCache({ 40.0f, 20.0f, 30.0f, 20.0f });
    const Theme theme = GetLightTheme();
    RecomputeYPositions(nodes, cache, theme);

    // h1 の後: heading_spacing_below_h1h2 が使われる
    EXPECT_FLOAT_EQ(GapAfter(cache, 0), theme.heading_spacing_below_h1h2);

    // h3 の後: heading_spacing_below（h3以降用）が使われる
    EXPECT_FLOAT_EQ(GapAfter(cache, 2), theme.heading_spacing_below);

    // 両者は実際に異なる値であること（テスト対象の分岐が意味を持つ前提）
    EXPECT_GT(theme.heading_spacing_below_h1h2, theme.heading_spacing_below);
}

TEST(RecomputeYPositionsTest, DetectsDirtyNodes)
{
    auto nodes = MakeNodes({ { .type = NodeType::Paragraph }, { .type = NodeType::Paragraph } });
    auto cache = MakeMeasuredCache({ 20.0f, 20.0f });
    cache[1].layout_dirty = true;
    const Theme theme = GetLightTheme();

    const bool has_dirty = RecomputeYPositions(nodes, cache, theme);
    EXPECT_TRUE(has_dirty);
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
        EXPECT_GT(cache.Top(i), cache.Top(i - 1));
    }
}

// ---- EnsureVisibleLayout テスト ----

TEST_F(LayoutTest, EnsureVisibleLayoutFixesDirtyVisibleNodes)
{
    // 複数の段落を作成し、ある幅でフルレイアウトを実行
    auto [nodes, cache] = ParseAndLayout(MakeParagraphs(20));

    // 別の幅で部分的なレイアウトを実行 — 画面外のノードがダーティにマークされる
    engine_.ComputeLayout(nodes, cache, 400.0f, 0.0f, 100.0f);

    // ビューポート外のノードはまだダーティであること
    ASSERT_GT(CountDirty(cache), 0u);

    // 修正パスをテストするため、表示中のノードを手動でダーティにマーク
    cache[0].layout_dirty = true;

    // EnsureVisibleLayoutが表示範囲を修正すること
    bool updated = engine_.EnsureVisibleLayout(nodes, cache, 400.0f, 0.0f, 100.0f);
    EXPECT_TRUE(updated);

    // 表示範囲内のノードはもうダーティでないこと
    for (size_t i = 0; i < nodes.size(); i++) {
        if (cache.Top(i) + cache[i].height < 0.0f) {
            continue;
        }
        if (cache.Top(i) > 100.0f) {
            break;
        }
        EXPECT_FALSE(cache[i].layout_dirty)
            << "y=" << cache.Top(i) << " の表示ノードがまだダーティ";
    }
}

TEST_F(LayoutTest, EnsureVisibleLayoutReturnsFalseWhenClean)
{
    auto [nodes, cache] = ParseAndLayout("Hello world");

    // すべてのノードがクリーンなので、EnsureVisibleLayoutはfalseを返すこと
    bool updated = engine_.EnsureVisibleLayout(nodes, cache, 800.0f, 0.0f, 1000.0f);
    EXPECT_FALSE(updated);
}

TEST_F(LayoutTest, EnsureVisibleLayoutSkipsOffscreenDirtyNodes)
{
    const auto md = MakeParagraphs(30);
    auto nodes = ParseMarkdown(md).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f, 0.0f, 50.0f);

    const auto dirty_before = CountDirty(cache);

    // 小さなビューポート範囲のみでEnsureVisibleLayoutを実行
    engine_.EnsureVisibleLayout(nodes, cache, 800.0f, 0.0f, 50.0f);

    // 一部のノード（画面外のもの）はまだダーティであること
    const auto dirty_after = CountDirty(cache);
    EXPECT_GT(dirty_after, 0u);
    EXPECT_LE(dirty_after, dirty_before);
}

TEST_F(LayoutTest, EnsureVisibleLayoutRecomputesYPositions)
{
    // 広い幅でフルレイアウト
    auto [nodes, cache] = ParseAndLayout(MakeParagraphs(10));

    // 狭い幅で部分的なレイアウトを実行（画面外をダーティにマーク）
    engine_.ComputeLayout(nodes, cache, 300.0f, 0.0f, 50.0f);

    // EnsureVisibleLayoutがY位置を一貫して更新すること
    engine_.EnsureVisibleLayout(nodes, cache, 300.0f, 0.0f, 50.0f);

    // Y位置が単調増加を維持していること
    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(cache.Top(i), cache.Top(i - 1))
            << "ノード " << i << " のyはノード " << (i - 1) << " より大きいこと";
    }
}

TEST_F(LayoutTest, EnsureVisibleLayoutUpdatesTotalHeight)
{
    const auto md = MakeParagraphs(10);
    auto nodes = ParseMarkdown(md).nodes;
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f, 0.0f, 50.0f);

    engine_.EnsureVisibleLayout(nodes, cache, 800.0f, 0.0f, 50.0f);

    // 表示ノードが再レイアウトされると全体の高さが変わる可能性があるが
    // 正の値を維持すること
    EXPECT_GT(ComputeTotalContentHeight(cache, nodes.size(), theme_.margin_bottom), 0.0f);
}

// 部分モードで不可視ノードの古い height を引きずらないこと（High-1 回帰）。
// ズーム/テーマ変更直後の Y 位置に stale な height が混入しないことを保証する。
TEST_F(LayoutTest, PartialLayoutRefreshesStaleInvisibleHeights)
{
    // フル幅でフルレイアウト → 全ノード正確な height を持つ
    auto [nodes, cache] = ParseAndLayout(MakeParagraphs(30));
    const float baseline_total = ComputeTotalContentHeight(cache, nodes.size(), theme_.margin_bottom);
    ASSERT_GT(baseline_total, 0.0f);

    // 不可視（後方）ノードを「旧テーマで非常に大きかった」状態にしてダーティ化する。
    // これは zoom-out 直後に invisible 領域だけ stale height が残るケースを模す。
    const size_t node_count = nodes.size();
    constexpr float STALE_HEIGHT = 9999.0f;
    for (size_t i = node_count / 2; i < node_count; i++) {
        cache[i].height = STALE_HEIGHT;
        cache[i].layout_dirty = true;
    }

    // 部分レイアウト：可視範囲は先頭わずかのみ。後方の invisible ダーティ群は
    // 現テーマでの推定値に置き換わるはずで、stale な巨大 height は混ざらない。
    engine_.ComputeLayout(nodes, cache, 800.0f, 0.0f, 50.0f);
    const float last_top_after = cache.Top(node_count - 1);

    // baseline と同程度（推定誤差ぶんはあり得る）に収束し、stale の合計を
    // 引きずった巨大値にはならないこと。
    EXPECT_LT(last_top_after, baseline_total * 2.0f)
        << "stale height (=" << STALE_HEIGHT << ") を Y 位置に取り込んでいる";
}

// ---- RecomputeYPositions 追加テスト ----

TEST(RecomputeYPositionsTest, MultipleHeadingsHaveCorrectSpacing)
{
    const Theme theme = GetLightTheme();
    auto nodes = MakeNodes({
        { .type = NodeType::Heading, .heading_level = 3 },
        { .type = NodeType::Heading, .heading_level = 3 },
    });
    auto cache = MakeMeasuredCache({ 40.0f, 30.0f });

    RecomputeYPositions(nodes, cache, theme);

    // 最初の見出し: margin_top + heading_spacing_above
    EXPECT_FLOAT_EQ(cache.Top(0), theme.margin_top + theme.heading_spacing_above);

    // 2番目の見出し: 最初の見出しの後 + heading_spacing_below + heading_spacing_above
    float expected_y = cache.Top(0) + cache[0].height + theme.heading_spacing_below + theme.heading_spacing_above;
    EXPECT_FLOAT_EQ(cache.Top(1), expected_y);
}

TEST(RecomputeYPositionsTest, AllNodeTypesProduceValidPositions)
{
    const Theme theme = GetLightTheme();
    auto nodes = MakeNodes({
        { .type = NodeType::Paragraph },
        { .type = NodeType::Heading },
        { .type = NodeType::CodeBlock },
        { .type = NodeType::HorizontalRule },
        { .type = NodeType::ListItem },
        { .type = NodeType::BlockQuote },
        { .type = NodeType::Table },
    });
    auto cache = MakeMeasuredCache({ 20.0f, 30.0f, 50.0f, 5.0f, 18.0f, 25.0f, 60.0f });

    RecomputeYPositions(nodes, cache, theme);

    // すべての位置が単調増加であること
    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(cache.Top(i), cache.Top(i - 1));
    }
}

// ---- FindFirstVisibleNodeIndex テスト（layout_cache.h のフリー関数） ----

TEST(FindFirstVisibleNodeIndex, AtStart)
{
    auto cache = MakeUniformCache(10, 50.0f);
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 10, 0.0f), 0);
}

TEST(FindFirstVisibleNodeIndex, MidDocument)
{
    auto cache = MakeUniformCache(10, 50.0f);
    // viewport_top = 120 → ノード2 (y=100, bottom=150) が最初の可視ノード
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 10, 120.0f), 2);
}

TEST(FindFirstVisibleNodeIndex, ExactBoundary)
{
    auto cache = MakeUniformCache(10, 50.0f);
    // viewport_top = 50 → ノード0はy=50で終了、ノード1はy=50で開始
    // ノード0の下端(50) == viewport_top(50)なので除外される
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 10, 50.0f), 1);
}

TEST(FindFirstVisibleNodeIndex, PastEnd)
{
    auto cache = MakeUniformCache(5, 50.0f);
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
    auto cache = MakeUniformCache(1, 100.0f);
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 1, 0.0f), 0);
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 1, 50.0f), 0);
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 1, 100.0f), 1); // ノードを過ぎた位置
}

TEST(FindFirstVisibleNodeIndex, LastNodeVisible)
{
    auto cache = MakeUniformCache(10, 50.0f);
    // viewport_top = 449 → ノード8は450で終了、まだ可視
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 10, 449.0f), 8);
}

// Reset 直後 (全ノードの位置・高さが 0) も該当なし扱い。
TEST(FindFirstVisibleNodeIndex, FreshCacheReturnsNodeCount)
{
    LayoutCache cache;
    cache.Reset(5);
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 5, 0.0f), 5);
}

TEST(FindFirstVisibleNodeIndex, ClampsNodeCountToCacheSize)
{
    auto cache = MakeUniformCache(3);
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 10, 1000.0f), 3);
}

// ---- 隣接 2 ノード間のスペーシング ----

TEST(RecomputeYPositionsTest, PairSpacingByNodeType)
{
    struct Case {
        const char* name;
        NodeSpec first;
        NodeSpec second;
        float first_height;
        float second_height;
        float (*expected_gap)(const Theme&);
    };
    const auto code_gap = [](const Theme& t) { return t.paragraph_spacing + t.code_block_spacing_above; };
    const auto list_gap = [](const Theme& t) { return t.list_item_spacing; };
    // 空テキスト LI は issue#237 の修正で sb=0 (loose 扱い)。tight LI 間隔の検証には HasText() が要る。
    const Case cases[] = {
        { "CodeBlockHasSpacingAbove", { .type = NodeType::Paragraph }, { .type = NodeType::CodeBlock }, 20.0f, 50.0f, code_gap },
        { "CodeBlockHasSpacingBelow", { .type = NodeType::CodeBlock }, { .type = NodeType::Paragraph }, 50.0f, 20.0f, code_gap },
        { "BlockQuoteHasSpacingAbove", { .type = NodeType::Paragraph }, { .type = NodeType::BlockQuote }, 20.0f, 30.0f, code_gap },
        { "ListItemUsesListItemSpacing", { NodeType::ListItem, "a" }, { NodeType::ListItem, "b" }, 18.0f, 18.0f, list_gap },
        { "TaskListItemUsesListItemSpacing", { NodeType::TaskListItem, "a" }, { NodeType::TaskListItem, "b" }, 18.0f, 18.0f, list_gap },
    };
    const Theme theme = GetLightTheme();
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        auto nodes = MakeNodes({ c.first, c.second });
        auto cache = MakeMeasuredCache({ c.first_height, c.second_height });
        RecomputeYPositions(nodes, cache, theme);
        EXPECT_FLOAT_EQ(GapAfter(cache, 0), c.expected_gap(theme));
    }
}

// ---- from_index による途中再開 ----

TEST(RecomputeYPositionsTest, FromIndexCodeBlock)
{
    const Theme theme = GetLightTheme();
    auto nodes = MakeNodes({ { .type = NodeType::CodeBlock }, { .type = NodeType::Paragraph } });
    auto cache = MakeMeasuredCache({ 50.0f, 20.0f });

    // まず全体を計算
    RecomputeYPositions(nodes, cache, theme);
    float expected_y1 = cache.Top(1);

    // from_index=1 で途中から再計算
    RecomputeYPositions(nodes, cache, theme, 1);
    EXPECT_FLOAT_EQ(cache.Top(1), expected_y1);
}

TEST(RecomputeYPositionsTest, FromIndexListItem)
{
    const Theme theme = GetLightTheme();
    auto nodes = MakeNodes({ { .type = NodeType::ListItem }, { .type = NodeType::Paragraph } });
    auto cache = MakeMeasuredCache({ 18.0f, 20.0f });

    RecomputeYPositions(nodes, cache, theme);
    float expected_y1 = cache.Top(1);

    RecomputeYPositions(nodes, cache, theme, 1);
    EXPECT_FLOAT_EQ(cache.Top(1), expected_y1);
}

// ---- リスト箇条書き記号の垂直位置（実DWriteレイアウト使用） ----

// ---- 見出し内インラインコードのフォントサイズ ----

TEST_F(LayoutTest, InlineCodeInHeadingAllLevels)
{
    for (int level = 1; level <= 6; ++level) {
        std::string md(level, '#');
        md += " Test `code`";

        auto [nodes, cache] = ParseAndLayout(md);

        ASSERT_EQ(nodes.size(), 1u);
        ASSERT_EQ(nodes[0].type, NodeType::Heading);
        ASSERT_NE(cache[0].text_layout.Get(), nullptr);

        bool found_code_run = false;
        for (const auto& run : nodes[0].runs) {
            if (run.code()) {
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
    auto [nodes, cache] = ParseAndLayout("Hello `code` world");

    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].type, NodeType::Paragraph);
    ASSERT_NE(cache[0].text_layout.Get(), nullptr);

    for (const auto& run : nodes[0].runs) {
        if (run.code()) {
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
    auto [nodes, cache] = ParseAndLayout("# Hello `code`");

    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_NE(cache[0].text_layout.Get(), nullptr);

    for (const auto& run : nodes[0].runs) {
        if (run.code()) {
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
    const Theme theme = GetLightTheme();
    const auto nodes = MakeNodes({ { .text = "Hello world" } });
    const auto cache = EstimateHeights(nodes, theme);

    EXPECT_GT(cache[0].height, 0.0f);
    EXPECT_GE(cache.Top(0), theme.margin_top);
}

TEST(EstimateNodeHeightsTest, YPositionsIncreaseMonotonically)
{
    const auto nodes = ParseMarkdown("# A\n\nB\n\nC\n\nD").nodes;
    const auto cache = EstimateHeights(nodes, GetLightTheme());

    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GT(cache.Top(i), cache.Top(i - 1))
            << "ノード " << i << " のy_positionがノード " << (i - 1) << " より大きいこと";
    }
}

TEST(EstimateNodeHeightsTest, NodesDoNotOverlap)
{
    const auto nodes = ParseMarkdown("# Heading\n\nParagraph\n\n---\n\n- List").nodes;
    const auto cache = EstimateHeights(nodes, GetLightTheme());

    for (size_t i = 1; i < nodes.size(); i++) {
        float prev_bottom = cache.Top(i - 1) + cache[i - 1].height;
        EXPECT_LE(prev_bottom, cache.Top(i))
            << "ノード " << (i - 1) << " がノード " << i << " と重なっている";
    }
}

TEST(EstimateNodeHeightsTest, HeadingHeightScalesWithLevel)
{
    // H1とH3を推定して、H1の方が高いことを確認
    const auto nodes = MakeNodes({
        { NodeType::Heading, "Title", 1 },
        { NodeType::Heading, "Title", 3 },
    });
    const auto cache = EstimateHeights(nodes, GetLightTheme());

    EXPECT_GT(cache[0].height, cache[1].height);
}

TEST(EstimateNodeHeightsTest, CodeBlockScalesWithLineCount)
{
    const auto nodes = MakeNodes({
        { NodeType::CodeBlock, "line1" },
        { NodeType::CodeBlock, "line1\nline2\nline3\nline4\nline5" },
    });
    const auto cache = EstimateHeights(nodes, GetLightTheme());

    EXPECT_GT(cache[1].height, cache[0].height);
}

TEST(EstimateNodeHeightsTest, HorizontalRuleHasFixedHeight)
{
    const Theme theme = GetLightTheme();
    const auto nodes = MakeNodes({ { .type = NodeType::HorizontalRule } });
    const auto cache = EstimateHeights(nodes, theme);

    EXPECT_FLOAT_EQ(cache[0].height, theme.paragraph_spacing + theme.hr_thickness);
}

TEST(EstimateNodeHeightsTest, ImageHasMinimumHeight)
{
    const auto nodes = MakeNodes({ { .type = NodeType::Image } });
    const auto cache = EstimateHeights(nodes, GetLightTheme());

    EXPECT_GE(cache[0].height, 60.0f);
}

TEST(EstimateNodeHeightsTest, TableScalesWithRowCount)
{
    const auto nodes = MakeNodes({
        { .type = NodeType::Table, .table_rows = 1 },
        { .type = NodeType::Table, .table_rows = 3 },
    });
    const auto cache = EstimateHeights(nodes, GetLightTheme());

    EXPECT_GT(cache[1].height, cache[0].height);
}

TEST(EstimateNodeHeightsTest, EmptyTextNodeUsesSpacing)
{
    const Theme theme = GetLightTheme();
    const auto nodes = MakeNodes({ { .type = NodeType::Paragraph } });
    const auto cache = EstimateHeights(nodes, theme);

    EXPECT_FLOAT_EQ(cache[0].height, theme.paragraph_spacing);
}

TEST(EstimateNodeHeightsTest, MultilineParagraphScalesWithLines)
{
    const auto nodes = MakeNodes({ { .text = "one line" }, { .text = "line1\nline2\nline3" } });
    const auto cache = EstimateHeights(nodes, GetLightTheme());

    EXPECT_GT(cache[1].height, cache[0].height);
}

TEST(EstimateNodeHeightsTest, LayoutDirtyNotChanged)
{
    const Theme theme = GetLightTheme();
    const auto nodes = MakeNodes({ { .text = "test" } });
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
    const auto nodes = MakeNodes({
        { NodeType::Paragraph, "content" },
        { NodeType::Heading, "content", 2 },
        { NodeType::CodeBlock, "content" },
        { NodeType::HorizontalRule },
        { NodeType::ListItem, "content" },
        { NodeType::BlockQuote, "content" },
        { NodeType::Table, "content", 0, 1 },
        { NodeType::TaskListItem, "content" },
        { NodeType::Image },
    });
    const auto cache = EstimateHeights(nodes, GetLightTheme());

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
    auto [nodes, cache] = ParseAndLayout("- Item text here");

    ASSERT_NE(cache[0].text_layout.Get(), nullptr);
    DWRITE_LINE_METRICS lm;
    UINT32 lc;
    ASSERT_TRUE(SUCCEEDED(cache[0].text_layout->GetLineMetrics(&lm, 1, &lc)));
    ASSERT_GT(lc, 0u);

    // bullet 中心は物理ピクセル境界へスナップされるため、期待値も同じ規則でスナップする。
    float expected_y = SnapToPhysicalPixel(cache.Top(0) + lm.height * 0.5f, 1.0f);

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

// issue#237: loose list の LI (空) と直下 Paragraph の text_top は一致すべき。
TEST_F(LayoutTest, LooseListBulletAlignsWithFollowingParagraphText)
{
    auto [nodes, cache] = ParseAndLayout("- a\n\n- b");

    ASSERT_EQ(nodes.size(), 4u);
    ASSERT_EQ(nodes[0].type, NodeType::ListItem);
    ASSERT_EQ(nodes[1].type, NodeType::Paragraph);
    ASSERT_EQ(nodes[2].type, NodeType::ListItem);
    ASSERT_EQ(nodes[3].type, NodeType::Paragraph);

    EXPECT_NEAR(cache.Top(0), cache.Top(1), 0.01f);
    EXPECT_NEAR(cache.Top(2), cache.Top(3), 0.01f);
}

// 空 LI の first_line_height はフォールバック (font_size*FALLBACK_LINE_HEIGHT_FACTOR) なので
// 実 line metrics と完全一致しない → epsilon 3px で許容。
TEST_F(LayoutTest, LooseListBulletCenteredOnFollowingParagraphLine)
{
    auto [nodes, cache] = ParseAndLayout("- a\n\n- b");

    ASSERT_EQ(nodes.size(), 4u);
    ASSERT_NE(cache[1].text_layout.Get(), nullptr);
    DWRITE_LINE_METRICS lm;
    UINT32 lc;
    ASSERT_TRUE(SUCCEEDED(cache[1].text_layout->GetLineMetrics(&lm, 1, &lc)));
    ASSERT_GT(lc, 0u);

    CommandGenerator gen;
    gen.SetTheme(&theme_);
    gen.SetFormats({ nullptr, nullptr, nullptr });
    PaneRect md_pane{ 0, 0, 800.0f, 2000.0f };
    auto cmds = gen.GenerateMdPane(nodes, cache, md_pane, 0.0f, TextSelection{});

    const float expected_y = cache.Top(1) + lm.height * 0.5f;
    bool found = false;
    for (const auto& cmd : cmds) {
        if (auto* e = std::get_if<FillEllipseCmd>(&cmd)) {
            EXPECT_NEAR(e->center.y, expected_y, 3.0f);
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "FillEllipseCmd が見つからない";
}

// 旧実装は checkbox 描画を GenNodeTextDecorations に置いており、loose の空 TaskListItem
// (text_layout=nullptr) で early return され checkbox ごと消えていた。
TEST_F(LayoutTest, LooseTaskListCheckboxIsEmitted)
{
    auto [nodes, cache] = ParseAndLayout("- [ ] a\n\n- [x] b");

    ASSERT_EQ(nodes.size(), 4u);
    EXPECT_EQ(nodes[0].type, NodeType::TaskListItem);
    EXPECT_FALSE(nodes[0].HasText());
    EXPECT_EQ(nodes[2].type, NodeType::TaskListItem);
    EXPECT_FALSE(nodes[2].HasText());

    // checkbox 描画には formats_.icon_font (非 null) が必要。
    Microsoft::WRL::ComPtr<IDWriteTextFormat> icon_fmt;
    ASSERT_TRUE(SUCCEEDED(dwrite_factory_->CreateTextFormat(
        L"Segoe UI Symbol", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        theme_.font_size_body, L"", &icon_fmt)));

    CommandGenerator gen;
    gen.SetTheme(&theme_);
    gen.SetFormats({ nullptr, icon_fmt.Get(), nullptr });
    PaneRect md_pane{ 0, 0, 800.0f, 2000.0f };
    auto cmds = gen.GenerateMdPane(nodes, cache, md_pane, 0.0f, TextSelection{});

    int unchecked = 0;
    int checked = 0;
    for (const auto& cmd : cmds) {
        if (auto* t = std::get_if<DrawTextCmd>(&cmd); t && t->text_len == 1) {
            const wchar_t ch = t->text()[0];
            if (ch == L'☐') {
                ++unchecked;
            }
            else if (ch == L'☑') {
                ++checked;
            }
        }
    }
    EXPECT_EQ(unchecked, 1);
    EXPECT_EQ(checked, 1);
}

// 22000 ノード × 200 iter で RecomputeYPositions のフルパス (from_index=0) 経過時間を測る。
// 通常の test 走行から外すため DISABLED_ プレフィックス。実行は次のコマンドで:
//   build/tests/Release/mendo_tests.exe --gtest_filter='RecomputeYPositionsTest.DISABLED_BenchLargeDocument' --gtest_also_run_disabled_tests
TEST(RecomputeYPositionsTest, DISABLED_BenchLargeDocument)
{
    using namespace mendo::layout;
    constexpr int N = 22000;
    constexpr int ITER = 200;

    std::pmr::vector<Node> nodes;
    nodes.resize(N);
    for (int i = 0; i < N; i++) {
        switch (i % 5) {
        case 0:
            nodes[i].type = NodeType::Heading;
            nodes[i].set_heading_level(2);
            break;
        case 1:
            nodes[i].type = NodeType::Paragraph;
            break;
        case 2:
            nodes[i].type = NodeType::CodeBlock;
            break;
        case 3:
            nodes[i].type = NodeType::ListItem;
            break;
        case 4:
            nodes[i].type = NodeType::HorizontalRule;
            break;
        }
    }

    Theme theme = MakeLayoutTestTheme();

    LayoutCache cache;
    cache.Resize(N);
    EstimateNodeHeights(nodes, cache, theme);

    // ウォームアップ
    for (int i = 0; i < 5; i++) {
        RecomputeYPositions(nodes, cache, theme);
    }

    auto start = std::chrono::high_resolution_clock::now();
    for (int iter = 0; iter < ITER; iter++) {
        RecomputeYPositions(nodes, cache, theme);
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    std::cout << "[BENCH] RecomputeYPositions N=" << N
              << " ITER=" << ITER
              << " total=" << elapsed_us << "us"
              << " avg=" << (static_cast<double>(elapsed_us) / ITER) << "us/iter\n";

    // EnsureVisibleLayout の実ユースケース: 可視範囲 (5 ノード) だけ height を弄り、
    // [from_index=100, safe_exit_after=104] で呼ぶ。tail [105, N) は shift-only パスを通る。
    constexpr size_t kFromIndex = 100;
    constexpr size_t kSafeExitAfter = 104;
    for (int i = 0; i < 5; i++) {
        cache[kFromIndex + static_cast<size_t>(i)].height += 1.0f; // 高さを揺らして delta != 0 にする
        RecomputeYPositions(nodes, cache, theme, kFromIndex, kSafeExitAfter);
    }
    auto start2 = std::chrono::high_resolution_clock::now();
    for (int iter = 0; iter < ITER; iter++) {
        cache[kFromIndex].height += (iter % 2 == 0) ? 0.5f : -0.5f;
        RecomputeYPositions(nodes, cache, theme, kFromIndex, kSafeExitAfter);
    }
    auto end2 = std::chrono::high_resolution_clock::now();
    auto elapsed2_us = std::chrono::duration_cast<std::chrono::microseconds>(end2 - start2).count();
    std::cout << "[BENCH] RecomputeYPositions (tail-shift) N=" << N
              << " from=" << kFromIndex << " safe_exit=" << kSafeExitAfter
              << " ITER=" << ITER
              << " total=" << elapsed2_us << "us"
              << " avg=" << (static_cast<double>(elapsed2_us) / ITER) << "us/iter\n";
}

// 幅不変の部分レイアウトは可視先頭から走査し、可視範囲の高さ変化は後続へ一括シフトする。
// 結果の Y 位置は全件レイアウトと一致しなければならない。
TEST_F(LayoutTest, PartialLayoutFromVisibleStartMatchesFullLayout)
{
    auto [nodes, cache] = ParseAndLayout(MakeParagraphs(200, " with some words"));
    std::vector<float> ref_tops(nodes.size());
    for (size_t i = 0; i < nodes.size(); i++) {
        ref_tops[i] = cache.Top(i);
    }

    // 可視帯のノードだけ高さを狂わせて dirty にし、Y をその高さで組み直しておく。
    for (size_t i = 100; i < 105; i++) {
        cache[i].height += 37.0f;
        cache[i].layout_dirty = true;
    }
    RecomputeYPositions(nodes, cache, theme_);
    ASSERT_NE(cache.Top(150), ref_tops[150]);

    engine_.ComputeLayout(nodes, cache, 800.0f, cache.Top(100), cache.Bottom(104));

    for (size_t i = 0; i < nodes.size(); i++) {
        EXPECT_FLOAT_EQ(cache.Top(i), ref_tops[i]) << "i=" << i;
    }
    EXPECT_TRUE(engine_.HasDirtyNodes()) << "走査しない範囲の dirty は保守的に仮定する";
}

// 範囲指定の RecomputeYPositions は、範囲外の後続ノードを一定量シフトするだけで全件計算と一致する。
TEST_F(LayoutTest, RecomputeYPositionsWithRangeMatchesFull)
{
    auto [nodes, cache] = ParseAndLayout(MakeParagraphs(50));

    cache[20].height += 50.0f;
    RecomputeYPositions(nodes, cache, theme_, 20, 20);
    std::vector<float> ranged(nodes.size());
    for (size_t i = 0; i < nodes.size(); i++) {
        ranged[i] = cache.Top(i);
    }
    RecomputeYPositions(nodes, cache, theme_);
    for (size_t i = 0; i < nodes.size(); i++) {
        EXPECT_FLOAT_EQ(ranged[i], cache.Top(i)) << "i=" << i;
    }
}
