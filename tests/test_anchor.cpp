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
