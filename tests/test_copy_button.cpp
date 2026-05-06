#include <gtest/gtest.h>
#include "hit_test_service.h"
#include "ui_constants.h"
#include "layout.h"
#include "mock_text_measurer.h"
#include "parser.h"

// OverlayButtonRect ユーティリティ関数と HitTestService::CopyButtonHitTest のテスト。
// MockTextMeasurer を使用するため DirectWrite / COM は不要。

class CopyButtonTest : public ::testing::Test {
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

    // Markdown をパースしてレイアウトを計算するヘルパー
    ParsedLayout Parse(std::string_view md, float viewport_w = 800.0f)
    {
        ParsedLayout r;
        r.nodes = ParseMarkdown(md).nodes;
        r.cache.Resize(r.nodes.size());
        engine_.ComputeLayout(r.nodes, r.cache, viewport_w);
        return r;
    }

    // コードブロックのコピーボタン中心座標をスクリーンピクセルで返すヘルパー
    // dpi_scale=1, md_pane_left=0 前提
    std::pair<int, int> CopyBtnCenter(const ParsedLayout& pr, int node_index, float viewport_w = 800.0f)
    {
        const auto& node = pr.nodes[node_index];
        const auto& entry = pr.cache[node_index];
        float indent = node.indent_level * theme_.indent_width;
        float content_width = viewport_w - theme_.margin_left - theme_.margin_right;
        float x = theme_.margin_left + indent;
        float w = content_width - indent;
        float pad = theme_.code_block_padding;
        float block_right = x + w;
        float block_top = entry.text_top - pad;
        D2D1_RECT_F btn = OverlayButtonRect(block_right, block_top);
        float cx = (btn.left + btn.right) * 0.5f;
        float cy = (btn.top + btn.bottom) * 0.5f;
        // scroll_y=0, dpi=1 のため dip == pixel。ただし HitTest は dip_y = screen_y + scroll_y なので
        // screen_y = dip_y - scroll_y = dip_y (scroll_y=0)
        return { static_cast<int>(cx), static_cast<int>(cy) };
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
    auto pr = Parse("```\nsome code\n```");
    // コードブロックのノードインデックスを特定
    int code_idx = -1;
    for (size_t i = 0; i < pr.nodes.size(); i++) {
        if (pr.nodes[i].type == NodeType::CodeBlock) {
            code_idx = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(code_idx, 0) << "コードブロックノードが見つからない";

    auto [cx, cy] = CopyBtnCenter(pr, code_idx);
    float content_width = 800.0f - theme_.margin_left - theme_.margin_right;
    int result = hit_test_.CopyButtonHitTest(
        { pr.nodes, pr.cache, theme_, 0.0f, 0.0f, 1.0f, cx, cy, content_width, 2000.0f });
    EXPECT_EQ(result, code_idx);
}

TEST_F(CopyButtonTest, HitOutsideCopyButtonReturnsNegative)
{
    auto pr = Parse("```\nsome code\n```");
    float content_width = 800.0f - theme_.margin_left - theme_.margin_right;
    // 明らかにボタン外の座標（左上端）
    int result = hit_test_.CopyButtonHitTest(
        { pr.nodes, pr.cache, theme_, 0.0f, 0.0f, 1.0f, 5, 5, content_width, 2000.0f });
    EXPECT_EQ(result, -1);
}

TEST_F(CopyButtonTest, NonCodeBlockReturnsNegative)
{
    auto pr = Parse("Just a paragraph");
    float content_width = 800.0f - theme_.margin_left - theme_.margin_right;
    // ドキュメント中央をクリック
    int result = hit_test_.CopyButtonHitTest(
        { pr.nodes, pr.cache, theme_, 0.0f, 0.0f, 1.0f, 400, 20, content_width, 2000.0f });
    EXPECT_EQ(result, -1);
}

TEST_F(CopyButtonTest, MermaidBlockReturnsNegative)
{
    auto pr = Parse("```mermaid\ngraph TD\n```");
    float content_width = 800.0f - theme_.margin_left - theme_.margin_right;
    // Mermaidブロックがある場合でもコピーボタンは無効
    int mermaid_idx = -1;
    for (size_t i = 0; i < pr.nodes.size(); i++) {
        if (pr.nodes[i].type == NodeType::CodeBlock) {
            mermaid_idx = static_cast<int>(i);
            break;
        }
    }
    if (mermaid_idx >= 0) {
        auto [cx, cy] = CopyBtnCenter(pr, mermaid_idx);
        int result = hit_test_.CopyButtonHitTest(
            { pr.nodes, pr.cache, theme_, 0.0f, 0.0f, 1.0f, cx, cy, content_width, 2000.0f });
        EXPECT_EQ(result, -1);
    }
}

TEST_F(CopyButtonTest, LatexMathBlockReturnsNegative)
{
    // LatexMath ブロックも Mermaid 同様コピーボタン非対応
    auto pr = Parse("$$E = mc^2$$");
    float content_width = 800.0f - theme_.margin_left - theme_.margin_right;
    int latex_idx = -1;
    for (size_t i = 0; i < pr.nodes.size(); i++) {
        if (pr.nodes[i].type == NodeType::CodeBlock &&
            pr.nodes[i].code_language == SyntaxLanguage::LatexMath) {
            latex_idx = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(latex_idx, 0);
    auto [cx, cy] = CopyBtnCenter(pr, latex_idx);
    int result = hit_test_.CopyButtonHitTest(
        { pr.nodes, pr.cache, theme_, 0.0f, 0.0f, 1.0f, cx, cy, content_width, 2000.0f });
    EXPECT_EQ(result, -1);
}

TEST_F(CopyButtonTest, MultipleCodeBlocksHitCorrectOne)
{
    auto pr = Parse("```\nfirst\n```\n\n```\nsecond\n```");
    float content_width = 800.0f - theme_.margin_left - theme_.margin_right;

    // すべてのコードブロックインデックスを収集
    std::vector<int> code_indices;
    for (size_t i = 0; i < pr.nodes.size(); i++) {
        if (pr.nodes[i].type == NodeType::CodeBlock) {
            code_indices.emplace_back(static_cast<int>(i));
        }
    }
    ASSERT_GE(code_indices.size(), 2u) << "2つ以上のコードブロックが必要";

    // 1つ目のコピーボタンをクリック
    auto [cx1, cy1] = CopyBtnCenter(pr, code_indices[0]);
    int r1 = hit_test_.CopyButtonHitTest(
        { pr.nodes, pr.cache, theme_, 0.0f, 0.0f, 1.0f, cx1, cy1, content_width, 2000.0f });
    EXPECT_EQ(r1, code_indices[0]);

    // 2つ目のコピーボタンをクリック
    auto [cx2, cy2] = CopyBtnCenter(pr, code_indices[1]);
    int r2 = hit_test_.CopyButtonHitTest(
        { pr.nodes, pr.cache, theme_, 0.0f, 0.0f, 1.0f, cx2, cy2, content_width, 2000.0f });
    EXPECT_EQ(r2, code_indices[1]);
}

TEST_F(CopyButtonTest, EmptyDocumentReturnsNegative)
{
    auto pr = Parse("");
    float content_width = 800.0f - theme_.margin_left - theme_.margin_right;
    int result = hit_test_.CopyButtonHitTest(
        { pr.nodes, pr.cache, theme_, 0.0f, 0.0f, 1.0f, 400, 400, content_width, 2000.0f });
    EXPECT_EQ(result, -1);
}

TEST_F(CopyButtonTest, ScrolledViewportHitTest)
{
    // 多くの段落の後にコードブロックを配置
    std::string md;
    for (int i = 0; i < 30; i++)
        md += "Paragraph " + std::to_string(i) + "\n\n";
    md += "```\nscrolled code\n```";
    auto pr = Parse(md);

    int code_idx = -1;
    for (size_t i = 0; i < pr.nodes.size(); i++) {
        if (pr.nodes[i].type == NodeType::CodeBlock) {
            code_idx = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(code_idx, 0);

    float content_width = 800.0f - theme_.margin_left - theme_.margin_right;
    // コードブロックが見えるようにスクロール
    float scroll_y = pr.cache[code_idx].text_top - 50.0f;
    if (scroll_y < 0)
        scroll_y = 0;

    // コピーボタンの座標を計算（スクロール後のスクリーン座標）
    const auto& node = pr.nodes[code_idx];
    const auto& entry = pr.cache[code_idx];
    float indent = node.indent_level * theme_.indent_width;
    float cw = content_width - indent;
    float x = theme_.margin_left + indent;
    float pad = theme_.code_block_padding;
    D2D1_RECT_F btn = OverlayButtonRect(x + cw, entry.text_top - pad);
    // screen_y = dip_y - scroll_y (dpi=1)
    int sx = static_cast<int>((btn.left + btn.right) * 0.5f);
    int sy = static_cast<int>((btn.top + btn.bottom) * 0.5f - scroll_y);

    int result = hit_test_.CopyButtonHitTest(
        { pr.nodes, pr.cache, theme_, scroll_y, 0.0f, 1.0f, sx, sy, content_width, 600.0f });
    EXPECT_EQ(result, code_idx);
}
