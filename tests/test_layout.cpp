#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include <format>
#include <functional>
#include <initializer_list>
#include <random>
#include <ranges>
#include <string>
#include <string_view>
#include "block_h_scroll.h"
#include "command_generator.h"
#include "document_test_helpers.h"
#include "draw_cmd_test_util.h"
#include "dwrite_test_base.h"
#include "layout_invariants.h"
#include "parser.h"
#include "test_helpers.h"
#include "test_nodes.h"

using layout::ComputeColumnWidths;
using layout::EstimateNodeHeights;
using layout::NodeContentWidth;
using layout::NodeIndent;
using layout::RecomputeYPositions;

class LayoutTest : public DWriteTestBase {};

namespace {

// RecomputeYPositions / EstimateNodeHeights に渡す手組みノードの仕様。
struct NodeSpec {
    NodeType type = NodeType::Paragraph;
    std::string_view text;
    int8_t heading_level = 0;
    uint32_t table_rows = 0;
    int8_t quote_depth = 0;
};

std::vector<Node> MakeNodes(std::initializer_list<NodeSpec> specs)
{
    std::vector<Node> nodes;
    for (const auto& spec : specs) {
        Node& n = nodes.emplace_back();
        n.type = spec.type;
        if (!spec.text.empty()) {
            SetNodeTextCounted(n, spec.text);
        }
        if (spec.heading_level > 0) {
            n.heading_level = spec.heading_level;
        }
        if (spec.table_rows > 0) {
            AttachTestTable(n)->row_count = spec.table_rows;
        }
        n.quote_depth = spec.quote_depth;
    }
    return nodes;
}

// 計測済み (layout_dirty=false) の高さだけを持つ cache。RecomputeYPositions の入力用。
LayoutCache MakeMeasuredCache(std::initializer_list<float> heights)
{
    LayoutCache cache(heights.size());
    for (const auto [i, h] : std::views::enumerate(heights)) {
        auto& entry = cache[static_cast<size_t>(i)];
        entry.height = h;
        entry.layout_dirty = false;
    }
    return cache;
}

LayoutCache EstimateHeights(const std::vector<Node>& nodes, const Theme& theme)
{
    LayoutCache cache(nodes.size());
    EstimateNodeHeights(nodes, cache, theme, 0.0f, {});
    return cache;
}

float GapAfter(const LayoutCache& cache, size_t i)
{
    return cache.Top(i + 1) - (cache.Top(i) + cache[i].height);
}

} // namespace

// 種類の混ざった文書で Y の連鎖が保たれ、種類ごとに要るレイアウトが作られる。
TEST_F(LayoutTest, MixedDocumentLayout)
{
    auto [nodes, cache] = ParseAndLayout(
        "# Heading\n\nParagraph\n\n```\ncode\n```\n\n---\n\n- List\n\n"
        "| A | B |\n|---|---|\n| 1 | 2 |\n\nC\n\nD");
    const std::vector<NodeType> expected_types{
        NodeType::Heading, NodeType::Paragraph, NodeType::CodeBlock, NodeType::HorizontalRule,
        NodeType::ListItem, NodeType::Table, NodeType::Paragraph, NodeType::Paragraph,
    };
    ASSERT_EQ(nodes | std::views::transform(&Node::type) | std::ranges::to<std::vector>(), expected_types);
    constexpr size_t kTable = 5;

    EXPECT_TRUE(YChainConsistent(nodes, cache, theme_));
    for (size_t i = 0; i < nodes.size(); i++) {
        SCOPED_TRACE(i);
        EXPECT_GT(cache[i].height, 0.0f);
        const bool has_text = nodes[i].type != NodeType::HorizontalRule && nodes[i].type != NodeType::Table;
        EXPECT_EQ(cache[i].text_layout() != nullptr, has_text);
    }
    EXPECT_GT(cache[0].height, cache[1].height) << "見出しは段落より高い";
    EXPECT_LE(cache.Bottom(nodes.size() - 1), ComputeTotalContentHeight(cache, theme_.margin_bottom));

    const auto* tl = cache[kTable].table_layout();
    ASSERT_NE(tl, nullptr);
    EXPECT_FALSE(tl->col_widths.empty());
    const auto* tbl = nodes[kTable].table_data();
    for (size_t r = 0; r < tbl->row_count; r++) {
        for (size_t c = 0; c < tbl->col_count; c++) {
            if (!tbl->GetCellText(r, c).empty()) {
                EXPECT_NE(tl->GetCellLayout(r, c), nullptr) << r << "," << c;
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

    ASSERT_NE(cache[0].table_layout(), nullptr);
    IDWriteTextLayout* cell_layout = cache[0].table_layout()->GetCellLayout(1, 1);
    ASSERT_NE(cell_layout, nullptr);

    bool has_link_run = false;
    for (const auto& run : tbl->GetCellRuns(1, 1)) {
        if (run.has_link()) {
            has_link_run = true;

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

// ========================================================
// 抽出されたフリー関数のテスト
// ========================================================

// ---- ComputeColumnWidths テスト ----

namespace {
std::vector<float> ColumnWidths(std::span<const float> natural, std::span<const float> min, float available)
{
    std::vector<float> out;
    ComputeColumnWidths(out, natural, min, available);
    return out;
}
} // namespace

// 最小幅が 0 なら、収まらない分は自然幅に比例して縮める。
TEST(ComputeColumnWidthsTest, ProportionalDistributionWhenTooWide)
{
    const auto equal = ColumnWidths(std::array{ 100.0f, 100.0f, 100.0f }, std::array{ 0.0f, 0.0f, 0.0f }, 150.0f);
    for (const float w : equal) {
        EXPECT_NEAR(w, 50.0f, 0.01f);
    }
    const auto unequal = ColumnWidths(std::array{ 300.0f, 100.0f }, std::array{ 0.0f, 0.0f }, 200.0f);
    EXPECT_NEAR(unequal[0], 150.0f, 0.01f);
    EXPECT_NEAR(unequal[1], 50.0f, 0.01f);
}

// GitHub (Chrome の自動の表組み) と同じく、最小幅 (最も長い単語) までは縮め、余りを (自然幅 - 最小幅) に比例して配る。
// 番号のような狭い列は自然幅のまま残り、長い文の列だけが折り返す。
TEST(ComputeColumnWidthsTest, ShrinksTowardMinWidthLikeGitHub)
{
    const auto narrow_kept = ColumnWidths(std::array{ 10.0f, 2000.0f }, std::array{ 10.0f, 80.0f }, 800.0f);
    EXPECT_NEAR(narrow_kept[0], 10.0f, 0.01f);
    EXPECT_NEAR(narrow_kept[1], 790.0f, 0.01f);

    const auto two_long = ColumnWidths(std::array{ 2000.0f, 600.0f }, std::array{ 60.0f, 60.0f }, 800.0f);
    const float ratio = (800.0f - 120.0f) / (2600.0f - 120.0f);
    EXPECT_NEAR(two_long[0], 60.0f + 1940.0f * ratio, 0.01f);
    EXPECT_NEAR(two_long[1], 60.0f + 540.0f * ratio, 0.01f);
}

// 最小幅の合計も収まらなければ、最小幅で並べて横スクロールに任せる (単語の途中では折り返さない)。
TEST(ComputeColumnWidthsTest, MinWidthsOverflowForScroll)
{
    const auto out = ColumnWidths(std::array{ 500.0f, 500.0f }, std::array{ 300.0f, 250.0f }, 400.0f);
    EXPECT_FLOAT_EQ(out[0], 300.0f);
    EXPECT_FLOAT_EQ(out[1], 250.0f);
}

// 収まる表は GitHub と同じく広げず、自然幅で並べる (空の列は幅 0 で、セルの余白だけになる)。
TEST(ComputeColumnWidthsTest, KeepsNaturalWidthsWhenFits)
{
    EXPECT_EQ(ColumnWidths(std::array{ 10.0f, 25.0f, 10.0f }, std::array{ 10.0f, 5.0f, 10.0f }, 300.0f), (std::vector{ 10.0f, 25.0f, 10.0f }));
    EXPECT_EQ(ColumnWidths(std::array{ 0.0f, 0.0f }, std::array{ 0.0f, 0.0f }, 200.0f), (std::vector{ 0.0f, 0.0f }));
}

// 狭いペインでも列の幅に下限は置かない (GitHub と同じ)。最小幅が 0 の列は使える幅を比例で分ける。
TEST(ComputeColumnWidthsTest, NoPerColumnFloorInNarrowPane)
{
    for (const float w : ColumnWidths(std::array{ 200.0f, 200.0f }, std::array{ 0.0f, 0.0f }, 40.0f)) {
        EXPECT_NEAR(w, 20.0f, 0.01f);
    }
}

// 自然幅が収まるなら自然幅のまま。収まらないなら、最小幅も収まらないときだけ最小幅のまま横スクロールに任せ、それ以外は
// 合計が使える幅ちょうどで、どの列も最小幅と自然幅の間。
TEST(ComputeColumnWidthsTest, NaturalWhenFitsOtherwiseShrinksBetweenMinAndNatural)
{
    constexpr float kEps = 0.05f;
    for (const uint32_t seed : { 1u, 2u, 3u, 4u, 5u }) {
        std::mt19937 rng(seed);
        std::uniform_int_distribution<size_t> col_dist(1, 8);
        std::uniform_real_distribution<float> narrow_dist(0.0f, 60.0f);
        std::uniform_real_distribution<float> wide_dist(0.0f, 1000.0f);
        std::uniform_real_distribution<float> avail_dist(-50.0f, 2000.0f);
        std::uniform_real_distribution<float> min_ratio_dist(0.0f, 1.0f);
        std::bernoulli_distribution pick_narrow(0.5);
        for (int step = 0; step < 2000; ++step) {
            const size_t n = col_dist(rng);
            std::vector<float> natural(n);
            std::vector<float> min(n);
            for (auto [w, m] : std::views::zip(natural, min)) {
                w = pick_narrow(rng) ? narrow_dist(rng) : wide_dist(rng);
                m = w * min_ratio_dist(rng);
            }
            const float available = avail_dist(rng);
            const auto out = ColumnWidths(natural, min, available);

            // 失敗時だけ評価されるメッセージで使い、成功ケースで文字列を組み立てない。
            const auto input = [&] {
                std::string s = "seed=" + std::to_string(seed) + " step=" + std::to_string(step) + " available=" + std::to_string(available) + " natural/min=";
                for (const auto [w, m] : std::views::zip(natural, min)) {
                    s += std::to_string(w) + "/" + std::to_string(m) + ",";
                }
                return s;
            };
            ASSERT_EQ(out.size(), n) << input();

            const float natural_total = std::ranges::fold_left(natural, 0.0f, std::plus{});
            const float min_total = std::ranges::fold_left(min, 0.0f, std::plus{});
            const float out_total = std::ranges::fold_left(out, 0.0f, std::plus{});
            if (natural_total <= available) {
                ASSERT_EQ(out, natural) << input();
                continue;
            }
            if (min_total >= available) {
                ASSERT_EQ(out, min) << input();
                continue;
            }
            ASSERT_NEAR(out_total, available, kEps) << input();
            for (size_t c = 0; c < n; ++c) {
                ASSERT_GE(out[c], min[c] - kEps) << "c=" << c << " " << input();
                ASSERT_LE(out[c], natural[c] + kEps) << "c=" << c << " " << input();
            }
        }
    }
}

// DWrite 実測で、番号の列は自然幅のまま残り、長い文の列が折り返して表がペインに収まる (GitHub と同じ)。
TEST_F(LayoutTest, NarrowColumnKeepsNaturalWidthWhileLongTextWraps)
{
    std::string sentence;
    for (int i = 0; i < 60; ++i) {
        sentence += "lorem ipsum ";
    }
    auto [nodes, cache] = ParseAndLayout("| # | text |\n|---|---|\n| 1 | " + sentence + "|\n", 800.0f);
    ASSERT_EQ(nodes.size(), 1u);
    const auto* tl = cache[0].table_layout();
    ASSERT_NE(tl, nullptr);
    EXPECT_NEAR(tl->col_widths[0], tl->natural_col_widths[0], 0.01f);
    EXPECT_LT(tl->col_widths[1], tl->natural_col_widths[1]);
    EXPECT_GE(tl->col_widths[1], tl->min_col_widths[1]);
    EXPECT_GT(tl->min_col_widths[1], 0.0f);
    EXPECT_LT(tl->min_col_widths[1], tl->natural_col_widths[1] / 10.0f) << "最小幅は最も長い単語の幅";
    const auto geom = GetBlockHScrollGeometry(nodes, cache, 0, theme_, 800.0f);
    EXPECT_FALSE(geom.can_scroll());
}

// DWrite 実測で、横スクロールは描画した表の右端までちょうど届き、表の外の空白へは動かない。ペインより狭い表は動かない。
TEST_F(LayoutTest, TableHScrollReachesRightEdgeWithoutBlank)
{
    const auto table = [](size_t col_count, std::string_view cell) {
        std::string header = "|";
        std::string sep = "|";
        for (size_t c = 0; c < col_count; ++c) {
            header += std::format(" {} |", cell);
            sep += "---|";
        }
        return header + "\n" + sep + "\n" + header + "\n";
    };
    const std::string mds[] = {
        // 短い列と長い列 (合計は収まるので自然幅のまま)
        "| a | " + std::string(40, 'w') + " |\n|---|---|\n| b | c |\n",
        // 長い文のセルを折り返して収める
        table(2, "lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor incididunt ut labore"),
        // 列が多く、最小幅でもペインを超えて表がはみ出す
        table(15, "a"),
        table(30, "a"),
        // 長い単語は最小幅でも収まらず、横スクロールに任せる
        table(2, std::string(80, 'w')),
        "| a | " + std::string(300, 'w') + " |\n|---|---|\n| b | c |\n",
    };
    for (const auto& md : mds) {
        for (const float viewport_w : { 500.0f, 600.0f, 700.0f, 800.0f, 1200.0f }) {
            SCOPED_TRACE(std::format("viewport_w={} md={}", viewport_w, md.substr(0, md.find('\n'))));
            auto [nodes, cache] = ParseAndLayout(md, viewport_w);
            ASSERT_EQ(nodes.size(), 1u);
            ASSERT_NE(cache[0].table_layout(), nullptr);
            const float table_width = cache[0].table_layout()->TableWidth();
            const auto geom = GetBlockHScrollGeometry(nodes, cache, 0, theme_, viewport_w);
            EXPECT_LE(table_width, geom.visible_width + geom.scroll_max() + BLOCK_H_SCROLL_SLACK);
            EXPECT_LE(geom.visible_width + geom.scroll_max(), std::max(table_width, geom.visible_width) + 0.05f);
        }
    }
}

// コードは左余白の分だけ右から描くので、末尾までスクロールしたとき最長行の右に余白が 1 つ分残る。
// 両側の余白込みで収まらなければ、行が可視幅に収まっていてもスクロールできる。
TEST_F(LayoutTest, CodeBlockHScrollShowsLongestLineWithRightPadding)
{
    const float pad = theme_.code_block_padding;
    int needs_padding_only = 0;
    for (const size_t line_len : { size_t{ 60 }, size_t{ 300 } }) {
        const std::string md = "```\n" + std::string(line_len, 'a') + "\n```";
        for (float viewport_w = 300.0f; viewport_w <= 1200.0f; viewport_w += 3.0f) {
            SCOPED_TRACE(std::format("line_len={} viewport_w={}", line_len, viewport_w));
            auto [nodes, cache] = ParseAndLayout(md, viewport_w);
            ASSERT_EQ(nodes[0].type, NodeType::CodeBlock);
            DWRITE_TEXT_METRICS m{};
            ASSERT_TRUE(SUCCEEDED(cache[0].text_layout()->GetMetrics(&m)));
            const float line_w = m.widthIncludingTrailingWhitespace;
            const auto geom = GetBlockHScrollGeometry(nodes, cache, 0, theme_, viewport_w);
            if (line_w <= geom.visible_width && line_w + 2.0f * pad > geom.visible_width + BLOCK_H_SCROLL_SLACK) {
                ++needs_padding_only;
            }
            if (geom.can_scroll()) {
                EXPECT_NEAR(pad + line_w - geom.scroll_max(), geom.visible_width - pad, 0.05f);
            }
            else {
                EXPECT_LE(pad + line_w, geom.visible_width - pad + BLOCK_H_SCROLL_SLACK);
            }
        }
    }
    EXPECT_GT(needs_padding_only, 0);
}

// ---- RecomputeYPositions テスト ----

// 空の文書は推定も位置の計算も何もしない。
TEST(RecomputeYPositionsTest, EmptyNodes)
{
    std::vector<Node> nodes;
    LayoutCache cache;
    const Theme& theme = GetLightTheme();
    EstimateNodeHeights(nodes, cache, theme, 0.0f, {});
    RecomputeYPositions(nodes, cache, theme);
    EXPECT_EQ(cache.size(), 0u);
}

// ---- ComputeTotalContentHeight テスト ----

TEST(ComputeTotalContentHeightTest, SingleNode)
{
    LayoutCache cache(1);
    cache.SetTop(0, 15.0f);
    cache[0].height = 50.0f;
    EXPECT_FLOAT_EQ(ComputeTotalContentHeight(cache, 15.0f), 80.0f);
}

TEST(ComputeTotalContentHeightTest, MultipleNodes)
{
    LayoutCache cache(3);
    cache.SetTop(0, 10.0f);
    cache[0].height = 20.0f;
    cache.SetTop(1, 40.0f);
    cache[1].height = 30.0f;
    cache.SetTop(2, 80.0f);
    cache[2].height = 25.0f;
    // 最後のノードのみが関係: 80 + 25 + 10 = 115
    EXPECT_FLOAT_EQ(ComputeTotalContentHeight(cache, 10.0f), 115.0f);
}

// 先頭ノードは margin_top から、見出しならその上の間隔を足した位置に置く。
TEST(RecomputeYPositionsTest, FirstNodeTop)
{
    const Theme theme = GetLightTheme();
    const std::pair<NodeSpec, float> cases[] = {
        { { .type = NodeType::Paragraph }, theme.margin_top },
        { { .type = NodeType::Heading, .heading_level = 3 }, theme.margin_top + theme.heading_spacing_above },
    };
    for (const auto& [spec, expected_top] : cases) {
        SCOPED_TRACE(static_cast<int>(spec.type));
        auto nodes = MakeNodes({ spec });
        auto cache = MakeMeasuredCache({ 20.0f });
        RecomputeYPositions(nodes, cache, theme);
        EXPECT_FLOAT_EQ(cache.Top(0), expected_top);
    }
}

// ---- EnsureVisibleLayout テスト ----

TEST_F(LayoutTest, EnsureVisibleLayoutFixesDirtyVisibleNodes)
{
    auto [nodes, cache] = ParseAndLayout(MakeParagraphs(20));

    ComputeLayout(engine_, nodes, cache, 400.0f, 0.0f, 100.0f);

    ASSERT_GT(CountDirty(cache), 0u);

    // 修正パスをテストするため、表示中のノードを手動でダーティにマーク
    cache[0].layout_dirty = true;

    bool updated = engine_.EnsureVisibleLayout(nodes, cache, 400.0f, 0.0f, 100.0f);
    EXPECT_TRUE(updated);

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

// 部分モードで不可視ノードの古い height を引きずらないこと（High-1 回帰）。
// ズーム/テーマ変更直後の Y 位置に stale な height が混入しないことを保証する。
TEST_F(LayoutTest, PartialLayoutRefreshesStaleInvisibleHeights)
{
    // フル幅でフルレイアウト → 全ノード正確な height を持つ
    auto [nodes, cache] = ParseAndLayout(MakeParagraphs(30));
    const float baseline_total = ComputeTotalContentHeight(cache, theme_.margin_bottom);
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
    ComputeLayout(engine_, nodes, cache, 800.0f, 0.0f, 50.0f);
    const float last_top_after = cache.Top(node_count - 1);

    // baseline と同程度（推定誤差ぶんはあり得る）に収束し、stale の合計を
    // 引きずった巨大値にはならないこと。
    EXPECT_LT(last_top_after, baseline_total * 2.0f)
        << "stale height (=" << STALE_HEIGHT << ") を Y 位置に取り込んでいる";
}

// ---- RecomputeYPositions 追加テスト ----

TEST(RecomputeYPositionsTest, AllNodeTypesProduceValidPositions)
{
    const Theme theme = GetLightTheme();
    auto nodes = MakeNodes({
        { .type = NodeType::Paragraph },
        { .type = NodeType::Heading },
        { .type = NodeType::CodeBlock },
        { .type = NodeType::HorizontalRule },
        { .type = NodeType::ListItem },
        { .quote_depth = 1 },
        { .type = NodeType::Table },
    });
    auto cache = MakeMeasuredCache({ 20.0f, 30.0f, 50.0f, 5.0f, 18.0f, 25.0f, 60.0f });

    RecomputeYPositions(nodes, cache, theme);
    EXPECT_TRUE(YChainConsistent(nodes, cache, theme));
}

// ---- FindFirstVisibleNodeIndex テスト（layout_cache.h のフリー関数） ----

TEST(FindFirstVisibleNodeIndex, ReturnsFirstNodeEndingBelowViewportTop)
{
    struct Case {
        int count;
        float viewport_top;
        int expected;
    };
    constexpr Case kCases[] = {
        { 10, 0.0f, 0 },
        { 10, 120.0f, 2 },
        // 下端がちょうど viewport_top のノードは含めない
        { 10, 50.0f, 1 },
        { 10, 449.0f, 8 },
        { 5, 300.0f, 5 },
        { 1, 50.0f, 0 },
        { 1, 100.0f, 1 },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::Message() << c.count << "," << c.viewport_top);
        const auto cache = MakeUniformCache(c.count, c.count == 1 ? 100.0f : 50.0f);
        EXPECT_EQ(FindFirstVisibleNodeIndex(cache, c.viewport_top), c.expected);
    }
    EXPECT_EQ(FindFirstVisibleNodeIndex(LayoutCache{}, 0.0f), 0);
}

// 作った直後 (全ノードの位置・高さが 0) も該当なし扱い。
TEST(FindFirstVisibleNodeIndex, FreshCacheReturnsNodeCount)
{
    LayoutCache cache(5);
    EXPECT_EQ(FindFirstVisibleNodeIndex(cache, 0.0f), 5);
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
    const auto code_gap = [](const Theme& t) static { return t.paragraph_spacing + t.code_block_spacing_above; };
    const auto list_gap = [](const Theme& t) static { return t.list_item_spacing; };
    const auto above_heading = [](const Theme& t) static { return t.paragraph_spacing + t.heading_spacing_above; };
    const auto below_h3 = [](const Theme& t) static { return t.heading_spacing_below; };
    const auto below_h1 = [](const Theme& t) static { return t.heading_spacing_below_h1h2; };
    const auto heading_to_heading = [](const Theme& t) static { return t.heading_spacing_below + t.heading_spacing_above; };
    constexpr NodeSpec kH1{ .type = NodeType::Heading, .heading_level = 1 };
    constexpr NodeSpec kH3{ .type = NodeType::Heading, .heading_level = 3 };
    // 空テキスト LI は issue#237 の修正で sb=0 (loose 扱い)。tight LI 間隔の検証には HasText() が要る。
    // h1/h2 は下線を描くので下の間隔が h3 以降より広い。
    const Case cases[] = {
        { "ParagraphToHeading", { .type = NodeType::Paragraph }, kH3, 20.0f, 30.0f, above_heading },
        { "H3UsesHeadingSpacingBelow", kH3, { .type = NodeType::Paragraph }, 30.0f, 20.0f, below_h3 },
        { "H1UsesH1H2SpacingBelow", kH1, { .type = NodeType::Paragraph }, 40.0f, 20.0f, below_h1 },
        { "HeadingToHeading", kH3, kH3, 40.0f, 30.0f, heading_to_heading },
        { "CodeBlockHasSpacingAbove", { .type = NodeType::Paragraph }, { .type = NodeType::CodeBlock }, 20.0f, 50.0f, code_gap },
        { "CodeBlockHasSpacingBelow", { .type = NodeType::CodeBlock }, { .type = NodeType::Paragraph }, 50.0f, 20.0f, code_gap },
        { "BlockQuoteHasSpacingAbove", { .type = NodeType::Paragraph }, { .quote_depth = 1 }, 20.0f, 30.0f, code_gap },
        { "ListItemUsesListItemSpacing", { NodeType::ListItem, "a" }, { NodeType::ListItem, "b" }, 18.0f, 18.0f, list_gap },
        { "TaskListItemUsesListItemSpacing", { NodeType::TaskListItem, "a" }, { NodeType::TaskListItem, "b" }, 18.0f, 18.0f, list_gap },
    };
    const Theme theme = GetLightTheme();
    ASSERT_GT(theme.heading_spacing_below_h1h2, theme.heading_spacing_below);
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        auto nodes = MakeNodes({ c.first, c.second });
        auto cache = MakeMeasuredCache({ c.first_height, c.second_height });
        RecomputeYPositions(nodes, cache, theme);
        EXPECT_FLOAT_EQ(GapAfter(cache, 0), c.expected_gap(theme));
    }
}

// ---- from_index による途中再開 ----

// 直前ノードの spacing_below が種類で変わっても、途中から組み直した位置が全体の計算と一致すること。
TEST(RecomputeYPositionsTest, FromIndexMatchesFullRecompute)
{
    const Theme theme = GetLightTheme();
    for (const NodeType first : { NodeType::CodeBlock, NodeType::ListItem }) {
        SCOPED_TRACE(static_cast<int>(first));
        auto nodes = MakeNodes({ { .type = first }, { .type = NodeType::Paragraph } });
        auto cache = MakeMeasuredCache({ 50.0f, 20.0f });

        RecomputeYPositions(nodes, cache, theme);
        const float expected_y1 = cache.Top(1);

        RecomputeYPositions(nodes, cache, theme, 1);
        EXPECT_FLOAT_EQ(cache.Top(1), expected_y1);
    }
}

// ---- 見出し内インラインコードのフォントサイズ ----

// インラインコードは見出しでは見出しの大きさ、段落では font_size_code。字体はどちらも等幅。
TEST_F(LayoutTest, InlineCodeFontFollowsBlock)
{
    std::vector<std::pair<std::string, float>> cases{ { "Hello `code` world", theme_.font_size_code } };
    for (int level = 1; level <= 6; ++level) {
        cases.emplace_back(std::string(level, '#') + " Test `code`", theme_.font_size_h[level - 1]);
    }
    for (const auto& [md, expected_size] : cases) {
        SCOPED_TRACE(md);
        auto [nodes, cache] = ParseAndLayout(md);
        ASSERT_EQ(nodes.size(), 1u);
        IDWriteTextLayout* layout = cache[0].text_layout();
        ASSERT_NE(layout, nullptr);
        ASSERT_TRUE(std::ranges::any_of(nodes[0].runs(), &TextRun::code));
        for (const auto& run : nodes[0].runs() | std::views::filter(&TextRun::code)) {
            float size = 0.0f;
            ASSERT_HRESULT_SUCCEEDED(layout->GetFontSize(run.start, &size));
            EXPECT_FLOAT_EQ(size, expected_size);
            WCHAR family[256] = {};
            ASSERT_HRESULT_SUCCEEDED(layout->GetFontFamilyName(run.start, family, 256));
            EXPECT_EQ(std::wstring(family), theme_.monospace_font);
        }
    }
}

// ========================================================
// EstimateNodeHeights テスト
// ========================================================

// 見出しの段・行数・表の行数が増えると推定の高さも増える。
TEST(EstimateNodeHeightsTest, HeightGrowsWithContent)
{
    struct Case {
        const char* name;
        NodeSpec smaller;
        NodeSpec larger;
    };
    const Case cases[] = {
        { "HeadingLevel", { NodeType::Heading, "Title", 3 }, { NodeType::Heading, "Title", 1 } },
        { "CodeBlockLines", { NodeType::CodeBlock, "line1" }, { NodeType::CodeBlock, "line1\nline2\nline3\nline4\nline5" } },
        { "TableRows", { .type = NodeType::Table, .table_rows = 1 }, { .type = NodeType::Table, .table_rows = 3 } },
        { "ParagraphLines", { .text = "one line" }, { .text = "line1\nline2\nline3" } },
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        const auto nodes = MakeNodes({ c.smaller, c.larger });
        const auto cache = EstimateHeights(nodes, GetLightTheme());
        EXPECT_GT(cache[1].height, cache[0].height);
    }
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

TEST(EstimateNodeHeightsTest, EmptyTextNodeUsesSpacing)
{
    const Theme theme = GetLightTheme();
    const auto nodes = MakeNodes({ { .type = NodeType::Paragraph } });
    const auto cache = EstimateHeights(nodes, theme);

    EXPECT_FLOAT_EQ(cache[0].height, theme.paragraph_spacing);
}

TEST(EstimateNodeHeightsTest, LayoutDirtyNotChanged)
{
    const Theme theme = GetLightTheme();
    const auto nodes = MakeNodes({ { .text = "test" } });
    LayoutCache cache(nodes.size());

    ASSERT_TRUE(cache[0].layout_dirty);

    EstimateNodeHeights(nodes, cache, theme, 0.0f, {});

    EXPECT_TRUE(cache[0].layout_dirty);
}

// 停止の確認で区切っても Y は続けて積む。区切りをまたぐ文書で、高さから一度に積み直した位置と一致する。
TEST(EstimateNodeHeightsTest, PositionsContinueAcrossStopChecks)
{
    const Theme theme = GetLightTheme();
    static const std::string md = [] {
        std::string s;
        for (int i = 0; i < 5000; ++i) {
            s += (i % 7 == 0) ? "# h\n\n" : "p\n\n";
        }
        return s;
    }();
    const auto nodes = ParseNodes(md);
    ASSERT_GT(nodes.size(), 4096u);
    const auto estimated = EstimateHeights(nodes, theme);
    auto recomputed = EstimateHeights(nodes, theme);
    RecomputeYPositions(nodes, recomputed, theme);
    const auto mismatch = std::ranges::find_if(std::views::iota(size_t{ 0 }, nodes.size()), [&](size_t i) {
        return estimated.Top(i) != recomputed.Top(i);
    });
    EXPECT_EQ(*mismatch, nodes.size());
}

TEST(EstimateNodeHeightsTest, AllNodeTypesProducePositiveHeight)
{
    const auto nodes = MakeNodes({
        { NodeType::Paragraph, "content" },
        { NodeType::Heading, "content", 2 },
        { NodeType::CodeBlock, "content" },
        { NodeType::HorizontalRule },
        { NodeType::ListItem, "content" },
        { NodeType::Paragraph, "content", 0, 0, 1 },
        { NodeType::Table, "content", 0, 1 },
        { NodeType::TaskListItem, "content" },
        { NodeType::Image },
    });
    const Theme theme = GetLightTheme();
    const auto cache = EstimateHeights(nodes, theme);

    EXPECT_GE(cache.Top(0), theme.margin_top);
    for (size_t i = 0; i < nodes.size(); i++) {
        EXPECT_GT(cache[i].height, 0.0f)
            << "ノードタイプ " << static_cast<int>(nodes[i].type) << " の高さが正であること";
    }
}

TEST_F(LayoutTest, EstimateVsActualHeightReasonableRange)
{
    // 推定値がDirectWrite実測値と比べて極端に乖離しないことを確認する
    auto nodes = ParseNodes("# Heading\n\nParagraph text\n\n```\ncode\n```\n\n---");
    LayoutCache est_cache(nodes.size());

    LayoutCache actual_cache(nodes.size());

    EstimateNodeHeights(nodes, est_cache, theme_, 0.0f, {});
    ComputeLayout(engine_, nodes, actual_cache, 800.0f);

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

    ASSERT_NE(cache[0].text_layout(), nullptr);
    DWRITE_LINE_METRICS lm;
    UINT32 lc;
    ASSERT_TRUE(SUCCEEDED(cache[0].text_layout()->GetLineMetrics(&lm, 1, &lc)));
    ASSERT_GT(lc, 0u);

    // bullet 中心は物理ピクセル境界へスナップされるため、期待値も同じ規則でスナップする。
    float expected_y = SnapToPhysicalPixel(cache.Top(0) + lm.height * 0.5f, 1.0f);

    CommandGenerator gen;
    InitWithoutText(gen, theme_);
    const auto cmds = GenerateMdPaneAt(gen, nodes, cache, PaneRect{ 0, 0, 800.0f, 2000.0f });

    const auto bullet = FindFirst<FillRoundedRectCmd>(cmds, kDisc);
    ASSERT_TRUE(bullet.has_value()) << "● が見つからない";
    EXPECT_NEAR(RectCenter(bullet->rect).y, expected_y, 0.01f)
        << "箇条書き記号は1行目の中央に配置されるべき";
}

// インデントが本文幅を超える深いネストでも、幅に下限があるのでテキストレイアウトを作れる。
TEST_F(LayoutTest, DeeplyNestedParagraphStillGetsTextLayout)
{
    std::string md;
    for (int i = 0; i < 127; i++) {
        md += "> ";
    }
    md += "text";
    auto [nodes, cache] = ParseAndLayout(md);

    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_GT(NodeIndent(nodes[0], theme_), theme_.ContentWidth(800.0f));
    EXPECT_NE(cache[0].text_layout(), nullptr);
    EXPECT_FALSE(cache[0].layout_dirty);
    EXPECT_GT(cache[0].height, 0.0f);
}

TEST(NodeContentWidthTest, FloorsAtMinimumButNotAboveContentWidth)
{
    const Theme theme = GetLightTheme();
    Node node;
    node.indent_level = 127;
    EXPECT_FLOAT_EQ(NodeContentWidth(node, theme, 800.0f), MIN_NODE_CONTENT_WIDTH);
    EXPECT_FLOAT_EQ(NodeContentWidth(node, theme, MIN_NODE_CONTENT_WIDTH * 0.5f), MIN_NODE_CONTENT_WIDTH * 0.5f);
    EXPECT_FLOAT_EQ(NodeContentWidth(node, theme, 0.0f), 0.0f);
    node.indent_level = 1;
    EXPECT_FLOAT_EQ(NodeContentWidth(node, theme, 800.0f), 800.0f - theme.indent_width);
}

// 余白はズームで広がるが本文ペインの最小幅は広がらない。最大ズームで最小幅まで狭めても本文幅は負にならない。
TEST(NodeContentWidthTest, ContentWidthStaysPositiveAtMaxZoomAndMinPaneWidth)
{
    Theme theme = GetLightTheme();
    theme.ApplyZoom(ZOOM_STEPS[ZOOM_STEP_COUNT - 1]);
    ASSERT_GT(theme.margin_left + theme.margin_right, MD_PANE_MIN_WIDTH) << "前提: 余白だけでペインの最小幅を超える";
    EXPECT_FLOAT_EQ(theme.ContentWidth(MD_PANE_MIN_WIDTH), MIN_NODE_CONTENT_WIDTH);
    Node node;
    node.indent_level = 3;
    EXPECT_FLOAT_EQ(NodeContentWidth(node, theme, theme.ContentWidth(MD_PANE_MIN_WIDTH)), MIN_NODE_CONTENT_WIDTH);
    // 最小化などで幅が無いときは 0 のまま (画像の適用などが「未確定」として飛ばす)
    EXPECT_FLOAT_EQ(theme.ContentWidth(0.0f), 0.0f);
}

// 子ブロックの無い空 LI は 1 行ぶんの高さを持ち、重ならない (高さ 0 だと全部が同じ Y に積まれる)。
// 推定と実測は別の場所で同じ式を書いているので、片方だけ変わると計測のたびにスクロール位置が飛ぶ。
TEST_F(LayoutTest, EmptyListItemEstimateMatchesMeasuredHeight)
{
    auto [nodes, cache] = ParseAndLayout("1.\n2.\n- [ ]\n");
    ASSERT_EQ(nodes.size(), 3u);
    const auto estimated = EstimateHeights(nodes, theme_);
    const float expected = layout::EmptyListItemHeight(theme_);
    EXPECT_GT(expected, 0.0f);
    EXPECT_LT(cache.Top(0), cache.Top(1));
    EXPECT_LT(cache.Top(1), cache.Top(2));
    for (size_t i = 0; i < nodes.size(); i++) {
        SCOPED_TRACE(i);
        EXPECT_FLOAT_EQ(cache[i].height, expected);
        EXPECT_FLOAT_EQ(estimated[i].height, expected);
    }
    EXPECT_FLOAT_EQ(cache.Top(1) - cache.Top(0), estimated.Top(1) - estimated.Top(0));
}

// loose list の LI (空) と直下の段落の text_top は一致する (issue#237)。記号は段落の 1 行目の中央に置く。
// 空 LI の first_line_height は本文の行送りで、段落の 1 行目と同じ。差は物理ピクセルへのスナップの分だけ。
TEST_F(LayoutTest, LooseListBulletCenteredOnFollowingParagraphLine)
{
    auto [nodes, cache] = ParseAndLayout("- a\n\n- b");

    ASSERT_EQ(nodes.size(), 4u);
    ASSERT_EQ(nodes[0].type, NodeType::ListItem);
    ASSERT_EQ(nodes[1].type, NodeType::Paragraph);
    ASSERT_EQ(nodes[2].type, NodeType::ListItem);
    ASSERT_EQ(nodes[3].type, NodeType::Paragraph);
    EXPECT_NEAR(cache.Top(0), cache.Top(1), 0.01f);
    EXPECT_NEAR(cache.Top(2), cache.Top(3), 0.01f);
    ASSERT_NE(cache[1].text_layout(), nullptr);
    DWRITE_LINE_METRICS lm;
    UINT32 lc;
    ASSERT_TRUE(SUCCEEDED(cache[1].text_layout()->GetLineMetrics(&lm, 1, &lc)));
    ASSERT_GT(lc, 0u);

    CommandGenerator gen;
    InitWithoutText(gen, theme_);
    const auto cmds = GenerateMdPaneAt(gen, nodes, cache, PaneRect{ 0, 0, 800.0f, 2000.0f });

    const float expected_y = cache.Top(1) + lm.height * 0.5f;
    const auto bullet = FindFirst<FillRoundedRectCmd>(cmds, kDisc);
    ASSERT_TRUE(bullet.has_value()) << "● が見つからない";
    EXPECT_NEAR(RectCenter(bullet->rect).y, expected_y, 0.5f);
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

    // checkbox 描画には formats_.checkbox (非 null) が必要。
    Microsoft::WRL::ComPtr<IDWriteTextFormat> icon_fmt;
    ASSERT_TRUE(SUCCEEDED(dwrite_factory_->CreateTextFormat(
        L"Segoe UI Symbol", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        theme_.font_size_body, L"", &icon_fmt)));

    TextLayoutCache text_layouts;
    text_layouts.SetFactory(dwrite_factory_.Get());
    CommandGenerator gen;
    const MdPaneFormats formats{ .checkbox = icon_fmt };
    gen.Init(theme_, formats, text_layouts);
    const auto cmds = GenerateMdPaneAt(gen, nodes, cache, PaneRect{ 0, 0, 800.0f, 2000.0f });

    int unchecked = 0;
    int checked = 0;
    for (const auto& cmd : cmds) {
        if (auto* t = std::get_if<DrawTextLayoutCmd>(&cmd)) {
            const auto text = text_layouts.TextOf(t->layout);
            if (text == L"☐") {
                ++unchecked;
            }
            else if (text == L"☑") {
                ++checked;
            }
        }
    }
    EXPECT_EQ(unchecked, 1);
    EXPECT_EQ(checked, 1);
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

    ComputeLayout(engine_, nodes, cache, 800.0f, cache.Top(100), cache.Bottom(104));

    // 一括シフトは足す順序が全件レイアウトと違い、行の高さが 2 進で割り切れないと丸めがずれる。
    for (size_t i = 0; i < nodes.size(); i++) {
        EXPECT_NEAR(cache.Top(i), ref_tops[i], 0.01f) << "i=" << i;
    }
    EXPECT_TRUE(cache.HasDirtyNodes()) << "走査しない範囲の dirty は保守的に仮定する";
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
