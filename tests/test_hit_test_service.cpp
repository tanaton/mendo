#include <gtest/gtest.h>
#include "document_test_helpers.h"
#include "hit_test_service.h"
#include "mock_text_measurer.h"
#include "syntax.h"
#include "ui_constants.h"
#include "ui_types.h"
#include <algorithm>

// MockTextMeasurer 前提のため、DirectWrite 依存経路 (text_layout
// 非 null、row_cum_y/col_cum_x による二分探索、cell_layout) は対象外。

class HitTestTest : public MockLayoutTestBase {
protected:
    // dpi=1・ペイン原点 0 なので、画面座標がそのままペイン内の DIP になる。
    MdPaneHitContext Ctx(const ParsedLayout& pl, float scroll_y, int screen_x, int screen_y) const
    {
        return { pl.nodes, pl.cache, theme_, scroll_y, PaneRect{}, 1.0f, screen_x, screen_y };
    }

    // オーバーレイボタンは可視範囲 (ペインの高さ) とコンテンツ幅で決まる。
    MdPaneHitContext OverlayCtx(const ParsedLayout& pl, int screen_x, int screen_y) const
    {
        return { pl.nodes, pl.cache, theme_, 0.0f, PaneRect{ 0.0f, 0.0f, 0.0f, 600.0f }, 1.0f,
                 screen_x, screen_y, theme_.ContentWidth(800.0f) };
    }
};

// ---- NavButtonHitTest: 純粋な座標計算のみ ----
// 矩形は src 側の NavBackButtonRect / NavForwardButtonRect を使う。
// 「式を二重実装するとリファクタ時にテストもズレる」問題を避けるため。

TEST_F(HitTestTest, NavButton_HitsButtonsInclusiveAndMissesAround)
{
    // MD ペインが原点以外にある場合も同じ結果になること
    constexpr PaneRect kPanes[] = {
        { 0.0f, 0.0f, 800.0f, 600.0f },
        { 200.0f, 40.0f, 600.0f, 500.0f },
    };
    for (const auto& md : kPanes) {
        const D2D1_RECT_F b = NavBackButtonRect(md);
        const D2D1_RECT_F f = NavForwardButtonRect(md);
        const float b_cx = (b.left + b.right) * 0.5f;
        const float b_cy = (b.top + b.bottom) * 0.5f;
        const float f_cy = (f.top + f.bottom) * 0.5f;
        const struct {
            const char* name;
            float x;
            float y;
            NavButtonHover expected;
        } cases[] = {
            { "BackCenter", b_cx, b_cy, NavButtonHover::Back },
            { "ForwardCenter", (f.left + f.right) * 0.5f, f_cy, NavButtonHover::Forward },
            // PointInRectInclusive なので左右の端ちょうども Back
            { "BackLeftEdge", b.left, b_cy, NavButtonHover::Back },
            { "BackRightEdge", b.right, b_cy, NavButtonHover::Back },
            { "GapBetweenButtons", b.right + NAV_BTN_GAP * 0.5f, b_cy, NavButtonHover::None },
            { "AboveBack", b_cx, b.top - 1.0f, NavButtonHover::None },
            { "BelowBack", b_cx, b.bottom + 1.0f, NavButtonHover::None },
            { "LeftOfBack", b.left - 1.0f, b_cy, NavButtonHover::None },
            { "RightOfForward", f.right + 1.0f, f_cy, NavButtonHover::None },
        };
        for (const auto& c : cases) {
            SCOPED_TRACE(::testing::Message() << c.name << " pane=(" << md.x << "," << md.y << ")");
            EXPECT_EQ(hit_test::NavButtonHitTest(c.x, c.y, md), c.expected);
        }
    }
}

// ---- HitTest: MD ペイン汎用ヒット ----

TEST_F(HitTestTest, HitTest_EmptyDocumentReturnsSentinel)
{
    auto pr = ParseAndLayout("");
    if (!pr.nodes.empty()) {
        // 空入力でも他のノード（例: 空段落）が生成される実装の場合は以下を適用。
        // このテストは ctx.nodes.empty() 分岐の検証なので、nodes が空の場合のみ
        // 期待値をチェックする。
        GTEST_SKIP() << "ParseMarkdown(\"\") が空でないため empty 分岐を検証できない";
    }
    const auto ctx = Ctx(pr, 0.0f, 100, 100);
    auto r = hit_test::HitTest(ctx);
    EXPECT_EQ(r.node_index, -1);
    EXPECT_EQ(r.text_pos, 0u);
}

TEST_F(HitTestTest, HitTest_BelowAllNodesReturnsLastNonEmpty)
{
    auto pr = ParseAndLayout("First paragraph\n\nSecond paragraph");
    ASSERT_FALSE(pr.nodes.empty());

    // 全ノードの下端より十分下（dpi=1, scroll=0 なので dip_y = screen_y）
    float last_bottom = 0.0f;
    for (size_t i = 0; i < pr.nodes.size(); ++i) {
        const float text_top = pr.cache.Top(i);
        last_bottom = std::max(last_bottom, text_top + pr.cache[i].height);
    }
    const auto ctx = Ctx(pr, 0.0f, 50, static_cast<int>(last_bottom) + 1000);
    auto r = hit_test::HitTest(ctx);

    // reverse 走査で最初に見つかる非空ノード
    int expected_idx = -1;
    for (int i = static_cast<int>(pr.nodes.size()) - 1; i >= 0; --i) {
        if (!pr.nodes[i].GetText().empty()) {
            expected_idx = i;
            break;
        }
    }
    ASSERT_GE(expected_idx, 0);
    EXPECT_EQ(r.node_index, expected_idx);
    EXPECT_EQ(r.text_pos,
              static_cast<uint32_t>(pr.nodes[expected_idx].GetText().size()));
}

TEST_F(HitTestTest, HitTest_AboveFirstNodeClampsToFirstNodeStart)
{
    // MockTextMeasurer は text_layout=nullptr を設定する。
    // 先頭ノードより上 (margin_top 内) のクリックは先頭の非空ノードの先頭に返る。
    auto pr = ParseAndLayout("Hello\n\nWorld");
    ASSERT_GE(pr.nodes.size(), 2u);

    const auto ctx = Ctx(pr, 0.0f, 50, 0);
    auto r = hit_test::HitTest(ctx);
    EXPECT_EQ(r.node_index, 0);
    EXPECT_EQ(r.text_pos, 0u);
}

TEST_F(HitTestTest, HitTest_GapBetweenNodesClampsToPrecedingNodeEnd)
{
    // ノード間の余白クリックは直前の非空ノード末尾に返る。
    // 文書全体の末尾へ飛ばすと、余白からのドラッグで巨大選択が作られてしまう。
    auto pr = ParseAndLayout("Hello\n\nWorld");
    ASSERT_GE(pr.nodes.size(), 2u);
    const float node0_bottom = pr.cache.Top(0) + pr.cache[0].height;
    ASSERT_LT(node0_bottom, pr.cache.Top(1)) << "ノード間に余白があること";

    const float gap_y = (node0_bottom + pr.cache.Top(1)) * 0.5f;
    const auto ctx = Ctx(pr, gap_y, 50, 0);
    auto r = hit_test::HitTest(ctx);
    EXPECT_EQ(r.node_index, 0);
    EXPECT_EQ(r.text_pos, pr.nodes[0].GetText().size());
}

// 表の直下の余白は表のテキスト末尾へ寄せる。表を飛ばして直前の段落末尾に寄せると、
// そこから始めたドラッグで表全体が選択に入る。
TEST_F(HitTestTest, HitTest_GapBelowTableClampsToTableTextEnd)
{
    auto pr = ParseAndLayout("Before\n\n| a |\n|---|\n| b |\n\nAfter");
    ASSERT_EQ(pr.nodes.size(), 3u);
    ASSERT_EQ(pr.nodes[1].type, NodeType::Table);
    ASSERT_EQ(pr.nodes[1].GetText(), "a\nb");
    const float table_bottom = pr.cache.Bottom(1) + layout::NodeBoxPadY(pr.nodes[1], theme_);
    ASSERT_LT(table_bottom, pr.cache.Top(2)) << "表と次の段落の間に余白があること";

    const float gap_y = (table_bottom + pr.cache.Top(2)) * 0.5f;
    const auto ctx = Ctx(pr, gap_y, 50, 0);
    const auto r = hit_test::HitTest(ctx);
    EXPECT_EQ(r.node_index, 1);
    EXPECT_EQ(r.text_pos, 3u);
}

TEST_F(HitTestTest, HitTest_TextlessCandidateFallsForwardToFirstNonEmpty)
{
    // candidate がテキストを持たないノード (水平線) の場合、
    // 後方探索が外れても -1 ではなく先頭の非空ノードに倒れること
    auto pr = ParseAndLayout("---\n\nHello");
    ASSERT_GE(pr.nodes.size(), 2u);
    ASSERT_EQ(pr.nodes[0].type, NodeType::HorizontalRule);
    ASSERT_TRUE(pr.nodes[0].GetText().empty());
    ASSERT_FALSE(pr.nodes[1].GetText().empty());
    ASSERT_GT(pr.cache[0].height, 0.0f);

    const float mid_y = pr.cache.Top(0) + pr.cache[0].height * 0.5f;
    const auto ctx = Ctx(pr, mid_y, 50, 0);
    auto r = hit_test::HitTest(ctx);
    EXPECT_EQ(r.node_index, 1);
    EXPECT_EQ(r.text_pos, 0u);
}

// alt が空の画像の上でも、前のノードへ移らず画像自身に当たる (src のツールチップを出すため)。
TEST_F(HitTestTest, HitTest_InsideEmptyAltImageHitsImage)
{
    auto pr = ParseAndLayout("Before\n\n![](x.png)\n\nAfter");
    ASSERT_EQ(pr.nodes.size(), 3u);
    ASSERT_EQ(pr.nodes[1].type, NodeType::Image);
    ASSERT_TRUE(pr.nodes[1].GetText().empty());
    ASSERT_GT(pr.cache[1].height, 0.0f);

    const float mid_y = pr.cache.Top(1) + pr.cache[1].height * 0.5f;
    const auto ctx = Ctx(pr, mid_y, 50, 0);
    const auto r = hit_test::HitTest(ctx);
    EXPECT_EQ(r.node_index, 1);
    EXPECT_EQ(r.text_pos, 0u);
    EXPECT_EQ(hit_test::HitNode(ctx), 1);
}

// ホバーは HitNode でノードだけ求めるので、HitTest と同じノードを返すこと。
TEST_F(HitTestTest, HitNodeMatchesHitTestNodeIndex)
{
    auto pr = ParseAndLayout("![](x.png)\n\nHello\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n```cpp\nint x;\n```\n\n---\n\nWorld");
    ASSERT_FALSE(pr.nodes.empty());
    const float bottom = pr.cache.Bottom(pr.nodes.size() - 1) + 50.0f;
    for (float y = -10.0f; y < bottom; y += 3.0f) {
        const auto ctx = Ctx(pr, y, 50, 0);
        EXPECT_EQ(hit_test::HitNode(ctx), hit_test::HitTest(ctx).node_index) << "y=" << y;
    }
}

// ---- HitTestTable ----

TEST_F(HitTestTest, HitTestTable_NoLayoutDataReturnsTextEnd)
{
    auto pr = ParseAndLayout("| A | B |\n|---|---|\n| 1 | 2 |");
    const int table_idx = FindFirstNodeIndexByType(pr.nodes, NodeType::Table);
    ASSERT_GE(table_idx, 0);

    // has_table_layout() を false にするためテーブル幾何を捨てる
    auto& entry = pr.cache[table_idx];
    entry.reset_table_layout();
    ASSERT_FALSE(entry.has_table_layout());

    const float entry_text_top = pr.cache.Top(static_cast<size_t>(table_idx));
    auto r = hit_test::HitTestTable(pr.nodes[table_idx], entry, entry_text_top, table_idx,
                                    theme_, 100.0f, entry_text_top + 5.0f);
    EXPECT_EQ(r.node_index, table_idx);
    EXPECT_EQ(r.text_pos,
              static_cast<uint32_t>(pr.nodes[table_idx].GetText().size()));
}

TEST_F(HitTestTest, HitTestTable_ClickAboveAllRowsReturnsTextEnd)
{
    auto pr = ParseAndLayout("| A | B |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |");
    const int table_idx = FindFirstNodeIndexByType(pr.nodes, NodeType::Table);
    ASSERT_GE(table_idx, 0);

    const auto& entry = pr.cache[table_idx];
    ASSERT_TRUE(entry.has_table_layout());

    // テーブル上端より 10 dip 上 → FindTableRow は -1 を返す
    const float entry_text_top = pr.cache.Top(static_cast<size_t>(table_idx));
    auto r = hit_test::HitTestTable(pr.nodes[table_idx], entry, entry_text_top, table_idx,
                                    theme_,
                                    theme_.margin_left + 10.0f,
                                    entry_text_top - 10.0f);
    EXPECT_EQ(r.node_index, table_idx);
    // 末尾は concat_text の長さ (0 だと先頭セルと区別できない)。
    EXPECT_EQ(r.text_pos, static_cast<uint32_t>(pr.nodes[table_idx].GetText().size()));
}

TEST_F(HitTestTest, HitTestTable_HitsFirstCellWithoutCellLayout)
{
    // MockTextMeasurer はセル layout を生成しない (null のまま) ので text_pos は flat_offset。
    // 先頭行・先頭列の flat_offset は 0。
    auto pr = ParseAndLayout("| A | B |\n|---|---|\n| 1 | 2 |");
    const int table_idx = FindFirstNodeIndexByType(pr.nodes, NodeType::Table);
    ASSERT_GE(table_idx, 0);

    const auto& entry = pr.cache[table_idx];
    ASSERT_TRUE(entry.has_table_layout());
    const auto& tl = *entry.table_layout();
    ASSERT_GE(tl.row_heights.size(), 1u);
    ASSERT_GE(tl.col_widths.size(), 1u);

    const float entry_text_top = pr.cache.Top(static_cast<size_t>(table_idx));
    const float row0_mid_y = entry_text_top + tl.row_heights[0] * 0.5f;
    const float col0_mid_x = theme_.margin_left + tl.col_widths[0] * 0.5f;

    auto r = hit_test::HitTestTable(pr.nodes[table_idx], entry, entry_text_top, table_idx,
                                    theme_, col0_mid_x, row0_mid_y);
    EXPECT_EQ(r.node_index, table_idx);
    EXPECT_EQ(r.text_pos, 0u);
}

// ---- SaveButtonHitTest ----

TEST_F(HitTestTest, SaveButton_NoDiagramReturnsNegative)
{
    auto pr = ParseAndLayout("Just text.");
    const auto ctx = OverlayCtx(pr, 400, 100);
    EXPECT_NE(hit_test::OverlayButtonHitTest(ctx).kind, OverlayButton::DiagramSave);
}

TEST_F(HitTestTest, SaveButton_NonDiagramCodeBlockReturnsNegative)
{
    auto pr = ParseAndLayout("```\nplain code\n```");
    const auto ctx = OverlayCtx(pr, 400, 100);
    EXPECT_NE(hit_test::OverlayButtonHitTest(ctx).kind, OverlayButton::DiagramSave);
}

TEST_F(HitTestTest, SaveButton_DiagramWithoutBitmapReturnsNegative)
{
    // Mermaid ブロックはあるが diagram.bitmap が空（未ロード）→ -1
    auto pr = ParseAndLayout("```mermaid\ngraph TD\n```");
    const int mermaid_idx = FindFirstNodeIndexByType(pr.nodes, NodeType::Diagram);
    ASSERT_GE(mermaid_idx, 0);
    ASSERT_TRUE(IsDiagramLanguage(pr.nodes[mermaid_idx].code_language()));
    const auto* diagram = pr.cache.FindDiagram(static_cast<size_t>(mermaid_idx));
    ASSERT_TRUE(diagram == nullptr || !diagram->bitmap);

    const float content_width = theme_.ContentWidth(800.0f);
    const float entry_text_top = pr.cache.Top(static_cast<size_t>(mermaid_idx));
    const auto ctx = OverlayCtx(pr, static_cast<int>(theme_.margin_left + content_width - 10), static_cast<int>(entry_text_top + 5));
    EXPECT_NE(hit_test::OverlayButtonHitTest(ctx).kind, OverlayButton::DiagramSave);
}

TEST_F(HitTestTest, SaveButton_EmptyDocumentReturnsNegative)
{
    auto pr = ParseAndLayout("");
    const auto ctx = OverlayCtx(pr, 400, 100);
    EXPECT_NE(hit_test::OverlayButtonHitTest(ctx).kind, OverlayButton::DiagramSave);
}
