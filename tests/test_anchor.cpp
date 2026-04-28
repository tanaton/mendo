#include <gtest/gtest.h>
#include "document_utils.h"

TEST(AnchorId, LowercasePassthrough)
{
    EXPECT_EQ(GenerateAnchorId(L"hello"), L"hello");
}

TEST(AnchorId, UppercaseToLowercase)
{
    EXPECT_EQ(GenerateAnchorId(L"Hello World"), L"hello-world");
}

TEST(AnchorId, NumbersPreserved)
{
    EXPECT_EQ(GenerateAnchorId(L"Step 1"), L"step-1");
}

TEST(AnchorId, HyphenPreserved)
{
    EXPECT_EQ(GenerateAnchorId(L"well-known"), L"well-known");
}

TEST(AnchorId, UnderscorePreserved)
{
    EXPECT_EQ(GenerateAnchorId(L"my_var"), L"my_var");
}

TEST(AnchorId, SpacesToHyphens)
{
    EXPECT_EQ(GenerateAnchorId(L"a b c"), L"a-b-c");
}

TEST(AnchorId, TabsToHyphens)
{
    EXPECT_EQ(GenerateAnchorId(L"a\tb"), L"a-b");
}

TEST(AnchorId, SpecialCharsStripped)
{
    EXPECT_EQ(GenerateAnchorId(L"Hello, World!"), L"hello-world");
}

TEST(AnchorId, CjkCharactersPreserved)
{
    EXPECT_EQ(GenerateAnchorId(L"見出しレベル2"), L"見出しレベル2");
}

TEST(AnchorId, MixedAsciiAndCjk)
{
    EXPECT_EQ(GenerateAnchorId(L"Step 1: テスト"), L"step-1-テスト");
}

TEST(AnchorId, EmptyString)
{
    EXPECT_EQ(GenerateAnchorId(L""), L"");
}

TEST(AnchorId, AllSpecialChars)
{
    EXPECT_EQ(GenerateAnchorId(L"!@#$%^&*()"), L"");
}

TEST(AnchorId, MultipleSpacesMultipleHyphens)
{
    EXPECT_EQ(GenerateAnchorId(L"a  b"), L"a--b");
}

// ---- 追加のエッジケース ----

TEST(AnchorId, FullWidthDigits)
{
    // 全角数字（０-９）は0x3000以上なので保持される
    EXPECT_EQ(GenerateAnchorId(L"テスト０１"), L"テスト０１");
}

TEST(AnchorId, FullWidthParenthesesStripped)
{
    // 全角括弧（U+FF08）と（U+FF09）はGitHubと同様に除去される
    EXPECT_EQ(GenerateAnchorId(L"テスト（サンプル）"), L"テストサンプル");
}

TEST(AnchorId, FullWidthPunctuationStripped)
{
    // 全角句読点記号は除去される
    EXPECT_EQ(GenerateAnchorId(L"見出し「補足」"), L"見出し補足");
}

TEST(AnchorId, MixedWhitespaceAndSpecialChars)
{
    EXPECT_EQ(GenerateAnchorId(L"Hello!! World??"), L"hello-world");
}

TEST(AnchorId, OnlySpaces)
{
    EXPECT_EQ(GenerateAnchorId(L"   "), L"---");
}

TEST(AnchorId, LeadingAndTrailingSpaces)
{
    EXPECT_EQ(GenerateAnchorId(L" hello "), L"-hello-");
}

TEST(AnchorId, NumbersOnly)
{
    EXPECT_EQ(GenerateAnchorId(L"123"), L"123");
}

TEST(AnchorId, HyphenAndUnderscore)
{
    EXPECT_EQ(GenerateAnchorId(L"a-b_c"), L"a-b_c");
}

TEST(AnchorId, LongText)
{
    std::wstring input(1000, L'A');
    auto result = GenerateAnchorId(input);
    EXPECT_EQ(result.size(), 1000u);
    // すべて小文字の'a'になるべき
    for (wchar_t c : result) {
        EXPECT_EQ(c, L'a');
    }
}
