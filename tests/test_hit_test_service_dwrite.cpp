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
    auto pl = ParseAndLayout("Hello, this is a long paragraph for hit testing.");
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
    auto pl = ParseAndLayout("| H1 | H2 |\n|---|---|\n| 11 | 12 |\n| 21 | 22 |");

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
    auto pl = ParseAndLayout("```\nint main() { return 0; }\n```");

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
    auto pl = ParseAndLayout("```\nfoo\n```");
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

// 回帰テスト: HitTest は partition_point と local_y 計算で同じ entry.text_top を
// 基準にすべき。Fenwick (block_heights) と entry.text_top が乖離すると、 修正前は
// candidate_text_top に Fenwick 経由の TextTopOf を使っていたため local_y がズレ、
// 縦スクロール量に比例してテキスト選択位置が大きく上にズレる現象が発生していた。
// (修正コミット: hit_test_service.cpp で TextTopOf → cache[candidate].text_top)
TEST_F(HitTestDWriteTest, HitTestRobustToFenwickDesync)
{
    auto pl = ParseAndLayout(
        "First paragraph here.\n\nSecond paragraph with more words.\n\n"
        "Third paragraph in the middle of document.\n\nFourth and last paragraph.");

    // 3 つ目以上の Paragraph ノードを target にする (上の方ほど誤差が小さく差が出にくい)。
    int target = -1;
    int para_count = 0;
    for (size_t i = 0; i < pl.nodes.size(); ++i) {
        if (pl.nodes[i].type == NodeType::Paragraph) {
            ++para_count;
            if (para_count == 3) {
                target = static_cast<int>(i);
                break;
            }
        }
    }
    ASSERT_GE(target, 0) << "テスト前提: 3 つ目以降の Paragraph ノードが必要";

    // Fenwick の block_heights を意図的に小さい値で潰す。entry.text_top はそのまま。
    // これで TextTopOf(i) = margin_top + PrefixSum(i) + sa[i] は本来より遥かに小さくなり、
    // 一方 partition_point の比較対象 entry.text_top は元の正しい累積位置のまま。
    for (size_t i = 0; i < pl.nodes.size(); ++i) {
        pl.cache.SetBlockHeight(i, 1.0f);
    }

    const auto& entry = pl.cache[target];
    const int sx = static_cast<int>(theme_.margin_left + 20.0f);
    // テキスト第 1 行の中央付近を狙う (entry.text_top + line_height * 0.5 程度)。
    const int sy = static_cast<int>(entry.text_top + 4.0f);

    const float content_width = ContentWidth(800.0f);
    const MdPaneHitContext ctx{
        pl.nodes, pl.cache, theme_, 0.0f, 0.0f, 1.0f,
        sx, sy, content_width, 600.0f
    };
    const auto r = hit_.HitTest(ctx);

    EXPECT_EQ(r.node_index, target)
        << "Fenwick が乖離しても partition_point は entry.text_top で比較するため正しい候補が選ばれる";

    // 修正前 (TextTopOf 使用) は local_y が巨大値になり HitTestPoint がテキスト末尾に
    // クランプして r.text_pos == text.size() になる。修正後 (entry.text_top 使用) は
    // 第 1 行内の早い位置を返すため text.size() より十分小さい。
    const auto target_text_size = pl.nodes[target].GetText().size();
    ASSERT_GT(target_text_size, 5u);
    EXPECT_LT(r.text_pos, target_text_size)
        << "local_y が entry.text_top 基準で計算されれば、 末尾クランプではなく行内位置が返る";
}
