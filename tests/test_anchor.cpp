#include <gtest/gtest.h>
#include "document_utils.h"
#include <string>
#include <string_view>

namespace {

struct AnchorCase {
    const char* name;
    std::string_view input;
    std::string_view expected;
};

constexpr AnchorCase kAnchorCases[] = {
    { "LowercasePassthrough", "hello", "hello" },
    { "UppercaseToLowercase", "Hello World", "hello-world" },
    { "NumbersPreserved", "Step 1", "step-1" },
    { "HyphenPreserved", "well-known", "well-known" },
    { "UnderscorePreserved", "my_var", "my_var" },
    { "SpacesToHyphens", "a b c", "a-b-c" },
    { "TabsToHyphens", "a\tb", "a-b" },
    { "SpecialCharsStripped", "Hello, World!", "hello-world" },
    { "CjkCharactersPreserved", "見出しレベル2", "見出しレベル2" },
    // U+0080–U+2FFF の文字 (アクセント付きラテン等) が脱落しないこと。
    // ASCII 以外は小文字化されない (FindAnchorIndex 側も ASCII のみ小文字化)。
    { "LatinAccentPreserved", "Café", "café" },
    { "CyrillicPreserved", "Привет", "Привет" },
    // 一般句読点 (U+2000–U+206F) は記号として除外される
    { "GeneralPunctuationStripped", "em—dash…", "emdash" },
    { "MixedAsciiAndCjk", "Step 1: テスト", "step-1-テスト" },
    { "EmptyString", "", "" },
    { "AllSpecialChars", "!@#$%^&*()", "" },
    { "MultipleSpacesMultipleHyphens", "a  b", "a--b" },
    // 全角数字（０-９）は0x3000以上なので保持される
    { "FullWidthDigits", "テスト０１", "テスト０１" },
    // 全角括弧（U+FF08）と（U+FF09）はGitHubと同様に除去される
    { "FullWidthParenthesesStripped", "テスト（サンプル）", "テストサンプル" },
    { "FullWidthPunctuationStripped", "見出し「補足」", "見出し補足" },
    { "MixedWhitespaceAndSpecialChars", "Hello!! World??", "hello-world" },
    { "OnlySpaces", "   ", "---" },
    { "LeadingAndTrailingSpaces", " hello ", "-hello-" },
    { "NumbersOnly", "123", "123" },
    { "HyphenAndUnderscore", "a-b_c", "a-b_c" },
};

} // namespace

TEST(AnchorId, GeneratesExpectedId)
{
    for (const auto& c : kAnchorCases) {
        SCOPED_TRACE(c.name);
        EXPECT_EQ(GenerateAnchorId(c.input), c.expected);
    }
}

TEST(AnchorId, LongText)
{
    const auto id = GenerateAnchorId(std::string(1000, 'A'));
    EXPECT_EQ(std::string_view(id), std::string(1000, 'a'));
}
