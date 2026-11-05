#include <gtest/gtest.h>
#include "document.h"
#include "layout_cache.h"
#include "layout_computer.h"
#include "theme.h"
#include "test_helpers.h"
#include <optional>

// 全レイアウトの無効化 (ズームなど) は計測結果を捨てる。旧倍率の実測値や高さが残ると、推定が縮めないので
// 画面外のコードブロック等の高さや総高さが古いまま過大になる。位置は次のレイアウトで組み直すまで保つ。
TEST(LayoutCacheTest, InvalidateAllLayoutsDropsMeasurement)
{
    LayoutCache cache(3);
    const std::vector<Node> nodes(3);
    for (size_t i = 0; i < 3; ++i) {
        cache[i].ensure_inline_code_bgs().emplace_back(0.0f, 0.0f, 10.0f, 10.0f);
        cache[i].cached_width = 600.0f;
        cache[i].height = 500.0f;
        cache.SetTop(i, 100.0f + static_cast<float>(i) * 50.0f);
    }

    cache.InvalidateAllLayouts(nodes);

    for (size_t i = 0; i < 3; ++i) {
        SCOPED_TRACE(i);
        EXPECT_TRUE(cache[i].view_inline_code_bgs().empty());
        EXPECT_EQ(cache[i].text_layout(), nullptr);
        EXPECT_FALSE(cache[i].is_measured());
        EXPECT_FLOAT_EQ(cache[i].height, 0.0f);
        EXPECT_FLOAT_EQ(cache.Top(i), 100.0f + static_cast<float>(i) * 50.0f);
    }

    // 空のキャッシュでも落ちない
    LayoutCache empty(0);
    empty.InvalidateAllLayouts({});
}

// 画像・図の高さと寸法 (DiagramEntry) は無効化で捨てない (ズーム→復帰で Mermaid 図が消える問題の再発防止)。
TEST(LayoutCacheTest, InvalidateAllLayoutsKeepsBitmapNodes)
{
    LayoutCache cache(3);
    std::vector<Node> nodes(3);
    nodes[1].type = NodeType::Image;
    nodes[2].type = NodeType::Diagram;
    nodes[2].code_language = SyntaxLanguage::Mermaid;
    for (size_t i = 0; i < 3; i++) {
        cache[i].height = 500.0f;
    }
    cache.EnsureDiagram(1).width = 400.0f;
    cache.EnsureDiagram(1).height = 300.0f;
    cache.EnsureDiagram(2).width = 500.0f;
    cache.EnsureDiagram(2).height = 250.0f;

    cache.InvalidateAllLayouts(nodes);

    EXPECT_FLOAT_EQ(cache[0].height, 0.0f);
    EXPECT_FLOAT_EQ(cache[1].height, 500.0f);
    EXPECT_FLOAT_EQ(cache[2].height, 500.0f);
    EXPECT_FLOAT_EQ(cache.FindDiagram(1)->width, 400.0f);
    EXPECT_FLOAT_EQ(cache.FindDiagram(1)->height, 300.0f);
    EXPECT_FLOAT_EQ(cache.FindDiagram(2)->width, 500.0f);
    EXPECT_FLOAT_EQ(cache.FindDiagram(2)->height, 250.0f);
}

TEST(LayoutComputerTest, EstimateAfterInvalidateAllLayoutsCanShrink)
{
    const Theme theme = MakeLayoutTestTheme();
    LayoutCache cache(1);
    std::vector<Node> nodes;
    nodes.push_back(MakeTextNode("short"));
    cache[0].height = 5000.0f;

    cache.InvalidateAllLayouts(nodes);
    layout::EstimateInvisibleNodeHeight(nodes[0], cache[0], theme, 600.0f);

    EXPECT_FLOAT_EQ(cache[0].height, layout::EstimateNodeHeight(nodes[0], theme, 0.0f));
}

// 幅が分かれば、折り返す段落の行数を文字数から見積もる。幅を知らない推定はハード改行の数だけを見る。
TEST(LayoutComputerTest, EstimateNodeHeightWrapsLongParagraphByWidth)
{
    const Theme theme = MakeLayoutTestTheme();
    static const std::string long_text(1000, 'a');
    const Node long_node = MakeTextNode(long_text.c_str());
    const Node short_node = MakeTextNode("short");

    const float width_free = layout::EstimateNodeHeight(long_node, theme, 0.0f);
    const float wide = layout::EstimateNodeHeight(long_node, theme, 2000.0f);
    const float narrow = layout::EstimateNodeHeight(long_node, theme, 200.0f);
    EXPECT_GT(wide, width_free);
    EXPECT_GT(narrow, wide);
    EXPECT_FLOAT_EQ(layout::EstimateNodeHeight(short_node, theme, 600.0f), layout::EstimateNodeHeight(short_node, theme, 0.0f));
}

// FindDiagram は確保しない。読み取り・破棄経路で未ロードのノード分を確保させないため。
TEST(LayoutCacheTest, FindDiagramDoesNotAllocate)
{
    LayoutCache cache(2);

    EXPECT_EQ(cache.FindDiagram(0), nullptr);

    cache.EnsureDiagram(1).width = 10.0f;
    ASSERT_NE(cache.FindDiagram(1), nullptr);
    EXPECT_FLOAT_EQ(cache.FindDiagram(1)->width, 10.0f);
    EXPECT_EQ(cache.FindDiagram(0), nullptr);
}

// テーマ変更やデバイスロストで全ビットマップを捨てるとき、エラー状態も消して再試行の契機にする (issue #271)。
// 寸法は再計測が表示高さに使うので残す。
TEST(LayoutCacheTest, InvalidateAllDiagramBitmapsClearsErrorAndKeepsSize)
{
    LayoutCache cache(2);
    for (size_t i = 0; i < 2; i++) {
        auto& d = cache.EnsureDiagram(i);
        d.width = 400.0f;
        d.height = 300.0f;
        d.error = { DiagramError::Kind::Render, L"Parse error on line 1" };
    }

    cache.InvalidateAllDiagramBitmaps();

    for (size_t i = 0; i < 2; i++) {
        EXPECT_TRUE(cache.FindDiagram(i)->error.empty());
        EXPECT_FLOAT_EQ(cache.FindDiagram(i)->width, 400.0f);
        EXPECT_FLOAT_EQ(cache.FindDiagram(i)->height, 300.0f);
    }
}

// 後続を一定量ずらす経路 (safe_exit_after) でずれが無いときも、残りノードの text_top がフル計算と一致する。
TEST(LayoutCacheTest, RecomputeYPositionsEarlyExitKeepsTextTop)
{
    LayoutCache cache(8);

    const Theme theme = MakeLayoutTestTheme();

    std::vector<Node> nodes;
    nodes.resize(8);
    for (auto& n : nodes) {
        n.type = NodeType::Paragraph;
    }

    layout::EstimateNodeHeights(nodes, cache, theme, 0.0f, {});
    std::vector<float> expected;
    for (size_t i = 0; i < cache.size(); ++i) {
        cache[i].layout_dirty = false;
        expected.push_back(cache.Top(i));
    }

    // EstimateNodeHeights 直後なので、index >= 3 のノードのずれは無い。
    layout::RecomputeYPositions(nodes, cache, theme, 0, 2);

    for (size_t i = 0; i < cache.size(); ++i) {
        EXPECT_FLOAT_EQ(cache.Top(i), expected[i]) << "after early-exit, index " << i;
    }
}

// ---- NodeOffsetToScrollY ----

TEST(NodeOffsetToScrollYTest, ClampsNodeAndResult)
{
    EXPECT_FLOAT_EQ(NodeOffsetToScrollY(LayoutCache{}, 0, 50.0f), 0.0f);

    const auto cache = MakeUniformCache(3);
    struct Case {
        int node;
        float offset;
        float expected;
    };
    constexpr Case kCases[] = {
        { -1, 50.0f, 0.0f },
        { 1, 25.0f, 125.0f },
        // 文書が短くなった後の復元などで末尾を超えた node は最後のノードに寄せる
        { 10, 5.0f, 205.0f },
        { 0, -40.0f, 0.0f },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::Message() << c.node << "," << c.offset);
        EXPECT_FLOAT_EQ(NodeOffsetToScrollY(cache, c.node, c.offset), c.expected);
    }
}

// ---- ComputeVisibleNodeRange ----

TEST(ComputeVisibleNodeRangeTest, ReturnsNodesOverlappingRange)
{
    const auto cache = MakeUniformCache(10);
    const auto [first, last_plus_1] = ComputeVisibleNodeRange(cache, 150.0f, 350.0f);
    EXPECT_EQ(first, 1u);
    EXPECT_EQ(last_plus_1, 4u);
}

TEST(ComputeVisibleNodeRangeTest, RangeBelowContentIsEmpty)
{
    const auto cache = MakeUniformCache(3);
    const auto [first, last_plus_1] = ComputeVisibleNodeRange(cache, 500.0f, 800.0f);
    EXPECT_EQ(first, last_plus_1);
}

// ---- TableLayoutData の行範囲 ----

namespace {

// 高さ 10 の行が 3 行 ([0,10) [10,20) [20,30))。
TableLayoutData MakeThreeRowTable()
{
    TableLayoutData tl(3, 1, false);
    tl.row_cum_y = { 0.0f, 10.0f, 20.0f, 30.0f };
    return tl;
}

} // namespace

TEST(TableRowRangeTest, VisibleRowRangeSelectsOverlappingRows)
{
    const auto tl = MakeThreeRowTable();
    EXPECT_EQ(tl.VisibleRowRange(15.0f, 25.0f), (std::pair<size_t, size_t>{ 1, 3 }));
    EXPECT_EQ(tl.VisibleRowRange(-50.0f, 5.0f), (std::pair<size_t, size_t>{ 0, 1 }));
    EXPECT_EQ(tl.VisibleRowRange(-50.0f, 500.0f), (std::pair<size_t, size_t>{ 0, 3 }));
}

TEST(TableRowRangeTest, VisibleRowRangeBelowTableIsEmpty)
{
    const auto tl = MakeThreeRowTable();
    const auto [begin, end] = tl.VisibleRowRange(100.0f, 200.0f);
    EXPECT_GE(begin, end);
}

TEST(TableRowRangeTest, RowIndexAtBoundaries)
{
    const auto tl = MakeThreeRowTable();
    EXPECT_EQ(tl.RowIndexAt(-0.1f), -1);
    EXPECT_EQ(tl.RowIndexAt(0.0f), 0);
    EXPECT_EQ(tl.RowIndexAt(9.9f), 0);
    EXPECT_EQ(tl.RowIndexAt(10.0f), 1);
    EXPECT_EQ(tl.RowIndexAt(29.9f), 2);
    EXPECT_EQ(tl.RowIndexAt(30.0f), -1);
}

// トークンは text_layout と一緒に作るので、layout を捨てたら一緒に解放される。
TEST(LayoutCacheTest, SyntaxTokensReleasedWithTextLayout)
{
    NodeLayoutEntry entry;
    entry.set_syntax_tokens({ SyntaxToken{ 0, 3, SyntaxTokenType::Keyword } });
    ASSERT_EQ(entry.syntax_tokens().size(), 1u);

    entry.release_layout_state();
    EXPECT_TRUE(entry.syntax_tokens().empty());
}

// dirty の有無はキャッシュと一緒に動く。差し替え用の新しいキャッシュは計測前なので dirty ありとして始まる。
TEST(LayoutCacheTest, HasDirtyNodesTravelsWithCache)
{
    LayoutCache cache(3);
    EXPECT_TRUE(cache.HasDirtyNodes());
    cache.SetHasDirtyNodes(false);
    LayoutCache moved = std::move(cache);
    EXPECT_FALSE(moved.HasDirtyNodes());
}

// Top はブロックごとのオフセットに分けて持つ。ブロックをまたいでずらしても位置は変わらない。
TEST(LayoutCacheTops, ShiftAcrossBlocksKeepsPositions)
{
    LayoutCache cache(10000);
    for (size_t i = 0; i < cache.size(); ++i) {
        cache.SetTop(i, static_cast<float>(i) * 2.0f);
    }
    cache.ShiftTops(3000, 5.0f);
    cache.ShiftTops(8000, -1.0f);
    const auto expected = [](size_t i) {
        return static_cast<float>(i) * 2.0f + (i >= 3000 ? 5.0f : 0.0f) + (i >= 8000 ? -1.0f : 0.0f);
    };
    for (const size_t i : { 0uz, 2999uz, 3000uz, 4095uz, 4096uz, 7999uz, 8000uz, 8191uz, 8192uz, 9999uz }) {
        EXPECT_FLOAT_EQ(cache.Top(i), expected(i)) << "i=" << i;
    }
}

// ---- 画面外のノードの高さの推定 ----

using layout::EstimateInvisibleNodeHeight;
using layout::EstimateNodeHeight;

namespace {

Node MakeParagraphNode(int line_count = 0)
{
    Node n;
    n.type = NodeType::Paragraph;
    SetTestText(n, "text", line_count);
    return n;
}

Node MakeCodeBlockNode(SyntaxLanguage lang, int line_count = 1)
{
    Node n;
    n.type = IsDiagramLanguage(lang) ? NodeType::Diagram : NodeType::CodeBlock;
    SetTestText(n, "code", line_count);
    n.code_language = lang;
    return n;
}

Node MakeTwoByTwoTableNode()
{
    Node n;
    n.type = NodeType::Table;
    auto* tbl = AttachTestTable(n);
    tbl->row_count = 2;
    tbl->col_count = 2;
    return n;
}

} // namespace

// 計測済みで幅が同じなら実測を残す。それ以外は推定まで伸ばすが、縮めはしない。図のコードブロックは高さに触らない。
TEST(EstimateInvisibleNodeHeight, GrowsToEstimateUnlessMeasuredAtSameWidth)
{
    const auto& theme = GetLightTheme();
    struct Case {
        const char* label;
        Node node;
        float height;
        float cached_width; // 0 は未計測
        float node_width;
        std::optional<float> expected; // 無ければ推定 (EstimateNodeHeight)
    };
    const Case kCases[] = {
        { "DiagramLanguageSkipped", MakeCodeBlockNode(SyntaxLanguage::Mermaid), 5.0f, 0.0f, 200.0f, 5.0f },
        { "DoesNotShrinkBelowExisting", MakeParagraphNode(/*line_count=*/0), 9999.0f, 0.0f, 400.0f, 9999.0f },
        { "GrowsToEstimateWhenSmaller", MakeParagraphNode(/*line_count=*/3), 1.0f, 0.0f, 400.0f, std::nullopt },
        { "NonDiagramCodeBlockGrows", MakeCodeBlockNode(SyntaxLanguage::Cpp, /*line_count=*/4), 0.0f, 0.0f, 400.0f, std::nullopt },
        { "KeepsMeasuredHeightWhenWidthMatches", MakeParagraphNode(/*line_count=*/20), 1.0f, 200.0f, 200.0f, 1.0f },
        { "FallsBackToEstimateWhenWidthDiffers", MakeParagraphNode(/*line_count=*/2), 1.0f, 200.0f, 800.0f, std::nullopt },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.label);
        NodeLayoutEntry entry;
        entry.height = c.height;
        if (c.cached_width > 0.0f) {
            entry.cached_width = c.cached_width;
        }
        EstimateInvisibleNodeHeight(c.node, entry, theme, c.node_width);
        EXPECT_FLOAT_EQ(entry.height, c.expected.value_or(EstimateNodeHeight(c.node, theme, 0.0f)));
    }
}

TEST(EstimateInvisibleNodeHeight, TableCacheHitPreservesTableLayout)
{
    auto node = MakeTwoByTwoTableNode();
    NodeLayoutEntry entry;
    entry.height = 200.0f;
    entry.cached_width = 300.0f;
    auto& tl = entry.set_table_layout(std::make_unique<TableLayoutData>(0, 2, false));
    tl.col_widths = { 100.0f, 200.0f };
    tl.CommitGeometry(300.0f);
    const auto& theme = GetLightTheme();
    // cache_hit = true 経路 → table_layout を保持
    EstimateInvisibleNodeHeight(node, entry, theme, 300.0f);
    EXPECT_FLOAT_EQ(entry.height, 200.0f);
    EXPECT_EQ(entry.table_layout(), &tl);
}

// 画像・図は寸法があればノード幅に収まるよう縮め、無ければ現在値、それも無ければプレースホルダー。
TEST(BitmapNodeHeight, ScalesKeepsOrFallsBack)
{
    const auto& theme = GetLightTheme();
    DiagramEntry wide;
    wide.width = 1200.0f;
    wide.height = 800.0f;
    EXPECT_FLOAT_EQ(layout::BitmapNodeHeight(&wide, 600.0f, 50.0f, theme), 400.0f);

    DiagramEntry narrow;
    narrow.width = 300.0f;
    narrow.height = 200.0f;
    EXPECT_FLOAT_EQ(layout::BitmapNodeHeight(&narrow, 600.0f, 50.0f, theme), 200.0f);

    EXPECT_FLOAT_EQ(layout::BitmapNodeHeight(nullptr, 600.0f, 50.0f, theme), 50.0f);
    EXPECT_FLOAT_EQ(layout::BitmapNodeHeight(nullptr, 600.0f, 0.0f, theme), layout::PlaceholderHeight(theme));
}

// 描けなかった図は、推定が 1 行ぶんでも文言を数行見せる高さにする。高い推定はそのまま保つ。
TEST(BitmapNodeHeight, ErrorKeepsAtLeastErrorPlaceholder)
{
    const auto& theme = GetLightTheme();
    DiagramEntry failed;
    failed.error = { DiagramError::Kind::Render, L"KaTeX parse error" };
    const float error_height = layout::ErrorPlaceholderHeight(theme);
    EXPECT_GT(error_height, layout::PlaceholderHeight(theme));
    EXPECT_FLOAT_EQ(layout::BitmapNodeHeight(&failed, 600.0f, 24.0f, theme), error_height);
    EXPECT_FLOAT_EQ(layout::BitmapNodeHeight(&failed, 600.0f, error_height + 50.0f, theme), error_height + 50.0f);
}
