// HitTestService の DirectWrite 経路テスト。
// MockTextMeasurer では `entry.text_layout` が nullptr になるため、
// HitTest 内の `if (entry.text_layout)` 経路や HitTestTable の cell_layout
// 経由の text_pos 計算は到達できない。本ファイルでは実 IDWriteFactory を
// 使う DWriteTestBase の上で、その経路を踏む。
#include <gtest/gtest.h>
#include "document_test_helpers.h"
#include "dwrite_test_base.h"
#include "command_generator.h"
#include "hit_test_service.h"
#include "ui_constants.h"
#include <algorithm>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

class HitTestDWriteTest : public DWriteTestBase {
protected:
    HitTestService hit_;
};

} // namespace

// 段落の text_layout 経路: paragraph 上の中央付近をクリックすると、当該ノードと
// 0 でない text_pos が返される。MockTextMeasurer 経路では text_pos = 0 のまま
// 返されてしまうため、DirectWrite を通したことの直接的な検証になる。
TEST_F(HitTestDWriteTest, ParagraphHitReturnsTextPositionFromTextLayout)
{
    auto pl = ParseAndLayout("Hello, this is a long paragraph for hit testing.");
    ASSERT_FALSE(pl.nodes.empty());

    // 段落の中央付近 (margin_left + 数文字分) をクリックする想定。
    // dpi=1, md_pane_left=0, scroll_y=0 で screen 座標 == DIP。
    const float content_width = theme_.ContentWidth(800.0f);
    const int screen_x = static_cast<int>(theme_.margin_left + 50.0f);
    const int screen_y = static_cast<int>(pl.cache.Top(0) + 4.0f);

    const MdPaneHitContext ctx{
        pl.nodes, pl.cache, theme_, 0.0f, PaneRect{ 0.0f, 0.0f, 0.0f, 600.0f }, 1.0f,
        screen_x, screen_y, content_width
    };
    const auto r = hit_.HitTest(ctx);

    EXPECT_EQ(r.node_index, 0);
    EXPECT_GT(r.text_pos, 0u)
        << "DirectWrite 経路では HitTestPoint で text_pos が 0 より大きく解決されるはず";
}

// ペイン原点 (md_rect.x/y) だけずらした同じ点は、同じノード・同じ文字位置に当たる。
TEST_F(HitTestDWriteTest, OffsetPaneHitMatchesUnoffsetPane)
{
    auto pl = ParseAndLayout("Hello, this is a long paragraph for hit testing.");
    ASSERT_FALSE(pl.nodes.empty());

    const float content_width = theme_.ContentWidth(800.0f);
    const int screen_x = static_cast<int>(theme_.margin_left + 50.0f);
    const int screen_y = static_cast<int>(pl.cache.Top(0) + 4.0f);
    constexpr int kPaneX = 220;
    constexpr int kPaneY = 32;

    const MdPaneHitContext base{
        pl.nodes, pl.cache, theme_, 0.0f, PaneRect{ 0.0f, 0.0f, 0.0f, 600.0f }, 1.0f,
        screen_x, screen_y, content_width
    };
    const MdPaneHitContext offset{
        pl.nodes, pl.cache, theme_, 0.0f, PaneRect{ static_cast<float>(kPaneX), static_cast<float>(kPaneY), 0.0f, 600.0f }, 1.0f,
        screen_x + kPaneX, screen_y + kPaneY, content_width
    };
    const auto a = hit_.HitTest(base);
    const auto b = hit_.HitTest(offset);
    EXPECT_EQ(a.node_index, b.node_index);
    EXPECT_EQ(a.text_pos, b.text_pos);
}

// テーブル row_cum_y / col_cum_x 経由の hit test。
// LayoutEngine + DWriteTextMeasurer を通した cache の table_layout には
// row_cum_y / col_cum_x が積まれる。HitTestTable はそれを upper_bound で参照する。
TEST_F(HitTestDWriteTest, TableHitDetectsRowAndColumn)
{
    auto pl = ParseAndLayout("| H1 | H2 |\n|---|---|\n| 11 | 12 |\n| 21 | 22 |");

    const int table_idx = FindFirstNodeIndexByType(pl.nodes, NodeType::Table);
    ASSERT_GE(table_idx, 0);
    ASSERT_TRUE(pl.cache[table_idx].has_table_layout());

    // テーブル中央あたりに hit する位置。
    const auto& entry = pl.cache[table_idx];
    const int sx = static_cast<int>(theme_.margin_left + 30.0f);
    const int sy = static_cast<int>(pl.cache.Top(table_idx) + entry.height * 0.5f);

    const float content_width = theme_.ContentWidth(800.0f);
    const MdPaneHitContext ctx{
        pl.nodes, pl.cache, theme_, 0.0f, PaneRect{ 0.0f, 0.0f, 0.0f, 600.0f }, 1.0f,
        sx, sy, content_width
    };
    const auto r = hit_.HitTest(ctx);

    EXPECT_EQ(r.node_index, table_idx)
        << "テーブル領域内の click は table ノードを返すはず";
}

// CodeBlockButtonsHitTest: Copy ボタンの上を click すると copy_node にコードブロック
// インデックスが入ること。共有 cache の matches も正しく動くかを軽く確認する。
TEST_F(HitTestDWriteTest, CodeBlockButtonsHitTest_CopyHitReturnsNode)
{
    auto pl = ParseAndLayout("```\nint main() { return 0; }\n```");

    const int code_idx = FindFirstNodeIndexByType(pl.nodes, NodeType::CodeBlock);
    ASSERT_GE(code_idx, 0);

    const float content_width = theme_.ContentWidth(800.0f);
    const float block_right = theme_.margin_left + content_width;
    const float block_top = pl.cache.Top(code_idx) - theme_.code_block_padding;
    const D2D1_RECT_F btn = OverlayButtonRect(block_right, block_top);
    const int sx = static_cast<int>((btn.left + btn.right) * 0.5f);
    const int sy = static_cast<int>((btn.top + btn.bottom) * 0.5f);

    const MdPaneHitContext ctx{
        pl.nodes, pl.cache, theme_, 0.0f, PaneRect{ 0.0f, 0.0f, 0.0f, 600.0f }, 1.0f,
        sx, sy, content_width
    };
    const auto hits = hit_.CodeBlockButtonsHitTest(ctx);
    EXPECT_EQ(hits.copy_node, code_idx);
    EXPECT_EQ(hits.save_node, -1);
    EXPECT_EQ(hits.diagram_copy_node, -1);
}

// ---- FindTableRow / FindTableCol: 累積配列経路と線形フォールバックの一致 ----

namespace {

// 列幅がばらけ、折り返しで行高も揃わないテーブル。
constexpr std::string_view VARIED_TABLE_MD =
    "| a | header two | h3 |\n"
    "|---|---|---|\n"
    "| x | wrapped cell text that should span several lines in a narrow pane | 1 |\n"
    "| yy | z | 22 |\n"
    "| q | r | longer third column value |";
constexpr float VARIED_TABLE_VIEWPORT_W = 420.0f;

struct VariedTable {
    const TableLayoutData* tl;
    size_t row_count;
    float height;
    float top;
};

} // namespace

class FindTableCellTest : public HitTestDWriteTest {
protected:
    ParsedLayout pl_;
    VariedTable t_{};
    // 累積配列 (row_cum_y / col_cum_x) を落として線形フォールバックに落とした複製。
    TableLayoutData linear_;
    float fallback_row_h_ = 0.0f;

    void SetUp() override
    {
        ASSERT_NO_FATAL_FAILURE(HitTestDWriteTest::SetUp());
        pl_ = ParseAndLayout(VARIED_TABLE_MD, VARIED_TABLE_VIEWPORT_W);
        const int idx = FindFirstNodeIndexByType(pl_.nodes, NodeType::Table);
        ASSERT_GE(idx, 0);
        ASSERT_TRUE(pl_.cache[idx].has_table_layout());
        const auto& tl = *pl_.cache[idx].table_layout;
        t_ = { &tl, pl_.nodes[idx].table_data()->row_count, pl_.cache[idx].height, pl_.cache.Top(static_cast<size_t>(idx)) };
        ASSERT_TRUE(tl.HasRowGeometry(t_.row_count));
        ASSERT_EQ(tl.col_cum_x.size(), tl.col_widths.size() + 1);
        linear_ = tl;
        linear_.row_cum_y.clear();
        linear_.col_cum_x.clear();
        fallback_row_h_ = theme_.font_size_body * TABLE_ROW_HEIGHT_FACTOR;
    }
};

TEST_F(FindTableCellTest, CumulativeAndLinearPathsAgree)
{
    for (float y = -2.0f; y <= t_.height + 2.0f; y += 0.25f) {
        const auto a = FindTableRow(*t_.tl, t_.row_count, fallback_row_h_, y);
        const auto b = FindTableRow(linear_, t_.row_count, fallback_row_h_, y);
        ASSERT_EQ(a.row, b.row) << "local_y=" << y;
        if (a.row >= 0) {
            ASSERT_FLOAT_EQ(a.row_top, b.row_top) << "local_y=" << y;
        }
    }
    for (float x = -4.0f; x <= t_.tl->cached_table_width + 4.0f; x += 0.25f) {
        const auto a = FindTableCol(*t_.tl, x);
        const auto b = FindTableCol(linear_, x);
        ASSERT_EQ(a.col, b.col) << "local_x=" << x;
        ASSERT_FLOAT_EQ(a.cell_left, b.cell_left) << "local_x=" << x;
    }
}

// 罫線上の点は、描画でその罫線を発行しているセル (列は左罫線、行は上罫線の持ち主) に帰属する。
TEST_F(FindTableCellTest, BordersAttributedLikeDrawing)
{
    const size_t col_count = t_.tl->col_widths.size();
    ASSERT_GE(col_count, 2u);

    CommandGenerator gen;
    gen.SetTheme(&theme_);
    gen.SetFormats({});
    const auto& cmds = gen.GenerateMdPane(pl_.nodes, pl_.cache, PaneRect{ 0.0f, 0.0f, 4000.0f, 2000.0f }, 0.0f, TextSelection{});

    std::vector<float> col_lines;
    std::vector<float> row_lines;
    for (const auto& c : cmds) {
        const auto* l = std::get_if<DrawLineCmd>(&c);
        if (!l || l->brush_id != BrushId::Hr) {
            continue;
        }
        if (l->p0.x == l->p1.x) {
            col_lines.push_back(l->p0.x - theme_.margin_left);
        }
        else if (l->p0.y == l->p1.y) {
            row_lines.push_back(l->p0.y - t_.top);
        }
    }
    std::ranges::sort(col_lines);
    col_lines.erase(std::ranges::unique(col_lines).begin(), col_lines.end());
    std::ranges::sort(row_lines);
    row_lines.erase(std::ranges::unique(row_lines).begin(), row_lines.end());
    // 末尾は右端/下端の外枠で、どのセルの持ち物でもない。
    ASSERT_EQ(col_lines.size(), col_count + 1);
    ASSERT_EQ(row_lines.size(), t_.row_count + 1);

    for (const auto* data : { t_.tl, static_cast<const TableLayoutData*>(&linear_) }) {
        SCOPED_TRACE(data == t_.tl ? "cumulative" : "linear");
        for (size_t c = 0; c < col_count; c++) {
            EXPECT_EQ(FindTableCol(*data, col_lines[c]).col, static_cast<int>(c)) << "col line " << c;
            if (c > 0) {
                EXPECT_EQ(FindTableCol(*data, col_lines[c] - 0.25f).col, static_cast<int>(c) - 1) << "col line " << c;
            }
        }
        for (size_t r = 0; r < t_.row_count; r++) {
            EXPECT_EQ(FindTableRow(*data, t_.row_count, fallback_row_h_, row_lines[r]).row, static_cast<int>(r)) << "row line " << r;
            EXPECT_EQ(FindTableRow(*data, t_.row_count, fallback_row_h_, row_lines[r] - 0.25f).row, static_cast<int>(r) - 1) << "row line " << r;
        }
    }
}
