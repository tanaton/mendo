#include <gtest/gtest.h>
#include "block_h_scroll.h"
#include "theme.h"

// ─────────────────────────────────────────────
// BlockHScrollGeometry::can_scroll / scroll_max（純粋な境界ロジック）
// ─────────────────────────────────────────────

TEST(BlockHScrollGeometry, ScrollableWhenNaturalExceedsVisible)
{
    constexpr BlockHScrollGeometry g{ .natural_width = 200.0f, .visible_width = 100.0f };
    EXPECT_TRUE(g.can_scroll());
    EXPECT_FLOAT_EQ(g.scroll_max(), 100.0f);
}

TEST(BlockHScrollGeometry, NotScrollableWhenEqual)
{
    constexpr BlockHScrollGeometry g{ .natural_width = 100.0f, .visible_width = 100.0f };
    EXPECT_FALSE(g.can_scroll());
    EXPECT_FLOAT_EQ(g.scroll_max(), 0.0f);
}

TEST(BlockHScrollGeometry, NotScrollableWhenNaturalSmaller)
{
    constexpr BlockHScrollGeometry g{ .natural_width = 50.0f, .visible_width = 100.0f };
    EXPECT_FALSE(g.can_scroll());
    EXPECT_FLOAT_EQ(g.scroll_max(), 0.0f);
}

// visible_width が 0（未レイアウト）のとき can_scroll は false。scroll_max は
// can_scroll でガードされる前提のため、ここでは natural をそのまま返す挙動を固定する。
TEST(BlockHScrollGeometry, NotScrollableWhenVisibleZero)
{
    constexpr BlockHScrollGeometry g{ .natural_width = 200.0f, .visible_width = 0.0f };
    EXPECT_FALSE(g.can_scroll());
    EXPECT_FLOAT_EQ(g.scroll_max(), 200.0f);
}

TEST(BlockHScrollGeometry, DefaultIsNotScrollable)
{
    constexpr BlockHScrollGeometry g;
    EXPECT_FALSE(g.can_scroll());
    EXPECT_FLOAT_EQ(g.scroll_max(), 0.0f);
}

// ─────────────────────────────────────────────
// GetBlockHScrollGeometry: ノード種別ごとの natural_width 選択 / visible_width 計算
// ─────────────────────────────────────────────

// CodeBlock（diagram でない）は natural_code_width を採用する。
TEST(GetBlockHScrollGeometry, ScrollableCodeBlockUsesNaturalCodeWidth)
{
    const Theme theme = GetLightTheme();
    Node node;
    node.type = NodeType::CodeBlock; // code_language()==None → IsScrollableCodeBlock
    NodeLayoutEntry entry;
    entry.natural_code_width = 1234.0f;

    constexpr float kPaneWidth = 800.0f;
    const auto g = GetBlockHScrollGeometry(node, entry, theme, kPaneWidth);
    EXPECT_FLOAT_EQ(g.natural_width, 1234.0f);
    EXPECT_FLOAT_EQ(g.visible_width, theme.ContentWidth(kPaneWidth));
}

// Table は table_layout->natural_total_width を採用する。
TEST(GetBlockHScrollGeometry, TableUsesNaturalTotalWidth)
{
    const Theme theme = GetLightTheme();
    Node node;
    node.type = NodeType::Table;
    NodeLayoutEntry entry;
    entry.ensure_table_layout().natural_total_width = 2048.0f;

    const auto g = GetBlockHScrollGeometry(node, entry, theme, 800.0f);
    EXPECT_FLOAT_EQ(g.natural_width, 2048.0f);
}

// table_layout 未確保（未計測）の Table は natural_width=0 でスクロール不可。
TEST(GetBlockHScrollGeometry, TableWithoutLayoutHasZeroNatural)
{
    const Theme theme = GetLightTheme();
    Node node;
    node.type = NodeType::Table;
    NodeLayoutEntry entry;

    const auto g = GetBlockHScrollGeometry(node, entry, theme, 800.0f);
    EXPECT_FLOAT_EQ(g.natural_width, 0.0f);
    EXPECT_FALSE(g.can_scroll());
}

// 段落はスクロール非対象。natural_code_width をセットしても無視される。
TEST(GetBlockHScrollGeometry, ParagraphIsNotScrollable)
{
    const Theme theme = GetLightTheme();
    Node node;
    node.type = NodeType::Paragraph;
    NodeLayoutEntry entry;
    entry.natural_code_width = 9999.0f;

    const auto g = GetBlockHScrollGeometry(node, entry, theme, 800.0f);
    EXPECT_FLOAT_EQ(g.natural_width, 0.0f);
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
    entry.natural_code_width = 1000.0f;

    constexpr float kPaneWidth = 800.0f;
    const auto g = GetBlockHScrollGeometry(node, entry, theme, kPaneWidth);
    EXPECT_FLOAT_EQ(g.visible_width, theme.ContentWidth(kPaneWidth) - 2 * theme.indent_width);
}
