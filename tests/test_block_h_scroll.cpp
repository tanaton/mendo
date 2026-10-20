#include <gtest/gtest.h>
#include "block_h_scroll.h"
#include "theme.h"

// ─────────────────────────────────────────────
// BlockHScrollGeometry::can_scroll / scroll_max（純粋な境界ロジック）
// ─────────────────────────────────────────────

TEST(BlockHScrollGeometry, ScrollableWhenBlockExceedsVisible)
{
    constexpr BlockHScrollGeometry g{ .block_width = 200.0f, .visible_width = 100.0f };
    EXPECT_TRUE(g.can_scroll());
    EXPECT_FLOAT_EQ(g.scroll_max(), 100.0f);
}

TEST(BlockHScrollGeometry, NotScrollableWhenEqual)
{
    constexpr BlockHScrollGeometry g{ .block_width = 100.0f, .visible_width = 100.0f };
    EXPECT_FALSE(g.can_scroll());
    EXPECT_FLOAT_EQ(g.scroll_max(), 0.0f);
}

TEST(BlockHScrollGeometry, NotScrollableWhenBlockSmaller)
{
    constexpr BlockHScrollGeometry g{ .block_width = 50.0f, .visible_width = 100.0f };
    EXPECT_FALSE(g.can_scroll());
    EXPECT_FLOAT_EQ(g.scroll_max(), 0.0f);
}

// visible_width が 0（未レイアウト）のときはスクロールさせない。
TEST(BlockHScrollGeometry, NotScrollableWhenVisibleZero)
{
    constexpr BlockHScrollGeometry g{ .block_width = 200.0f, .visible_width = 0.0f };
    EXPECT_FALSE(g.can_scroll());
    EXPECT_FLOAT_EQ(g.scroll_max(), 0.0f);
}

// BLOCK_H_SCROLL_SLACK 以下のはみ出しではスクロールさせない。
TEST(BlockHScrollGeometry, OverflowWithinSlackIsNotScrollable)
{
    constexpr BlockHScrollGeometry g{ .block_width = 100.0f + BLOCK_H_SCROLL_SLACK, .visible_width = 100.0f };
    EXPECT_FALSE(g.can_scroll());
    EXPECT_FLOAT_EQ(g.scroll_max(), 0.0f);
    EXPECT_FLOAT_EQ(g.ClampScrollX(0.3f), 0.0f);
}

TEST(BlockHScrollGeometry, OverflowBeyondSlackScrollsByFullOverflow)
{
    constexpr BlockHScrollGeometry g{ .block_width = 101.0f, .visible_width = 100.0f };
    EXPECT_TRUE(g.can_scroll());
    EXPECT_FLOAT_EQ(g.scroll_max(), 1.0f);
}

TEST(BlockHScrollGeometry, DefaultIsNotScrollable)
{
    constexpr BlockHScrollGeometry g;
    EXPECT_FALSE(g.can_scroll());
    EXPECT_FLOAT_EQ(g.scroll_max(), 0.0f);
}

// ─────────────────────────────────────────────
// GetBlockHScrollGeometry: ノード種別ごとの block_width 選択 / visible_width 計算
// ─────────────────────────────────────────────

// CodeBlock（diagram でない）は natural_code_width を採用する。
TEST(GetBlockHScrollGeometry, ScrollableCodeBlockUsesNaturalCodeWidth)
{
    const Theme theme = GetLightTheme();
    Node node;
    node.type = NodeType::CodeBlock;
    NodeLayoutEntry entry;
    entry.set_natural_code_width(1234.0f);

    constexpr float kPaneWidth = 800.0f;
    const auto g = GetBlockHScrollGeometry(node, entry, theme, kPaneWidth);
    EXPECT_FLOAT_EQ(g.block_width, 1234.0f);
    EXPECT_FLOAT_EQ(g.visible_width, theme.ContentWidth(kPaneWidth));
}

// Table は描画と同じ TableWidth (配置後の列幅合計) を採用する。幾何を無効化した表は描画されないので、スクロールもさせない。
TEST(GetBlockHScrollGeometry, TableUsesLaidOutTableWidth)
{
    const Theme theme = GetLightTheme();
    Node node;
    node.type = NodeType::Table;
    NodeLayoutEntry entry;
    auto& tl = entry.ensure_table_layout();
    tl.col_widths = { 1000.0f, 1000.0f };
    tl.RebuildColCumX();

    const auto g = GetBlockHScrollGeometry(node, entry, theme, 800.0f);
    EXPECT_FLOAT_EQ(g.block_width, tl.TableWidth());
    EXPECT_TRUE(g.can_scroll());

    tl.InvalidateGeometry();
    const auto invalidated = GetBlockHScrollGeometry(node, entry, theme, 800.0f);
    EXPECT_FLOAT_EQ(invalidated.block_width, 0.0f);
    EXPECT_FALSE(invalidated.can_scroll());
}

// table_layout 未確保（未計測）の Table は block_width=0 でスクロール不可。
TEST(GetBlockHScrollGeometry, TableWithoutLayoutHasZeroWidth)
{
    const Theme theme = GetLightTheme();
    Node node;
    node.type = NodeType::Table;
    NodeLayoutEntry entry;

    const auto g = GetBlockHScrollGeometry(node, entry, theme, 800.0f);
    EXPECT_FLOAT_EQ(g.block_width, 0.0f);
    EXPECT_FALSE(g.can_scroll());
}

// 段落はスクロール非対象。natural_code_width をセットしても無視される。
TEST(GetBlockHScrollGeometry, ParagraphIsNotScrollable)
{
    const Theme theme = GetLightTheme();
    Node node;
    node.type = NodeType::Paragraph;
    NodeLayoutEntry entry;
    entry.set_natural_code_width(9999.0f);

    const auto g = GetBlockHScrollGeometry(node, entry, theme, 800.0f);
    EXPECT_FLOAT_EQ(g.block_width, 0.0f);
    EXPECT_FALSE(g.can_scroll());
}

// indent_level の分だけ visible_width が縮む（ネストしたコードブロック）。
TEST(GetBlockHScrollGeometry, IndentReducesVisibleWidth)
{
    const Theme theme = GetLightTheme();
    Node node;
    node.type = NodeType::CodeBlock;
    node.indent_level = 2;
    NodeLayoutEntry entry;
    entry.set_natural_code_width(1000.0f);

    constexpr float kPaneWidth = 800.0f;
    const auto g = GetBlockHScrollGeometry(node, entry, theme, kPaneWidth);
    EXPECT_FLOAT_EQ(g.visible_width, theme.ContentWidth(kPaneWidth) - 2 * theme.indent_width);
}

// ─────────────────────────────────────────────
// BlockHScrollGeometry::ClampScrollX
// ─────────────────────────────────────────────

TEST(BlockHScrollGeometry, ClampScrollXClampsToScrollableRange)
{
    constexpr BlockHScrollGeometry g{ .block_width = 300.0f, .visible_width = 100.0f };
    EXPECT_FLOAT_EQ(g.ClampScrollX(-10.0f), 0.0f);
    EXPECT_FLOAT_EQ(g.ClampScrollX(50.0f), 50.0f);
    EXPECT_FLOAT_EQ(g.ClampScrollX(500.0f), 200.0f);
}

// 幅が広がって収まるようになったブロックは、以前のスクロール位置を残さない。
TEST(BlockHScrollGeometry, ClampScrollXIsZeroWhenNotScrollable)
{
    constexpr BlockHScrollGeometry g{ .block_width = 100.0f, .visible_width = 300.0f };
    EXPECT_FLOAT_EQ(g.ClampScrollX(80.0f), 0.0f);
}
