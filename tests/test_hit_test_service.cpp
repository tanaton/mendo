#include <gtest/gtest.h>
#include "hit_test_service.h"
#include "layout_computer.h"
#include "ui_constants.h"
#include "layout.h"
#include "mock_text_measurer.h"
#include "parser.h"
#include "ui_types.h"
#include "syntax.h"
#include <algorithm>

// MockTextMeasurer 前提のため、DirectWrite 依存経路 (text_layout
// 非 null、row_cum_y/col_cum_x による二分探索、cell_layout) は対象外。

class HitTestServiceTest : public ::testing::Test {
protected:
    MockTextMeasurer mock_;
    LayoutEngine engine_;
    HitTestService hit_test_;
    Theme theme_;

    void SetUp() override
    {
        theme_ = GetLightTheme();
        ASSERT_TRUE(engine_.Init(&mock_, theme_));
    }

    struct ParsedLayout {
        std::pmr::vector<Node> nodes;
        LayoutCache cache;
    };

    ParsedLayout Parse(std::string_view md, float viewport_w = 800.0f)
    {
        ParsedLayout r;
        r.nodes = ParseMarkdown(md).nodes;
        r.cache.Resize(r.nodes.size());
        engine_.ComputeLayout(r.nodes, r.cache, viewport_w);
        return r;
    }
};

// ---- NavButtonHitTest: 純粋な座標計算のみ ----
// 矩形は src 側の NavBackButtonRect / NavForwardButtonRect を使う。
// 「式を二重実装するとリファクタ時にテストもズレる」問題を避けるため。

TEST_F(HitTestServiceTest, NavButton_HitBackCenter)
{
    PaneRect md{ 0.0f, 0.0f, 800.0f, 600.0f };
    const ButtonRect back = NavBackButtonRect(md);
    EXPECT_EQ(hit_test_.NavButtonHitTest(back.x + back.w * 0.5f,
                                         back.y + back.h * 0.5f, md),
              NavButtonHover::Back);
}

TEST_F(HitTestServiceTest, NavButton_HitForwardCenter)
{
    PaneRect md{ 0.0f, 0.0f, 800.0f, 600.0f };
    const ButtonRect fwd = NavForwardButtonRect(md);
    EXPECT_EQ(hit_test_.NavButtonHitTest(fwd.x + fwd.w * 0.5f,
                                         fwd.y + fwd.h * 0.5f, md),
              NavButtonHover::Forward);
}

TEST_F(HitTestServiceTest, NavButton_GapBetweenButtonsIsNone)
{
    PaneRect md{ 0.0f, 0.0f, 800.0f, 600.0f };
    const ButtonRect back = NavBackButtonRect(md);
    // Back の右端より NAV_BTN_GAP/2 右（Forward 左端より NAV_BTN_GAP/2 左）
    const float gx = back.x + back.w + NAV_BTN_GAP * 0.5f;
    EXPECT_EQ(hit_test_.NavButtonHitTest(gx, back.y + back.h * 0.5f, md),
              NavButtonHover::None);
}

TEST_F(HitTestServiceTest, NavButton_AboveRangeIsNone)
{
    PaneRect md{ 0.0f, 0.0f, 800.0f, 600.0f };
    const ButtonRect back = NavBackButtonRect(md);
    EXPECT_EQ(hit_test_.NavButtonHitTest(back.x + back.w * 0.5f, back.y - 1.0f, md),
              NavButtonHover::None);
}

TEST_F(HitTestServiceTest, NavButton_BelowRangeIsNone)
{
    PaneRect md{ 0.0f, 0.0f, 800.0f, 600.0f };
    const ButtonRect back = NavBackButtonRect(md);
    EXPECT_EQ(hit_test_.NavButtonHitTest(back.x + back.w * 0.5f,
                                         back.y + back.h + 1.0f, md),
              NavButtonHover::None);
}

TEST_F(HitTestServiceTest, NavButton_LeftOfBackIsNone)
{
    PaneRect md{ 0.0f, 0.0f, 800.0f, 600.0f };
    const ButtonRect back = NavBackButtonRect(md);
    EXPECT_EQ(hit_test_.NavButtonHitTest(back.x - 1.0f, back.y + back.h * 0.5f, md),
              NavButtonHover::None);
}

TEST_F(HitTestServiceTest, NavButton_RightOfForwardIsNone)
{
    PaneRect md{ 0.0f, 0.0f, 800.0f, 600.0f };
    const ButtonRect fwd = NavForwardButtonRect(md);
    EXPECT_EQ(hit_test_.NavButtonHitTest(fwd.x + fwd.w + 1.0f,
                                         fwd.y + fwd.h * 0.5f, md),
              NavButtonHover::None);
}

TEST_F(HitTestServiceTest, NavButton_BackLeftEdgeIsInclusive)
{
    // dip_x == back.x は ButtonRect::Contains の >= 条件なので Back
    PaneRect md{ 0.0f, 0.0f, 800.0f, 600.0f };
    const ButtonRect back = NavBackButtonRect(md);
    EXPECT_EQ(hit_test_.NavButtonHitTest(back.x, back.y + back.h * 0.5f, md),
              NavButtonHover::Back);
}

TEST_F(HitTestServiceTest, NavButton_BackRightEdgeIsInclusive)
{
    // dip_x == back.x + back.w も <= 条件なので Back
    PaneRect md{ 0.0f, 0.0f, 800.0f, 600.0f };
    const ButtonRect back = NavBackButtonRect(md);
    EXPECT_EQ(hit_test_.NavButtonHitTest(back.x + back.w,
                                         back.y + back.h * 0.5f, md),
              NavButtonHover::Back);
}

TEST_F(HitTestServiceTest, NavButton_PositionsWithNonZeroPaneOrigin)
{
    // MD ペインが原点以外にある場合でも正しく算出されること
    PaneRect md{ 200.0f, 40.0f, 600.0f, 500.0f };
    const ButtonRect back = NavBackButtonRect(md);
    EXPECT_EQ(hit_test_.NavButtonHitTest(back.x + back.w * 0.5f,
                                         back.y + back.h * 0.5f, md),
              NavButtonHover::Back);
}

// ---- HitTest: MD ペイン汎用ヒット ----

TEST_F(HitTestServiceTest, HitTest_EmptyDocumentReturnsSentinel)
{
    auto pr = Parse("");
    if (!pr.nodes.empty()) {
        // 空入力でも他のノード（例: 空段落）が生成される実装の場合は以下を適用。
        // このテストは ctx.nodes.empty() 分岐の検証なので、nodes が空の場合のみ
        // 期待値をチェックする。
        GTEST_SKIP() << "ParseMarkdown(\"\") が空でないため empty 分岐を検証できない";
    }
    MdPaneHitContext ctx{
        pr.nodes, pr.cache, theme_,
        0.0f, 0.0f, 1.0f, 100, 100
    };
    auto r = hit_test_.HitTest(ctx);
    EXPECT_EQ(r.node_index, -1);
    EXPECT_EQ(r.text_pos, 0u);
}

TEST_F(HitTestServiceTest, HitTest_BelowAllNodesReturnsLastNonEmpty)
{
    auto pr = Parse("First paragraph\n\nSecond paragraph");
    ASSERT_FALSE(pr.nodes.empty());

    // 全ノードの下端より十分下（dpi=1, scroll=0 なので dip_y = screen_y）
    float last_bottom = 0.0f;
    for (size_t i = 0; i < pr.nodes.size(); ++i) {
        const float text_top = mendo::layout::TextTopOf(pr.cache, i, pr.nodes[i], theme_);
        last_bottom = std::max(last_bottom, text_top + pr.cache[i].height);
    }
    MdPaneHitContext ctx{
        pr.nodes, pr.cache, theme_,
        0.0f, 0.0f, 1.0f,
        50,
        static_cast<int>(last_bottom) + 1000
    };
    auto r = hit_test_.HitTest(ctx);

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

TEST_F(HitTestServiceTest, HitTest_AboveFirstNodeClampsToFirstNodeStart)
{
    // MockTextMeasurer は text_layout=nullptr を設定する。
    // 先頭ノードより上 (margin_top 内) のクリックは先頭の非空ノードの先頭に返る。
    auto pr = Parse("Hello\n\nWorld");
    ASSERT_GE(pr.nodes.size(), 2u);

    MdPaneHitContext ctx{
        pr.nodes, pr.cache, theme_,
        0.0f, 0.0f, 1.0f, 50, 0
    };
    auto r = hit_test_.HitTest(ctx);
    EXPECT_EQ(r.node_index, 0);
    EXPECT_EQ(r.text_pos, 0u);
}

TEST_F(HitTestServiceTest, HitTest_GapBetweenNodesClampsToPrecedingNodeEnd)
{
    // ノード間の余白クリックは直前の非空ノード末尾に返る。
    // 文書全体の末尾へ飛ばすと、余白からのドラッグで巨大選択が作られてしまう。
    auto pr = Parse("Hello\n\nWorld");
    ASSERT_GE(pr.nodes.size(), 2u);
    const float node0_bottom = pr.cache[0].text_top + pr.cache[0].height;
    ASSERT_LT(node0_bottom, pr.cache[1].text_top) << "ノード間に余白があること";

    const float gap_y = (node0_bottom + pr.cache[1].text_top) * 0.5f;
    MdPaneHitContext ctx{
        pr.nodes, pr.cache, theme_,
        gap_y, 0.0f, 1.0f, 50, 0
    };
    auto r = hit_test_.HitTest(ctx);
    EXPECT_EQ(r.node_index, 0);
    EXPECT_EQ(r.text_pos, pr.nodes[0].GetText().size());
}

// ---- HitTestTable ----

TEST_F(HitTestServiceTest, HitTestTable_NoLayoutDataReturnsTextEnd)
{
    auto pr = Parse("| A | B |\n|---|---|\n| 1 | 2 |");
    int table_idx = -1;
    for (size_t i = 0; i < pr.nodes.size(); ++i) {
        if (pr.nodes[i].type == NodeType::Table) {
            table_idx = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(table_idx, 0);

    // has_table_layout() を false にするため unique_ptr をリセット
    auto& entry = pr.cache[table_idx];
    entry.table_layout.reset();
    ASSERT_FALSE(entry.has_table_layout());

    const float entry_text_top = mendo::layout::TextTopOf(pr.cache, static_cast<size_t>(table_idx), pr.nodes[table_idx], theme_);
    auto r = hit_test_.HitTestTable(pr.nodes[table_idx], entry, entry_text_top, table_idx,
                                    theme_, 100.0f, entry_text_top + 5.0f);
    EXPECT_EQ(r.node_index, table_idx);
    EXPECT_EQ(r.text_pos,
              static_cast<uint32_t>(pr.nodes[table_idx].GetText().size()));
}

TEST_F(HitTestServiceTest, HitTestTable_ClickAboveAllRowsReturnsTextEnd)
{
    auto pr = Parse("| A | B |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |");
    int table_idx = -1;
    for (size_t i = 0; i < pr.nodes.size(); ++i) {
        if (pr.nodes[i].type == NodeType::Table) {
            table_idx = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(table_idx, 0);

    const auto& entry = pr.cache[table_idx];
    ASSERT_TRUE(entry.has_table_layout());

    // テーブル上端より 10 dip 上 → FindTableRow は -1 を返す
    const float entry_text_top = mendo::layout::TextTopOf(pr.cache, static_cast<size_t>(table_idx), pr.nodes[table_idx], theme_);
    auto r = hit_test_.HitTestTable(pr.nodes[table_idx], entry, entry_text_top, table_idx,
                                    theme_,
                                    theme_.margin_left + 10.0f,
                                    entry_text_top - 10.0f);
    EXPECT_EQ(r.node_index, table_idx);
    EXPECT_EQ(r.text_pos,
              static_cast<uint32_t>(pr.nodes[table_idx].GetText().size()));
}

TEST_F(HitTestServiceTest, HitTestTable_LinearScanHitsFirstRow)
{
    // MockTextMeasurer は row_cum_y / col_cum_x / cell_layouts を設定しないので
    // 線形フォールバックに落ちる。cell_layout=null のため text_pos は flat_offset。
    // 先頭行・先頭列の flat_offset は 0。
    auto pr = Parse("| A | B |\n|---|---|\n| 1 | 2 |");
    int table_idx = -1;
    for (size_t i = 0; i < pr.nodes.size(); ++i) {
        if (pr.nodes[i].type == NodeType::Table) {
            table_idx = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(table_idx, 0);

    const auto& entry = pr.cache[table_idx];
    ASSERT_TRUE(entry.has_table_layout());
    const auto& tl = *entry.table_layout;
    ASSERT_GE(tl.row_heights.size(), 1u);
    ASSERT_GE(tl.col_widths.size(), 1u);

    const float entry_text_top = mendo::layout::TextTopOf(pr.cache, static_cast<size_t>(table_idx), pr.nodes[table_idx], theme_);
    const float row0_mid_y = entry_text_top + tl.row_heights[0] * 0.5f;
    const float col0_mid_x = theme_.margin_left + tl.col_widths[0] * 0.5f;

    auto r = hit_test_.HitTestTable(pr.nodes[table_idx], entry, entry_text_top, table_idx,
                                    theme_, col0_mid_x, row0_mid_y);
    EXPECT_EQ(r.node_index, table_idx);
    EXPECT_EQ(r.text_pos, 0u);
}

// ---- SaveButtonHitTest ----

TEST_F(HitTestServiceTest, SaveButton_NoDiagramReturnsNegative)
{
    auto pr = Parse("Just text.");
    const float content_width = 800.0f - theme_.margin_left - theme_.margin_right;
    MdPaneHitContext ctx{
        pr.nodes, pr.cache, theme_,
        0.0f, 0.0f, 1.0f, 400, 100,
        content_width, 600.0f
    };
    EXPECT_EQ(hit_test_.CodeBlockButtonsHitTest(ctx).save_node, -1);
}

TEST_F(HitTestServiceTest, SaveButton_NonDiagramCodeBlockReturnsNegative)
{
    auto pr = Parse("```\nplain code\n```");
    const float content_width = 800.0f - theme_.margin_left - theme_.margin_right;
    MdPaneHitContext ctx{
        pr.nodes, pr.cache, theme_,
        0.0f, 0.0f, 1.0f, 400, 100,
        content_width, 600.0f
    };
    EXPECT_EQ(hit_test_.CodeBlockButtonsHitTest(ctx).save_node, -1);
}

TEST_F(HitTestServiceTest, SaveButton_DiagramWithoutBitmapReturnsNegative)
{
    // Mermaid ブロックはあるが diagram.bitmap が空（未ロード）→ -1
    auto pr = Parse("```mermaid\ngraph TD\n```");
    int mermaid_idx = -1;
    for (size_t i = 0; i < pr.nodes.size(); ++i) {
        if (pr.nodes[i].type == NodeType::CodeBlock &&
            IsDiagramLanguage(pr.nodes[i].code_language())) {
            mermaid_idx = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(mermaid_idx, 0);
    ASSERT_FALSE(pr.cache.GetDiagram(static_cast<size_t>(mermaid_idx)).bitmap);

    const float content_width = 800.0f - theme_.margin_left - theme_.margin_right;
    const float entry_text_top = mendo::layout::TextTopOf(pr.cache, static_cast<size_t>(mermaid_idx), pr.nodes[mermaid_idx], theme_);
    MdPaneHitContext ctx{
        pr.nodes, pr.cache, theme_,
        0.0f, 0.0f, 1.0f,
        static_cast<int>(theme_.margin_left + content_width - 10),
        static_cast<int>(entry_text_top + 5),
        content_width, 600.0f
    };
    EXPECT_EQ(hit_test_.CodeBlockButtonsHitTest(ctx).save_node, -1);
}

TEST_F(HitTestServiceTest, SaveButton_EmptyDocumentReturnsNegative)
{
    auto pr = Parse("");
    const float content_width = 800.0f - theme_.margin_left - theme_.margin_right;
    MdPaneHitContext ctx{
        pr.nodes, pr.cache, theme_,
        0.0f, 0.0f, 1.0f, 400, 100,
        content_width, 600.0f
    };
    EXPECT_EQ(hit_test_.CodeBlockButtonsHitTest(ctx).save_node, -1);
}
