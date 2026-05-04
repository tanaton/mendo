// HitTestService の DirectWrite 経路テスト。
// MockTextMeasurer では `entry.text_layout` が nullptr になるため、
// HitTest 内の `if (entry.text_layout)` 経路や HitTestTable の cell_layout
// 経由の text_pos 計算は到達できない。本ファイルでは実 IDWriteFactory を
// 使う DWriteTestBase の上で、その経路を踏む。
#include <gtest/gtest.h>
#include "dwrite_test_base.h"
#include "hit_test_service.h"
#include "ui_constants.h"
#include <utility>

namespace {

class HitTestDWriteTest : public DWriteTestBase {
protected:
    HitTestService hit_;

    float ContentWidth(float pane_w) const noexcept
    {
        return pane_w - theme_.margin_left - theme_.margin_right;
    }
};

} // namespace

// 段落の text_layout 経路: paragraph 上の中央付近をクリックすると、当該ノードと
// 0 でない text_pos が返される。MockTextMeasurer 経路では text_pos = 0 のまま
// 返されてしまうため、DirectWrite を通したことの直接的な検証になる。
TEST_F(HitTestDWriteTest, ParagraphHitReturnsTextPositionFromTextLayout)
{
    auto pl = ParseAndLayout(L"Hello, this is a long paragraph for hit testing.");
    ASSERT_FALSE(pl.nodes.empty());

    // 段落の中央付近 (margin_left + 数文字分) をクリックする想定。
    // dpi=1, md_pane_left=0, scroll_y=0 で screen 座標 == DIP。
    const float content_width = ContentWidth(800.0f);
    const int screen_x = static_cast<int>(theme_.margin_left + 50.0f);
    const int screen_y = static_cast<int>(pl.cache[0].text_top + 4.0f);

    const MdPaneHitContext ctx{
        pl.nodes, pl.cache, theme_, 0.0f, 0.0f, 1.0f,
        screen_x, screen_y, content_width, 600.0f
    };
    const auto r = hit_.HitTest(ctx);

    EXPECT_EQ(r.node_index, 0);
    EXPECT_GT(r.text_pos, 0u)
        << "DirectWrite 経路では HitTestPoint で text_pos が 0 より大きく解決されるはず";
}

// テーブル row_cum_y / col_cum_x 経由の hit test。
// LayoutEngine + DWriteTextMeasurer を通した cache の table_layout には
// row_cum_y / col_cum_x が積まれる。HitTestTable はそれを upper_bound で参照する。
TEST_F(HitTestDWriteTest, TableHitDetectsRowAndColumn)
{
    auto pl = ParseAndLayout(L"| H1 | H2 |\n|---|---|\n| 11 | 12 |\n| 21 | 22 |");

    int table_idx = -1;
    for (size_t i = 0; i < pl.nodes.size(); ++i) {
        if (pl.nodes[i].type == NodeType::Table) {
            table_idx = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(table_idx, 0);
    ASSERT_TRUE(pl.cache[table_idx].has_table_layout());

    // テーブル中央あたりに hit する位置。
    const auto& entry = pl.cache[table_idx];
    const int sx = static_cast<int>(theme_.margin_left + 30.0f);
    const int sy = static_cast<int>(entry.text_top + entry.height * 0.5f);

    const float content_width = ContentWidth(800.0f);
    const MdPaneHitContext ctx{
        pl.nodes, pl.cache, theme_, 0.0f, 0.0f, 1.0f,
        sx, sy, content_width, 600.0f
    };
    const auto r = hit_.HitTest(ctx);

    EXPECT_EQ(r.node_index, table_idx)
        << "テーブル領域内の click は table ノードを返すはず";
}

// CodeBlockButtonsHitTest: Copy ボタンの上を click すると copy_node にコードブロック
// インデックスが入ること。共有 cache の matches も正しく動くかを軽く確認する。
TEST_F(HitTestDWriteTest, CodeBlockButtonsHitTest_CopyHitReturnsNode)
{
    auto pl = ParseAndLayout(L"```\nint main() { return 0; }\n```");

    int code_idx = -1;
    for (size_t i = 0; i < pl.nodes.size(); ++i) {
        if (pl.nodes[i].type == NodeType::CodeBlock) {
            code_idx = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(code_idx, 0);

    const auto& entry = pl.cache[code_idx];
    const float content_width = ContentWidth(800.0f);
    const float block_right = theme_.margin_left + content_width;
    const float block_top = entry.text_top - theme_.code_block_padding;
    const D2D1_RECT_F btn = OverlayButtonRect(block_right, block_top);
    const int sx = static_cast<int>((btn.left + btn.right) * 0.5f);
    const int sy = static_cast<int>((btn.top + btn.bottom) * 0.5f);

    const MdPaneHitContext ctx{
        pl.nodes, pl.cache, theme_, 0.0f, 0.0f, 1.0f,
        sx, sy, content_width, 600.0f
    };
    const auto hits = hit_.CodeBlockButtonsHitTest(ctx);
    EXPECT_EQ(hits.copy_node, code_idx);
    EXPECT_EQ(hits.save_node, -1);
    EXPECT_EQ(hits.svg_copy_node, -1);
}

// CodeBlockButtonsHitTest はキャッシュ機構を持つ: 同じ ctx + 同じ effects_generation
// の繰り返し呼び出しでは結果が再利用される。書き換えしないことを 2 回呼んで確認する。
TEST_F(HitTestDWriteTest, CodeBlockButtonsHitTest_RepeatCallReturnsSameResult)
{
    auto pl = ParseAndLayout(L"```\nfoo\n```");
    const float content_width = ContentWidth(800.0f);
    const MdPaneHitContext ctx{
        pl.nodes, pl.cache, theme_, 0.0f, 0.0f, 1.0f,
        100, 100, content_width, 600.0f
    };
    const auto a = hit_.CodeBlockButtonsHitTest(ctx);
    const auto b = hit_.CodeBlockButtonsHitTest(ctx);
    EXPECT_EQ(a.copy_node, b.copy_node);
    EXPECT_EQ(a.save_node, b.save_node);
    EXPECT_EQ(a.svg_copy_node, b.svg_copy_node);
}
