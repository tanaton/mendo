#include <gtest/gtest.h>
#include "document_test_helpers.h"
#include "hit_test_service.h"
#include "mock_text_measurer.h"
#include "ui_constants.h"
#include <algorithm>
#include <utility>
#include <vector>

// OverlayButtonRect ユーティリティ関数と HitTestService::CopyButtonHitTest のテスト。
// MockTextMeasurer を使用するため DirectWrite / COM は不要。

class CopyButtonTest : public MockLayoutTestBase {
protected:
    HitTestService hit_test_;

    float ContentWidth() const
    {
        return theme_.ContentWidth(800.0f);
    }

    // コードブロックのコピーボタン中心をスクリーン座標で返す (dpi_scale=1, md_pane_left=0 前提)。
    // HitTest は dip_y = screen_y + scroll_y で逆変換する。
    std::pair<int, int> CopyBtnCenter(const ParsedLayout& pr, int node_index, float scroll_y = 0.0f) const
    {
        const float indent = NodeIndent(pr.nodes[node_index], theme_);
        const float x = theme_.margin_left + indent;
        const float w = ContentWidth() - indent;
        const float block_top = pr.cache.Top(node_index) - NodeBoxPadY(pr.nodes[node_index], theme_);
        const D2D1_RECT_F btn = OverlayButtonRect(x + w, block_top);
        return { static_cast<int>((btn.left + btn.right) * 0.5f),
                 static_cast<int>((btn.top + btn.bottom) * 0.5f - scroll_y) };
    }

    int CopyNodeAt(const ParsedLayout& pr, int x, int y, float scroll_y = 0.0f, float pane_h = 2000.0f)
    {
        return hit_test_.CodeBlockButtonsHitTest(
            { pr.nodes, pr.cache, theme_, scroll_y, 0.0f, 1.0f, x, y, ContentWidth(), pane_h }).copy_node;
    }
};

// ---- OverlayButtonRect ----

TEST_F(CopyButtonTest, CopyButtonRectHasCorrectSize)
{
    D2D1_RECT_F r = OverlayButtonRect(100.0f, 10.0f);
    float w = r.right - r.left;
    float h = r.bottom - r.top;
    EXPECT_FLOAT_EQ(w, COPY_BTN_SIZE);
    EXPECT_FLOAT_EQ(h, COPY_BTN_SIZE);
}

TEST_F(CopyButtonTest, CopyButtonRectIsInsideBlockTopRight)
{
    float block_right = 500.0f;
    float block_top = 100.0f;
    D2D1_RECT_F r = OverlayButtonRect(block_right, block_top);
    // ボタンの右端はブロック右端から COPY_BTN_MARGIN 内側
    EXPECT_FLOAT_EQ(r.right, block_right - COPY_BTN_MARGIN);
    // ボタンの上端はブロック上端から COPY_BTN_MARGIN 下
    EXPECT_FLOAT_EQ(r.top, block_top + COPY_BTN_MARGIN);
}

// ---- CopyButtonHitTest ----

TEST_F(CopyButtonTest, HitOnCopyButtonReturnsNodeIndex)
{
    auto pr = ParseAndLayout("```\nsome code\n```");
    const int code_idx = FindFirstNodeIndexByType(pr.nodes, NodeType::CodeBlock);
    ASSERT_GE(code_idx, 0) << "コードブロックノードが見つからない";

    auto [cx, cy] = CopyBtnCenter(pr, code_idx);
    EXPECT_EQ(CopyNodeAt(pr, cx, cy), code_idx);
}

TEST_F(CopyButtonTest, HitOutsideCopyButtonReturnsNegative)
{
    auto pr = ParseAndLayout("```\nsome code\n```");
    // 明らかにボタン外の座標（左上端）
    EXPECT_EQ(CopyNodeAt(pr, 5, 5), -1);
}

TEST_F(CopyButtonTest, NonCodeBlockReturnsNegative)
{
    auto pr = ParseAndLayout("Just a paragraph");
    // ドキュメント中央をクリック
    EXPECT_EQ(CopyNodeAt(pr, 400, 20), -1);
}

TEST_F(CopyButtonTest, MermaidBlockReturnsNegative)
{
    // ダイアグラムにはテキスト用コピーボタン(copy_node)は付かない (専用のダイアグラムコピーボタンは別)
    auto pr = ParseAndLayout("```mermaid\ngraph TD\n```");
    const int mermaid_idx = FindFirstNodeIndexByType(pr.nodes, NodeType::CodeBlock);
    ASSERT_GE(mermaid_idx, 0);
    auto [cx, cy] = CopyBtnCenter(pr, mermaid_idx);
    EXPECT_EQ(CopyNodeAt(pr, cx, cy), -1);
}

TEST_F(CopyButtonTest, LatexMathBlockReturnsNegative)
{
    // LatexMath も Mermaid 同様、テキスト用コピーボタン(copy_node)は付かない (専用のダイアグラムコピーボタンは別)
    auto pr = ParseAndLayout("$$E = mc^2$$");
    const int latex_idx = FindFirstNodeIndexByType(pr.nodes, NodeType::CodeBlock);
    ASSERT_GE(latex_idx, 0);
    ASSERT_EQ(pr.nodes[latex_idx].code_language(), SyntaxLanguage::LatexMath);
    auto [cx, cy] = CopyBtnCenter(pr, latex_idx);
    EXPECT_EQ(CopyNodeAt(pr, cx, cy), -1);
}

TEST_F(CopyButtonTest, MultipleCodeBlocksHitCorrectOne)
{
    auto pr = ParseAndLayout("```\nfirst\n```\n\n```\nsecond\n```");

    std::vector<int> code_indices;
    for (size_t i = 0; i < pr.nodes.size(); i++) {
        if (pr.nodes[i].type == NodeType::CodeBlock) {
            code_indices.emplace_back(static_cast<int>(i));
        }
    }
    ASSERT_GE(code_indices.size(), 2u) << "2つ以上のコードブロックが必要";

    auto [cx1, cy1] = CopyBtnCenter(pr, code_indices[0]);
    EXPECT_EQ(CopyNodeAt(pr, cx1, cy1), code_indices[0]);

    auto [cx2, cy2] = CopyBtnCenter(pr, code_indices[1]);
    EXPECT_EQ(CopyNodeAt(pr, cx2, cy2), code_indices[1]);
}

TEST_F(CopyButtonTest, EmptyDocumentReturnsNegative)
{
    auto pr = ParseAndLayout("");
    EXPECT_EQ(CopyNodeAt(pr, 400, 400), -1);
}

TEST_F(CopyButtonTest, ScrolledViewportHitTest)
{
    auto pr = ParseAndLayout(MakeParagraphs(30) + "```\nscrolled code\n```");
    const int code_idx = FindFirstNodeIndexByType(pr.nodes, NodeType::CodeBlock);
    ASSERT_GE(code_idx, 0);

    // コードブロックが見えるようにスクロール
    const float scroll_y = std::max(0.0f, pr.cache.Top(code_idx) - 50.0f);
    auto [sx, sy] = CopyBtnCenter(pr, code_idx, scroll_y);
    EXPECT_EQ(CopyNodeAt(pr, sx, sy, scroll_y, 600.0f), code_idx);
}
