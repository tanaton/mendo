#include <gtest/gtest.h>
#include "dwrite_test_base.h"
#include "syntax.h"
#include <algorithm>

class DWriteMeasurerTest : public DWriteTestBase {};

// 描画時にそのまま SetDrawingEffect へ渡せるよう、トークンは UTF-16 単位で持つ。
TEST_F(DWriteMeasurerTest, CodeBlockSyntaxTokensAreUtf16)
{
    auto [nodes, cache] = ParseAndLayout("```cpp\n// あい\nreturn 0;\n```\n");
    ASSERT_EQ(nodes.size(), 1u);
    const auto tokens = cache[0].syntax_tokens();
    ASSERT_FALSE(tokens.empty());

    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
    EXPECT_EQ(tokens[0].start, 0u);
    EXPECT_EQ(tokens[0].length, 5u); // "// あい" は UTF-8 で 9 byte、UTF-16 で 5
    const auto ret = std::ranges::find(tokens, SyntaxTokenType::Keyword, &SyntaxToken::type);
    ASSERT_NE(ret, tokens.end());
    EXPECT_EQ(ret->start, 6u);
    EXPECT_EQ(ret->length, 6u);
}
