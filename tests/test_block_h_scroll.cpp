#include <gtest/gtest.h>
#include <string>
#include "document_test_helpers.h"
#include "test_nodes.h"
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

namespace {

// ノード 1 つの文書と、その計測結果。
struct OneNodeDoc {
    std::vector<Node> nodes;
    LayoutCache cache;

    explicit OneNodeDoc(NodeType type, int8_t indent_level = 0)
    {
        Node node;
        node.type = type;
        node.indent_level = indent_level;
        nodes.push_back(node);
        cache = LayoutCache(1);
    }
    NodeLayoutEntry& entry()
    {
        return cache[0];
    }
    BlockHScrollGeometry Geometry(const Theme& theme, float md_pane_width) const
    {
        return GetBlockHScrollGeometry(nodes, cache, 0, theme, md_pane_width);
    }
};

} // namespace

// CodeBlock（diagram でない）は natural_code_width を採用する。
TEST(GetBlockHScrollGeometry, ScrollableCodeBlockUsesNaturalCodeWidth)
{
    const Theme theme = GetLightTheme();
    OneNodeDoc doc{ NodeType::CodeBlock };
    doc.entry().set_natural_code_width(1234.0f);

    constexpr float kPaneWidth = 800.0f;
    const auto g = doc.Geometry(theme, kPaneWidth);
    EXPECT_FLOAT_EQ(g.block_width, 1234.0f);
    EXPECT_FLOAT_EQ(g.visible_width, theme.ContentWidth(kPaneWidth));
    EXPECT_EQ(g.scroll_key, 0);
}

// Table は描画と同じ TableWidth (配置後の列幅合計) を採用する。
TEST(GetBlockHScrollGeometry, TableUsesLaidOutTableWidth)
{
    const Theme theme = GetLightTheme();
    OneNodeDoc doc{ NodeType::Table };
    auto& tl = doc.entry().set_table_layout(std::make_unique<TableLayoutData>(0, 2, false));
    tl.col_widths = { 1000.0f, 1000.0f };
    tl.CommitGeometry(2000.0f);

    const auto g = doc.Geometry(theme, 800.0f);
    EXPECT_FLOAT_EQ(g.block_width, tl.TableWidth());
    EXPECT_TRUE(g.can_scroll());
}

// table_layout 未確保（未計測）の Table は block_width=0 でスクロール不可。
TEST(GetBlockHScrollGeometry, TableWithoutLayoutHasZeroWidth)
{
    const Theme theme = GetLightTheme();
    const OneNodeDoc doc{ NodeType::Table };

    const auto g = doc.Geometry(theme, 800.0f);
    EXPECT_FLOAT_EQ(g.block_width, 0.0f);
    EXPECT_FALSE(g.can_scroll());
}

// 段落はスクロール非対象。natural_code_width をセットしても無視される。
TEST(GetBlockHScrollGeometry, ParagraphIsNotScrollable)
{
    const Theme theme = GetLightTheme();
    OneNodeDoc doc{ NodeType::Paragraph };
    doc.entry().set_natural_code_width(9999.0f);

    const auto g = doc.Geometry(theme, 800.0f);
    EXPECT_FLOAT_EQ(g.block_width, 0.0f);
    EXPECT_FALSE(g.can_scroll());
}

// indent_level の分だけ visible_width が縮む（ネストしたコードブロック）。
TEST(GetBlockHScrollGeometry, IndentReducesVisibleWidth)
{
    const Theme theme = GetLightTheme();
    OneNodeDoc doc{ NodeType::CodeBlock, 2 };
    doc.entry().set_natural_code_width(1000.0f);

    constexpr float kPaneWidth = 800.0f;
    const auto g = doc.Geometry(theme, kPaneWidth);
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

// 区切ったコードブロックは並び全体で横スクロールする。スクロール量の鍵は先頭で、幅は並びの最大。
TEST(GetBlockHScrollGeometry, SplitCodeBlockSharesKeyAndWidestChunk)
{
    const std::string md = MakeCodeBlockMd(5000);
    const auto nodes = ParseNodes(md);
    ASSERT_GT(nodes.size(), 2u);
    LayoutCache cache(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        cache[i].set_natural_code_width(i == 1 ? 500.0f : 100.0f);
    }
    for (size_t i = 0; i < nodes.size(); ++i) {
        SCOPED_TRACE(i);
        const auto g = GetBlockHScrollGeometry(nodes, cache, i, 300.0f);
        EXPECT_EQ(g.scroll_key, 0);
        EXPECT_FLOAT_EQ(g.block_width, 500.0f);
    }
}
