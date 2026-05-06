#include <gtest/gtest.h>
#include "document_utils.h"

TEST(AnchorId, LowercasePassthrough)
{
    EXPECT_EQ(GenerateAnchorId("hello"), "hello");
}

TEST(AnchorId, UppercaseToLowercase)
{
    EXPECT_EQ(GenerateAnchorId("Hello World"), "hello-world");
}

TEST(AnchorId, NumbersPreserved)
{
    EXPECT_EQ(GenerateAnchorId("Step 1"), "step-1");
}

TEST(AnchorId, HyphenPreserved)
{
    EXPECT_EQ(GenerateAnchorId("well-known"), "well-known");
}

TEST(AnchorId, UnderscorePreserved)
{
    EXPECT_EQ(GenerateAnchorId("my_var"), "my_var");
}

TEST(AnchorId, SpacesToHyphens)
{
    EXPECT_EQ(GenerateAnchorId("a b c"), "a-b-c");
}

TEST(AnchorId, TabsToHyphens)
{
    EXPECT_EQ(GenerateAnchorId("a\tb"), "a-b");
}

TEST(AnchorId, SpecialCharsStripped)
{
    EXPECT_EQ(GenerateAnchorId("Hello, World!"), "hello-world");
}

TEST(AnchorId, CjkCharactersPreserved)
{
    EXPECT_EQ(GenerateAnchorId("見出しレベル2"), "見出しレベル2");
}

TEST(AnchorId, MixedAsciiAndCjk)
{
    EXPECT_EQ(GenerateAnchorId("Step 1: テスト"), "step-1-テスト");
}

TEST(AnchorId, EmptyString)
{
    EXPECT_EQ(GenerateAnchorId(""), "");
}

TEST(AnchorId, AllSpecialChars)
{
    EXPECT_EQ(GenerateAnchorId("!@#$%^&*()"), "");
}

TEST(AnchorId, MultipleSpacesMultipleHyphens)
{
    EXPECT_EQ(GenerateAnchorId("a  b"), "a--b");
}

// ---- 追加のエッジケース ----

TEST(AnchorId, FullWidthDigits)
{
    // 全角数字（０-９）は0x3000以上なので保持される
    EXPECT_EQ(GenerateAnchorId("テスト０１"), "テスト０１");
}

TEST(AnchorId, FullWidthParenthesesStripped)
{
    // 全角括弧（U+FF08）と（U+FF09）はGitHubと同様に除去される
    EXPECT_EQ(GenerateAnchorId("テスト（サンプル）"), "テストサンプル");
}

TEST(AnchorId, FullWidthPunctuationStripped)
{
    // 全角句読点記号は除去される
    EXPECT_EQ(GenerateAnchorId("見出し「補足」"), "見出し補足");
}

TEST(AnchorId, MixedWhitespaceAndSpecialChars)
{
    EXPECT_EQ(GenerateAnchorId("Hello!! World??"), "hello-world");
}

TEST(AnchorId, OnlySpaces)
{
    EXPECT_EQ(GenerateAnchorId("   "), "---");
}

TEST(AnchorId, LeadingAndTrailingSpaces)
{
    EXPECT_EQ(GenerateAnchorId(" hello "), "-hello-");
}

TEST(AnchorId, NumbersOnly)
{
    EXPECT_EQ(GenerateAnchorId("123"), "123");
}

TEST(AnchorId, HyphenAndUnderscore)
{
    EXPECT_EQ(GenerateAnchorId("a-b_c"), "a-b_c");
}

TEST(AnchorId, LongText)
{
    std::string input(1000, 'A');
    auto result = GenerateAnchorId(input);
    EXPECT_EQ(result.size(), 1000u);
    // すべて小文字の'a'になるべき
    for (wchar_t c : result) {
        EXPECT_EQ(c, 'a');
    }
}
