#include <gtest/gtest.h>
#include "document_utils.h"

TEST(AnchorId, LowercasePassthrough)
{
    EXPECT_EQ(GenerateAnchorId(MENDO_LIT("hello")), MENDO_LIT("hello"));
}

TEST(AnchorId, UppercaseToLowercase)
{
    EXPECT_EQ(GenerateAnchorId(MENDO_LIT("Hello World")), MENDO_LIT("hello-world"));
}

TEST(AnchorId, NumbersPreserved)
{
    EXPECT_EQ(GenerateAnchorId(MENDO_LIT("Step 1")), MENDO_LIT("step-1"));
}

TEST(AnchorId, HyphenPreserved)
{
    EXPECT_EQ(GenerateAnchorId(MENDO_LIT("well-known")), MENDO_LIT("well-known"));
}

TEST(AnchorId, UnderscorePreserved)
{
    EXPECT_EQ(GenerateAnchorId(MENDO_LIT("my_var")), MENDO_LIT("my_var"));
}

TEST(AnchorId, SpacesToHyphens)
{
    EXPECT_EQ(GenerateAnchorId(MENDO_LIT("a b c")), MENDO_LIT("a-b-c"));
}

TEST(AnchorId, TabsToHyphens)
{
    EXPECT_EQ(GenerateAnchorId(MENDO_LIT("a\tb")), MENDO_LIT("a-b"));
}

TEST(AnchorId, SpecialCharsStripped)
{
    EXPECT_EQ(GenerateAnchorId(MENDO_LIT("Hello, World!")), MENDO_LIT("hello-world"));
}

TEST(AnchorId, CjkCharactersPreserved)
{
    EXPECT_EQ(GenerateAnchorId(MENDO_LIT("見出しレベル2")), MENDO_LIT("見出しレベル2"));
}

TEST(AnchorId, MixedAsciiAndCjk)
{
    EXPECT_EQ(GenerateAnchorId(MENDO_LIT("Step 1: テスト")), MENDO_LIT("step-1-テスト"));
}

TEST(AnchorId, EmptyString)
{
    EXPECT_EQ(GenerateAnchorId(MENDO_LIT("")), MENDO_LIT(""));
}

TEST(AnchorId, AllSpecialChars)
{
    EXPECT_EQ(GenerateAnchorId(MENDO_LIT("!@#$%^&*()")), MENDO_LIT(""));
}

TEST(AnchorId, MultipleSpacesMultipleHyphens)
{
    EXPECT_EQ(GenerateAnchorId(MENDO_LIT("a  b")), MENDO_LIT("a--b"));
}

// ---- 追加のエッジケース ----

TEST(AnchorId, FullWidthDigits)
{
    // 全角数字（０-９）は0x3000以上なので保持される
    EXPECT_EQ(GenerateAnchorId(MENDO_LIT("テスト０１")), MENDO_LIT("テスト０１"));
}

TEST(AnchorId, FullWidthParenthesesStripped)
{
    // 全角括弧（U+FF08）と（U+FF09）はGitHubと同様に除去される
    EXPECT_EQ(GenerateAnchorId(MENDO_LIT("テスト（サンプル）")), MENDO_LIT("テストサンプル"));
}

TEST(AnchorId, FullWidthPunctuationStripped)
{
    // 全角句読点記号は除去される
    EXPECT_EQ(GenerateAnchorId(MENDO_LIT("見出し「補足」")), MENDO_LIT("見出し補足"));
}

TEST(AnchorId, MixedWhitespaceAndSpecialChars)
{
    EXPECT_EQ(GenerateAnchorId(MENDO_LIT("Hello!! World??")), MENDO_LIT("hello-world"));
}

TEST(AnchorId, OnlySpaces)
{
    EXPECT_EQ(GenerateAnchorId(MENDO_LIT("   ")), MENDO_LIT("---"));
}

TEST(AnchorId, LeadingAndTrailingSpaces)
{
    EXPECT_EQ(GenerateAnchorId(MENDO_LIT(" hello ")), MENDO_LIT("-hello-"));
}

TEST(AnchorId, NumbersOnly)
{
    EXPECT_EQ(GenerateAnchorId(MENDO_LIT("123")), MENDO_LIT("123"));
}

TEST(AnchorId, HyphenAndUnderscore)
{
    EXPECT_EQ(GenerateAnchorId(MENDO_LIT("a-b_c")), MENDO_LIT("a-b_c"));
}

TEST(AnchorId, LongText)
{
    mendo::doc_string_std input(1000, MENDO_LIT('A'));
    auto result = GenerateAnchorId(input);
    EXPECT_EQ(result.size(), 1000u);
    // すべて小文字の'a'になるべき
    for (wchar_t c : result) {
        EXPECT_EQ(c, MENDO_LIT('a'));
    }
}
