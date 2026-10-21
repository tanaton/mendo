#include <gtest/gtest.h>
#include "dwrite_test_base.h"
#include "syntax.h"
#include <algorithm>

class DWriteMeasurerTest : public DWriteTestBase {};

// 描画時にグリフランの文字位置とそのまま突き合わせられるよう、トークンは UTF-16 単位で持つ。
TEST_F(DWriteMeasurerTest, CodeBlockSyntaxTokensAreUtf16)
{
    auto [nodes, cache] = ParseAndLayout("```cpp\n// あい\nreturn 0;\n```\n");
    ASSERT_EQ(nodes.size(), 1u);
    const auto tokens = cache[0].syntax_tokens();
    ASSERT_FALSE(tokens.empty());

    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
    EXPECT_EQ(tokens[0].start, 0u);
    EXPECT_EQ(tokens[0].length, 6u); // "// あい" (UTF-8 で 9 byte、UTF-16 で 5) と続く改行
    const auto ret = std::ranges::find(tokens, SyntaxTokenType::Keyword, &SyntaxToken::type);
    ASSERT_NE(ret, tokens.end());
    EXPECT_EQ(ret->start, 6u);
    EXPECT_EQ(ret->length, 7u); // "return" と続く空白
}

// 空白・改行だけの隙間は直前のトークンに含め、同種ならまとめる。記号を挟む隙間はそのまま残す。
TEST_F(DWriteMeasurerTest, CodeBlockSyntaxTokensAbsorbBlankGaps)
{
    auto [nodes, cache] = ParseAndLayout("```java\npublic static\n  final int x;\nint y;\n```\n");
    ASSERT_EQ(nodes.size(), 1u);
    const auto tokens = cache[0].syntax_tokens();
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Keyword);
    EXPECT_EQ(tokens[0].start, 0u);
    EXPECT_EQ(tokens[0].length, 22u); // "public static\n  final "
    EXPECT_EQ(tokens[1].type, SyntaxTokenType::Type);
    EXPECT_EQ(tokens[1].start, 22u);
    EXPECT_EQ(tokens[1].length, 3u); // " x;\n" は記号を含むので伸ばさない
    EXPECT_EQ(tokens[2].type, SyntaxTokenType::Type);
    EXPECT_EQ(tokens[2].start, 29u);
}

// 左揃えで列幅に収まるセルは無限幅のまま使い、折り返す幅になったら列幅を付け、広がれば無限幅に戻す。
// 中央揃えは揃える基準が要るので常に列幅を付ける。高さは折り返しの有無に従う。
TEST_F(DWriteMeasurerTest, FittingLeftAlignedCellKeepsNaturalWidthLayout)
{
    auto [nodes, cache] = ParseAndLayout("| left | center |\n|---|:---:|\n| some words that wrap when narrow | other words that wrap when narrow |\n");
    ASSERT_EQ(nodes.size(), 1u);
    auto& entry = cache[0];
    const auto* tl = entry.table_layout();
    ASSERT_NE(tl, nullptr);
    auto* left = tl->GetCellLayout(1, 0);
    auto* center = tl->GetCellLayout(1, 1);
    ASSERT_NE(left, nullptr);
    ASSERT_NE(center, nullptr);
    const float wide_height = tl->cell_heights[tl->CellIndex(1, 0)];
    EXPECT_GE(left->GetMaxWidth(), 1.0e6f);
    EXPECT_LT(center->GetMaxWidth(), 1.0e6f);

    measurer_.MeasureNode(nodes[0], entry, 300.0f);
    EXPECT_LT(left->GetMaxWidth(), 1.0e6f) << "収まらない幅では列幅で折り返す";
    EXPECT_GT(tl->cell_heights[tl->CellIndex(1, 0)], wide_height);

    measurer_.MeasureNode(nodes[0], entry, 800.0f);
    EXPECT_GE(left->GetMaxWidth(), 1.0e6f);
    EXPECT_FLOAT_EQ(tl->cell_heights[tl->CellIndex(1, 0)], wide_height);
}
