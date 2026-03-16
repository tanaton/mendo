#include <gtest/gtest.h>
#include "parser.h"

TEST(AnchorId, LowercasePassthrough) {
    EXPECT_EQ(GenerateAnchorId(L"hello"), L"hello");
}

TEST(AnchorId, UppercaseToLowercase) {
    EXPECT_EQ(GenerateAnchorId(L"Hello World"), L"hello-world");
}

TEST(AnchorId, NumbersPreserved) {
    EXPECT_EQ(GenerateAnchorId(L"Step 1"), L"step-1");
}

TEST(AnchorId, HyphenPreserved) {
    EXPECT_EQ(GenerateAnchorId(L"well-known"), L"well-known");
}

TEST(AnchorId, UnderscorePreserved) {
    EXPECT_EQ(GenerateAnchorId(L"my_var"), L"my_var");
}

TEST(AnchorId, SpacesToHyphens) {
    EXPECT_EQ(GenerateAnchorId(L"a b c"), L"a-b-c");
}

TEST(AnchorId, TabsToHyphens) {
    EXPECT_EQ(GenerateAnchorId(L"a\tb"), L"a-b");
}

TEST(AnchorId, SpecialCharsStripped) {
    EXPECT_EQ(GenerateAnchorId(L"Hello, World!"), L"hello-world");
}

TEST(AnchorId, CjkCharactersPreserved) {
    EXPECT_EQ(GenerateAnchorId(L"見出しレベル2"), L"見出しレベル2");
}

TEST(AnchorId, MixedAsciiAndCjk) {
    EXPECT_EQ(GenerateAnchorId(L"Step 1: テスト"), L"step-1-テスト");
}

TEST(AnchorId, EmptyString) {
    EXPECT_EQ(GenerateAnchorId(L""), L"");
}

TEST(AnchorId, AllSpecialChars) {
    EXPECT_EQ(GenerateAnchorId(L"!@#$%^&*()"), L"");
}

TEST(AnchorId, MultipleSpacesMultipleHyphens) {
    EXPECT_EQ(GenerateAnchorId(L"a  b"), L"a--b");
}

// ---- Additional edge cases ----

TEST(AnchorId, FullWidthDigits) {
    // Full-width digits (０-９) are >= 0x3000, so they should be kept
    EXPECT_EQ(GenerateAnchorId(L"テスト０１"), L"テスト０１");
}

TEST(AnchorId, FullWidthParenthesesStripped) {
    // Full-width parentheses （U+FF08）and （U+FF09） should be stripped like GitHub
    EXPECT_EQ(GenerateAnchorId(L"テスト（サンプル）"), L"テストサンプル");
}

TEST(AnchorId, FullWidthPunctuationStripped) {
    // Full-width punctuation marks should be stripped
    EXPECT_EQ(GenerateAnchorId(L"見出し「補足」"), L"見出し補足");
}

TEST(AnchorId, MixedWhitespaceAndSpecialChars) {
    EXPECT_EQ(GenerateAnchorId(L"Hello!! World??"), L"hello-world");
}

TEST(AnchorId, OnlySpaces) {
    EXPECT_EQ(GenerateAnchorId(L"   "), L"---");
}

TEST(AnchorId, LeadingAndTrailingSpaces) {
    EXPECT_EQ(GenerateAnchorId(L" hello "), L"-hello-");
}

TEST(AnchorId, NumbersOnly) {
    EXPECT_EQ(GenerateAnchorId(L"123"), L"123");
}

TEST(AnchorId, HyphenAndUnderscore) {
    EXPECT_EQ(GenerateAnchorId(L"a-b_c"), L"a-b_c");
}

TEST(AnchorId, LongText) {
    std::wstring input(1000, L'A');
    auto result = GenerateAnchorId(input);
    EXPECT_EQ(result.size(), 1000u);
    // All should be lowercase 'a'
    for (wchar_t c : result) {
        EXPECT_EQ(c, L'a');
    }
}
