#include <gtest/gtest.h>
#include "document.h"
#include "layout_cache.h"
#include "layout_computer.h"
#include "theme.h"

using mendo::layout::EstimateInvisibleNodeHeight;
using mendo::layout::EstimateNodeHeight;

namespace {

Node MakeParagraphNode(int line_count = 0)
{
    Node n;
    n.type = NodeType::Paragraph;
    n.SetText("text");
    n.line_count = line_count;
    return n;
}

Node MakeCodeBlockNode(SyntaxLanguage lang, int line_count = 1)
{
    Node n;
    n.type = NodeType::CodeBlock;
    n.SetText("code");
    n.line_count = line_count;
    n.ensure_code()->code_language = lang;
    return n;
}

Node MakeTableNode()
{
    Node n;
    n.type = NodeType::Table;
    auto* tbl = n.ensure_table();
    tbl->row_count = 2;
    tbl->col_count = 2;
    return n;
}

NodeLayoutEntry MakeEntry(float height = 10.0f, bool measured = false)
{
    NodeLayoutEntry e;
    e.height = height;
    if (measured) {
        e.cached_width = 100.0f;
        e.cached_height = height;
    }
    return e;
}

} // namespace

TEST(EstimateInvisibleNodeHeight, DiagramLanguageSkipped)
{
    auto node = MakeCodeBlockNode(SyntaxLanguage::Mermaid);
    auto entry = MakeEntry(/*height=*/5.0f);
    const auto& theme = GetLightTheme();
    const bool changed = EstimateInvisibleNodeHeight(node, entry, theme, 200.0f);
    EXPECT_FALSE(changed);
    EXPECT_FLOAT_EQ(entry.height, 5.0f); // 触らない
}

TEST(EstimateInvisibleNodeHeight, DoesNotShrinkBelowExisting)
{
    auto node = MakeParagraphNode(/*line_count=*/0);
    auto entry = MakeEntry(/*height=*/9999.0f);
    const auto& theme = GetLightTheme();
    const bool changed = EstimateInvisibleNodeHeight(node, entry, theme, 400.0f);
    EXPECT_FALSE(changed);
    EXPECT_FLOAT_EQ(entry.height, 9999.0f);
}

TEST(EstimateInvisibleNodeHeight, GrowsToEstimateWhenSmaller)
{
    auto node = MakeParagraphNode(/*line_count=*/3);
    auto entry = MakeEntry(/*height=*/1.0f);
    const auto& theme = GetLightTheme();
    const float estimated = EstimateNodeHeight(node, theme);
    const bool changed = EstimateInvisibleNodeHeight(node, entry, theme, 400.0f);
    EXPECT_TRUE(changed);
    EXPECT_FLOAT_EQ(entry.height, estimated);
}

TEST(EstimateInvisibleNodeHeight, UsesCachedHeightWhenWidthMatches)
{
    auto node = MakeParagraphNode(/*line_count=*/0);
    NodeLayoutEntry entry;
    entry.height = 1.0f;
    entry.cached_width = 200.0f;
    entry.cached_height = 77.0f; // 推定値より大きい
    const auto& theme = GetLightTheme();
    const bool changed = EstimateInvisibleNodeHeight(node, entry, theme, 200.0f);
    EXPECT_TRUE(changed);
    EXPECT_FLOAT_EQ(entry.height, 77.0f);
}

TEST(EstimateInvisibleNodeHeight, FallsBackToEstimateWhenWidthDiffers)
{
    auto node = MakeParagraphNode(/*line_count=*/2);
    NodeLayoutEntry entry;
    entry.height = 1.0f;
    entry.cached_width = 200.0f;
    entry.cached_height = 5.0f; // 推定値より小さい設定
    const auto& theme = GetLightTheme();
    const float estimated = EstimateNodeHeight(node, theme);
    // node_width が cached_width と離れているので fallback は EstimateNodeHeight
    const bool changed = EstimateInvisibleNodeHeight(node, entry, theme, 800.0f);
    EXPECT_TRUE(changed);
    EXPECT_FLOAT_EQ(entry.height, estimated);
}

TEST(EstimateInvisibleNodeHeight, TableEstimateClearsTableLayoutCache)
{
    auto node = MakeTableNode();
    NodeLayoutEntry entry;
    entry.height = 1.0f;
    auto& tl = entry.ensure_table_layout();
    tl.col_widths = { 50.0f, 50.0f };
    tl.cached_table_width = 100.0f;
    const auto& theme = GetLightTheme();
    // cache_hit = false 経路 (cached_width 未設定) → 推定で成長 + table_layout を invalidate
    const bool changed = EstimateInvisibleNodeHeight(node, entry, theme, 300.0f);
    EXPECT_TRUE(changed);
    EXPECT_TRUE(tl.col_widths.empty());
    EXPECT_FLOAT_EQ(tl.cached_table_width, 0.0f);
}

TEST(EstimateInvisibleNodeHeight, TableCacheHitPreservesTableLayout)
{
    auto node = MakeTableNode();
    NodeLayoutEntry entry;
    entry.height = 1.0f;
    entry.cached_width = 300.0f;
    entry.cached_height = 200.0f;
    auto& tl = entry.ensure_table_layout();
    tl.col_widths = { 100.0f, 200.0f };
    tl.cached_table_width = 300.0f;
    const auto& theme = GetLightTheme();
    // cache_hit = true 経路 → table_layout を保持
    const bool changed = EstimateInvisibleNodeHeight(node, entry, theme, 300.0f);
    EXPECT_TRUE(changed);
    EXPECT_FLOAT_EQ(entry.height, 200.0f);
    EXPECT_EQ(tl.col_widths.size(), 2u);
    EXPECT_FLOAT_EQ(tl.cached_table_width, 300.0f);
}

TEST(EstimateInvisibleNodeHeight, NonDiagramCodeBlockGrowsNormally)
{
    auto node = MakeCodeBlockNode(SyntaxLanguage::Cpp, /*line_count=*/4);
    auto entry = MakeEntry(/*height=*/0.0f);
    const auto& theme = GetLightTheme();
    const float estimated = EstimateNodeHeight(node, theme);
    const bool changed = EstimateInvisibleNodeHeight(node, entry, theme, 400.0f);
    EXPECT_TRUE(changed);
    EXPECT_FLOAT_EQ(entry.height, estimated);
}
