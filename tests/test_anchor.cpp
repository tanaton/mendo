#include <gtest/gtest.h>
#include "anchor.h"
#include "document_test_helpers.h"
#include "document_utils.h"
#include "unicode_lower_table.h"
#include "utf8_codec.h"
#include <array>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
    // U+0080–U+2FFF の文字 (アクセント付きラテン等) が脱落せず、GitHub と同じく小文字になること。
    { "LatinAccentPreserved", "Café", "café" },
    { "LatinAccentLowered", "Über Ŷ", "über-ŷ" },
    { "CyrillicLowered", "Привет Ёж", "привет-ёж" },
    { "GreekLowered", "Ωμέγα Άλφα", "ωμέγα-άλφα" },
    { "FullWidthLatinLowered", "ＡＢＣ", "ａｂｃ" },
    { "ArmenianLowered", "Բարեւ", "բարեւ" },
    { "CherokeeLowered", "Ꭰ", "ꭰ" },
    { "SupplementaryPlaneLowered", "𐐀", "𐐨" },
    // U+0130 は単純小文字の i にする (toLowerCase の i + U+0307 は UTF-8 長が伸びるので扱わない)
    { "DottedCapitalISimpleLowercase", "İ", "i" },
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

// 出力は入力と同じ長さで確保して書くので、表に UTF-8 長が伸びる変換が入ると確保を越えて書く。
// CJK 帯 [U+3000, U+A640) は表を引かずに返すので、その帯に大文字が入ると小文字化を取りこぼす。
TEST(AnchorId, LowerTableKeepsUtf8LengthAndSkipsCjkBand)
{
    for (const auto& m : kUnicodeLowerMappings) {
        char upper[4];
        char lower[4];
        EXPECT_LE(utf8_codec::EncodeCp(m.lower, lower), utf8_codec::EncodeCp(m.upper, upper)) << std::hex << m.upper;
        EXPECT_FALSE(m.upper >= 0x3000 && m.upper < 0xA640) << std::hex << m.upper;
    }
    // 帯の上端 U+A640 は表を引く側 (Ꙁ → ꙁ)
    EXPECT_EQ(GenerateAnchorId("\xEA\x99\x80"), "\xEA\x99\x81");
    EXPECT_EQ(ToLowerAnchorCopy("\xEA\x99\x80"), "\xEA\x99\x81");
}

// 不正・途中で切れた UTF-8 はバイトのまま残し、末尾を越えて読まない (どの見出しとも一致させない)。
TEST(AnchorId, ToLowerAnchorCopyKeepsMalformedBytes)
{
    EXPECT_EQ(ToLowerAnchorCopy("A\xC3"), "a\xC3");
    EXPECT_EQ(ToLowerAnchorCopy("\xFF\xC3\x9C"), "\xFF\xC3\xBC");
    EXPECT_EQ(ToLowerAnchorCopy("\xE3\x81Z"), "\xE3\x81z");
}

TEST(AnchorId, LongText)
{
    const auto id = GenerateAnchorId(std::string(1000, 'A'));
    EXPECT_EQ(std::string_view(id), std::string(1000, 'a'));
}

// ---- HeadingAnchorIndex (見出しへのアンカー割り当て) ----

namespace {

std::vector<std::string> AssignAnchors(std::initializer_list<std::string_view> headings)
{
    HeadingAnchorIndex index;
    std::vector<std::string> anchors;
    int node = 0;
    for (const auto text : headings) {
        anchors.emplace_back(index.Add(text, node++));
    }
    return anchors;
}

} // namespace

TEST(HeadingAnchorIndex, DuplicateHeadingsGetSequentialSuffix)
{
    EXPECT_EQ(AssignAnchors({ "Title", "Title", "Title" }), (std::vector<std::string>{ "title", "title-1", "title-2" }));
    EXPECT_EQ(AssignAnchors({ "A", "B", "A" }), (std::vector<std::string>{ "a", "b", "a-1" }));
}

// 連番付きスラグと本文由来のスラグの衝突 (github-slugger と同じく未使用になるまで連番を進める)
TEST(HeadingAnchorIndex, SuffixSkipsSlugTakenByLiteralHeading)
{
    EXPECT_EQ(AssignAnchors({ "A", "A", "A-1" }), (std::vector<std::string>{ "a", "a-1", "a-1-1" }));
    EXPECT_EQ(AssignAnchors({ "A-1", "A", "A" }), (std::vector<std::string>{ "a-1", "a", "a-2" }));
}

TEST(HeadingAnchorIndex, FindReturnsNodeOfRegisteredHeading)
{
    HeadingAnchorIndex index;
    index.Add("コードブロック", 3);
    index.Add("コードブロック", 7);
    EXPECT_EQ(index.Find("コードブロック"), 3);
    EXPECT_EQ(index.Find("コードブロック-1"), 7);
    EXPECT_EQ(index.Find("missing"), -1);
    EXPECT_EQ(index.heading_count(), 2u);
}

namespace {

constexpr std::array<int32_t, 2> kDefs{ 10, 11 };
constexpr std::array<FootnoteRef, 3> kRefs{ {
    { 1, 1, 2 },
    { 2, 1, 4 },
    { 1, 2, 7 },
} };

int FindFootnote(std::string_view anchor)
{
    return FindFootnoteAnchor(anchor, kDefs, kRefs);
}

} // namespace

TEST(FootnoteAnchor, FormatsGitHubCompatibleUrls)
{
    EXPECT_EQ(FootnoteDefUrl(12), "#user-content-fn-12");
    EXPECT_EQ(FootnoteRefUrl(3, 1), "#user-content-fnref-3");
    EXPECT_EQ(FootnoteRefUrl(3, 2), "#user-content-fnref-3-2");
}

TEST(FootnoteAnchor, FindsDefinition)
{
    EXPECT_EQ(FindFootnote("user-content-fn-1"), 10);
    EXPECT_EQ(FindFootnote("user-content-fn-2"), 11);
    EXPECT_EQ(FindFootnote("user-content-fn-3"), -1);
    EXPECT_EQ(FindFootnote("user-content-fn-0"), -1);
}

TEST(FootnoteAnchor, FindsReference)
{
    EXPECT_EQ(FindFootnote("user-content-fnref-1"), 2);
    EXPECT_EQ(FindFootnote("user-content-fnref-2"), 4);
    EXPECT_EQ(FindFootnote("user-content-fnref-1-2"), 7);
    EXPECT_EQ(FindFootnote("user-content-fnref-1-3"), -1);
}

TEST(FootnoteAnchor, RejectsMalformedAnchors)
{
    for (const char* anchor : { "", "fn-1", "user-content-fn-", "user-content-fn-1x", "user-content-fn--1", "user-content-fnref-",
                                "user-content-fnref-1-", "user-content-fnref-a", "user-content-fn-99999999999" }) {
        EXPECT_EQ(FindFootnote(anchor), -1) << anchor;
    }
}
