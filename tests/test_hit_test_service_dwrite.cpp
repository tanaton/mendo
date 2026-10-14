// hit_test の DirectWrite 経路テスト。
// MockTextMeasurer では `entry.text_layout` が nullptr になるため、
// HitTest 内の `if (entry.text_layout)` 経路や HitTestTable の cell_layout
// 経由の text_pos 計算は到達できない。本ファイルでは実 IDWriteFactory を
// 使う DWriteTestBase の上で、その経路を踏む。
#include <gtest/gtest.h>
#include "document_test_helpers.h"
#include "dwrite_test_base.h"
#include "command_generator.h"
#include "document_utils.h"
#include "hit_test_service.h"
#include "layout_computer.h"
#include "selection_html.h"
#include "ui_constants.h"
#include <algorithm>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using mendo::layout::NodeIndent;
using mendo::layout::NodeTextXOffset;

namespace {

class HitTestDWriteTest : public DWriteTestBase {
protected:

    // dpi=1, ペイン原点 0, scroll_y=0 なので画面座標 == DIP。
    MdPaneHitContext Ctx(const ParsedLayout& pl, DipPoint p) const
    {
        return { pl.nodes, pl.cache, theme_, 0.0f, PaneRect{ 0.0f, 0.0f, 0.0f, 600.0f }, 1.0f,
                 static_cast<int>(p.x), static_cast<int>(p.y), theme_.ContentWidth(800.0f) };
    }

    // 段落ノード node の UTF-16 位置 pos の文字の右寄り (幅の 3/4)。
    DipPoint RightHalfOfChar(const ParsedLayout& pl, size_t node, uint32_t pos) const
    {
        FLOAT x = 0.0f, y = 0.0f;
        DWRITE_HIT_TEST_METRICS m{};
        pl.cache[node].text_layout()->HitTestTextPosition(pos, FALSE, &x, &y, &m);
        const auto& n = pl.nodes[node];
        const float left = theme_.margin_left + NodeIndent(n, theme_) + NodeTextXOffset(n, theme_);
        return { left + m.left + m.width * 0.75f, pl.cache.Top(node) + m.top + m.height * 0.5f };
    }
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
    const auto r = hit_test::HitTest(ctx);

    EXPECT_EQ(r.node_index, 0);
    EXPECT_GT(r.text_pos, 0u)
        << "DirectWrite 経路では HitTestPoint で text_pos が 0 より大きく解決されるはず";
}

// 1 文字だけのリンク "L" の右半分。キャレット位置は文字の後ろ (リンクの外) になるが、リンクとして当たること。
TEST_F(HitTestDWriteTest, RightHalfOfLastLinkCharHitsLink)
{
    auto pl = ParseAndLayout("aaa [L](https://l.example) tail");
    ASSERT_FALSE(pl.nodes.empty());
    const auto& node = pl.nodes[0];
    const auto r = hit_test::HitTest(Ctx(pl, RightHalfOfChar(pl, 0, 4)));

    ASSERT_EQ(r.node_index, 0);
    EXPECT_EQ(r.text_pos, 5u);
    EXPECT_EQ(r.char_pos, 4u);
    EXPECT_EQ(FindLinkAtPosition(node, r.char_pos), "https://l.example");
}

// 単語末尾の文字の右半分でも、キャレット位置 (直後の空白) ではなくその単語を選ぶ。
TEST_F(HitTestDWriteTest, RightHalfOfLastWordCharSelectsWord)
{
    auto pl = ParseAndLayout("hello world");
    ASSERT_FALSE(pl.nodes.empty());
    const auto r = hit_test::HitTest(Ctx(pl, RightHalfOfChar(pl, 0, 4)));

    ASSERT_EQ(r.char_pos, 4u);
    const auto wb = FindWordBoundaries(pl.nodes[0].GetText(), r.char_pos);
    EXPECT_TRUE(wb.found);
    EXPECT_EQ(wb.start, 0u);
    EXPECT_EQ(wb.end, 5u);
}

// 行末より右の余白は文字の上ではないので、行末のリンクにも当たらない。
TEST_F(HitTestDWriteTest, RightOfLineEndHasNoChar)
{
    auto pl = ParseAndLayout("aaa [L](https://l.example)");
    ASSERT_FALSE(pl.nodes.empty());
    const auto r = hit_test::HitTest(Ctx(pl, { theme_.margin_left + 400.0f, pl.cache.Top(0) + 4.0f }));
    ASSERT_EQ(r.node_index, 0);
    EXPECT_EQ(r.text_pos, pl.nodes[0].GetText().size());
    EXPECT_EQ(r.char_pos, hit_test::HitResult::kNoChar);
}

TEST_F(HitTestDWriteTest, RightHalfOfLastLinkCharInTableCellHitsLink)
{
    auto pl = ParseAndLayout("| h |\n|---|\n| [L](https://l.example) |");
    const int idx = FindFirstNodeIndexByType(pl.nodes, NodeType::Table);
    ASSERT_GE(idx, 0);
    const auto& node = pl.nodes[static_cast<size_t>(idx)];
    const auto* tl = pl.cache[idx].table_layout();
    ASSERT_NE(tl, nullptr);
    ASSERT_TRUE(tl->HasRowGeometry(node.table_data()->row_count));
    IDWriteTextLayout* cell = tl->GetCellLayout(1, 0);
    ASSERT_NE(cell, nullptr);
    FLOAT x = 0.0f, y = 0.0f;
    DWRITE_HIT_TEST_METRICS m{};
    ASSERT_HRESULT_SUCCEEDED(cell->HitTestTextPosition(0, FALSE, &x, &y, &m));
    const float text_x = theme_.margin_left + NodeIndent(node, theme_) + tl->col_cum_x[0] + TABLE_CELL_PADDING;
    const float text_y = pl.cache.Top(static_cast<size_t>(idx)) + tl->row_cum_y[1] + TABLE_CELL_PADDING;

    const auto r = hit_test::HitTest(Ctx(pl, { text_x + m.left + m.width * 0.75f, text_y + m.top + m.height * 0.5f }));

    ASSERT_EQ(r.node_index, idx);
    const uint32_t cell_start = node.table_data()->CellTextStart(1, 0);
    EXPECT_EQ(r.text_pos, cell_start + 1);
    EXPECT_EQ(r.char_pos, cell_start);
    EXPECT_EQ(FindLinkAtPosition(node, r.char_pos), "https://l.example");
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
    const auto a = hit_test::HitTest(base);
    const auto b = hit_test::HitTest(offset);
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
    const auto r = hit_test::HitTest(ctx);

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
    const auto hits = hit_test::CodeBlockButtonsHitTest(ctx);
    EXPECT_EQ(hits.copy_node, code_idx);
    EXPECT_EQ(hits.save_node, -1);
    EXPECT_EQ(hits.diagram_copy_node, -1);
}

// ---- FindTableRow / FindTableCol ----

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

    void SetUp() override
    {
        ASSERT_NO_FATAL_FAILURE(HitTestDWriteTest::SetUp());
        pl_ = ParseAndLayout(VARIED_TABLE_MD, VARIED_TABLE_VIEWPORT_W);
        const int idx = FindFirstNodeIndexByType(pl_.nodes, NodeType::Table);
        ASSERT_GE(idx, 0);
        ASSERT_TRUE(pl_.cache[idx].has_table_layout());
        const auto& tl = *pl_.cache[idx].table_layout();
        t_ = { &tl, pl_.nodes[idx].table_data()->row_count, pl_.cache[idx].height, pl_.cache.Top(static_cast<size_t>(idx)) };
        ASSERT_TRUE(tl.HasGeometry(t_.row_count, tl.col_count));
    }
};

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

    const auto& tl = *t_.tl;
    for (size_t c = 0; c < col_count; c++) {
        EXPECT_EQ(FindTableCol(tl, col_lines[c]).col, static_cast<int>(c)) << "col line " << c;
        if (c > 0) {
            EXPECT_EQ(FindTableCol(tl, col_lines[c] - 0.25f).col, static_cast<int>(c) - 1) << "col line " << c;
        }
    }
    for (size_t r = 0; r < t_.row_count; r++) {
        EXPECT_EQ(FindTableRow(tl, t_.row_count, row_lines[r]).row, static_cast<int>(r)) << "row line " << r;
        EXPECT_EQ(FindTableRow(tl, t_.row_count, row_lines[r] - 0.25f).row, static_cast<int>(r) - 1) << "row line " << r;
    }
}
