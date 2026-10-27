// hit_test の DirectWrite 経路テスト。
// MockTextMeasurer では `entry.text_layout` が nullptr になるため、
// HitTest 内の `if (entry.text_layout)` 経路や HitTestTable の cell_layout
// 経由の text_pos 計算は到達できない。本ファイルでは実 IDWriteFactory を
// 使う DWriteTestBase の上で、その経路を踏む。
#include <gtest/gtest.h>
#include "document_test_helpers.h"
#include "dwrite_test_base.h"
#include "block_h_scroll.h"
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

using layout::NodeIndent;
using layout::NodeTextXOffset;

namespace {

class HitTestDWriteTest : public DWriteTestBase {
protected:
    // dpi=1, ペイン原点 0, scroll_y=0 なので画面座標 == DIP。
    MdPaneHitContext Ctx(const ParsedLayout& pl, DipPoint p) const
    {
        return { pl.nodes, pl.cache, theme_, 0.0f, PaneRect{ 0.0f, 0.0f, 0.0f, 600.0f }, 1.0f,
                 static_cast<int>(p.x), static_cast<int>(p.y), theme_.ContentWidth(800.0f) };
    }

    // 段落ノード node の UTF-16 位置 pos の文字 (クラスタ) の、左端から幅の fraction の位置。
    DipPoint PointInChar(const ParsedLayout& pl, size_t node, uint32_t pos, float fraction) const
    {
        FLOAT x = 0.0f, y = 0.0f;
        DWRITE_HIT_TEST_METRICS m{};
        pl.cache[node].text_layout()->HitTestTextPosition(pos, FALSE, &x, &y, &m);
        const auto& n = pl.nodes[node];
        const float left = theme_.margin_left + NodeIndent(n, theme_) + NodeTextXOffset(n, theme_);
        return { left + m.left + m.width * fraction, pl.cache.Top(node) + m.top + m.height * 0.5f };
    }

    DipPoint RightHalfOfChar(const ParsedLayout& pl, size_t node, uint32_t pos) const
    {
        return PointInChar(pl, node, pos, 0.75f);
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
    const auto r = hit_test::HitTest(Ctx(pl, { theme_.margin_left + 50.0f, pl.cache.Top(0) + 4.0f }));

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

// 結合文字を含むクラスタの右半分は、クラスタ全体の後ろに置く (+1 だと e と U+0301 の間に入る)。
TEST_F(HitTestDWriteTest, RightHalfOfCombiningClusterSkipsWholeCluster)
{
    auto pl = ParseAndLayout("e\xCC\x81x tail");
    ASSERT_FALSE(pl.nodes.empty());
    const auto right = hit_test::HitTest(Ctx(pl, RightHalfOfChar(pl, 0, 0)));
    ASSERT_EQ(right.node_index, 0);
    EXPECT_EQ(right.text_pos, 3u);
    EXPECT_EQ(right.char_pos, 0u);

    FLOAT x = 0.0f, y = 0.0f;
    DWRITE_HIT_TEST_METRICS m{};
    pl.cache[0].text_layout()->HitTestTextPosition(0, FALSE, &x, &y, &m);
    ASSERT_EQ(m.length, 2u) << "e + U+0301 が 1 クラスタであること";
    EXPECT_EQ(hit_test::HitTest(Ctx(pl, PointInChar(pl, 0, 0, 0.25f))).text_pos, 0u);
}

// 保存済みの横スクロール量は幅が変わっても残るので、ヒットテストは描画と同じく clamp した値を使う。
// clamp しないと、収まるようになったブロックや末尾を越えた値でクリック位置がずれる。
TEST_F(HitTestDWriteTest, BlockScrollXIsClampedBeforeHitTest)
{
    const auto hit_with = [&](const ParsedLayout& pl, const std::pmr::unordered_map<int, float>* scroll) {
        auto ctx = Ctx(pl, { theme_.margin_left + 40.0f, pl.cache.Top(0) + pl.cache[0].height * 0.5f });
        ctx.block_scroll_x = scroll;
        return hit_test::HitTest(ctx);
    };

    auto narrow = ParseAndLayout("```\nabcdef\n```");
    ASSERT_EQ(narrow.nodes[0].type, NodeType::CodeBlock);
    ASSERT_FALSE(GetBlockHScrollGeometry(narrow.nodes, narrow.cache, 0, theme_, 800.0f).can_scroll());
    const std::pmr::unordered_map<int, float> stale{ { 0, 500.0f } };
    const auto unscrolled = hit_with(narrow, nullptr);
    const auto with_stale = hit_with(narrow, &stale);
    EXPECT_EQ(with_stale.text_pos, unscrolled.text_pos);
    EXPECT_EQ(with_stale.char_pos, unscrolled.char_pos);

    const std::string wide_md = "```\n" + std::string(300, 'a') + "\n```";
    auto wide = ParseAndLayout(wide_md);
    const auto geom = GetBlockHScrollGeometry(wide.nodes, wide.cache, 0, theme_, 800.0f);
    ASSERT_TRUE(geom.can_scroll());
    const std::pmr::unordered_map<int, float> at_max{ { 0, geom.scroll_max() } };
    const std::pmr::unordered_map<int, float> beyond{ { 0, 1.0e6f } };
    const auto r_max = hit_with(wide, &at_max);
    const auto r_beyond = hit_with(wide, &beyond);
    EXPECT_GT(r_max.text_pos, hit_with(wide, nullptr).text_pos);
    EXPECT_EQ(r_beyond.text_pos, r_max.text_pos);
    EXPECT_EQ(r_beyond.char_pos, r_max.char_pos);
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
    ASSERT_TRUE(tl->HasRowGeometry());
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

    constexpr float kPaneX = 220.0f;
    constexpr float kPaneY = 32.0f;
    const DipPoint p{ theme_.margin_left + 50.0f, pl.cache.Top(0) + 4.0f };

    const auto base = Ctx(pl, p);
    auto offset = Ctx(pl, { p.x + kPaneX, p.y + kPaneY });
    offset.md_rect.x = kPaneX;
    offset.md_rect.y = kPaneY;
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
    ASSERT_NE(pl.cache[table_idx].table_layout(), nullptr);

    // テーブル中央あたりに hit する位置。
    const auto r = hit_test::HitTest(Ctx(pl, { theme_.margin_left + 30.0f, pl.cache.Top(table_idx) + pl.cache[table_idx].height * 0.5f }));

    EXPECT_EQ(r.node_index, table_idx)
        << "テーブル領域内の click は table ノードを返すはず";
}

// OverlayButtonHitTest: Copy ボタンの上を click するとコードブロックの CodeCopy
// インデックスが入ること。共有 cache の matches も正しく動くかを軽く確認する。
TEST_F(HitTestDWriteTest, OverlayButtonHitTest_CopyHitReturnsNode)
{
    auto pl = ParseAndLayout("```\nint main() { return 0; }\n```");

    const int code_idx = FindFirstNodeIndexByType(pl.nodes, NodeType::CodeBlock);
    ASSERT_GE(code_idx, 0);

    const float content_width = theme_.ContentWidth(800.0f);
    const float block_right = theme_.margin_left + content_width;
    const float block_top = pl.cache.Top(code_idx) - theme_.code_block_padding;
    const D2D1_RECT_F btn = OverlayButtonRect(block_right, block_top, OverlayButton::CodeCopy);
    const auto ctx = Ctx(pl, { (btn.left + btn.right) * 0.5f, (btn.top + btn.bottom) * 0.5f });
    EXPECT_EQ(hit_test::OverlayButtonHitTest(ctx), (OverlayButtonHit{ code_idx, OverlayButton::CodeCopy }));
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
        ASSERT_NE(pl_.cache[idx].table_layout(), nullptr);
        const auto& tl = *pl_.cache[idx].table_layout();
        t_ = { &tl, pl_.nodes[idx].table_data()->row_count, pl_.cache[idx].height, pl_.cache.Top(static_cast<size_t>(idx)) };
        ASSERT_TRUE(tl.HasGeometry());
    }
};

// 罫線上の点は、描画でその罫線を発行しているセル (列は左罫線、行は上罫線の持ち主) に帰属する。
TEST_F(FindTableCellTest, BordersAttributedLikeDrawing)
{
    const size_t col_count = t_.tl->col_widths.size();
    ASSERT_GE(col_count, 2u);

    CommandGenerator gen;
    gen.Init(theme_);
    const auto& cmds = gen.GenerateMdPane({ .nodes = pl_.nodes, .cache = pl_.cache, .selection = TextSelection{}, .md_pane_rect = PaneRect{ 0.0f, 0.0f, 4000.0f, 2000.0f } }, 1.0f);

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
        EXPECT_EQ(FindTableRow(tl, row_lines[r]).row, static_cast<int>(r)) << "row line " << r;
        EXPECT_EQ(FindTableRow(tl, row_lines[r] - 0.25f).row, static_cast<int>(r) - 1) << "row line " << r;
    }
}
