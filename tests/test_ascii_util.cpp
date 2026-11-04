#include <gtest/gtest.h>
#include "ascii_util.h"
#include <string>
#include <string_view>
#include <utility>

// ── Find ──────────────────────────────────────────────────────

// 1 バイトのクエリも 2 点一致の経路を通る。16 バイトのブロックと端数のどちらの位置でも見つかる。
TEST(SimdAsciiFindTest, SingleByteAtEveryPosition)
{
    for (size_t len = 1; len <= 40; ++len) {
        for (size_t at = 0; at < len; ++at) {
            std::string t(len, '.');
            t[at] = 'x';
            EXPECT_EQ(ascii_util::Find(t, "x", 0), at) << len << " " << at;
            t[at] = 'X';
            EXPECT_EQ(ascii_util::FindAsciiCaseInsensitive(t, "x", 0), at) << len << " " << at;
            EXPECT_EQ(ascii_util::Find(t, "x", 0), ascii_util::npos) << len << " " << at;
            EXPECT_EQ(ascii_util::Find(t, "X", at + 1), ascii_util::npos) << len << " " << at;
        }
    }
}

// ── iequal / istarts_with ─────────────────────────────────────

TEST(AsciiUtilIequal, CharAndWideLiterals)
{
    EXPECT_TRUE(ascii_util::iequal(std::string_view{ "MerMaid" }, "mermaid"));
    EXPECT_FALSE(ascii_util::iequal(std::string_view{ "mermai" }, "mermaid"));
    EXPECT_TRUE(ascii_util::iequal(std::wstring_view{ L".TXT" }, L".txt"));
    EXPECT_FALSE(ascii_util::iequal(std::wstring_view{ L".txt2" }, L".txt"));
}

TEST(AsciiUtilIequal, IstartsWith)
{
    EXPECT_TRUE(ascii_util::istarts_with("HTTPS://example.com", "https://"));
    EXPECT_TRUE(ascii_util::istarts_with("rem", "rem"));
    EXPECT_FALSE(ascii_util::istarts_with("re", "rem"));
    EXPECT_FALSE(ascii_util::istarts_with("ftp://x", "https://"));
}

// ─────────────────────────────────────────────
// Find (先頭+末尾 2 点フィルタ) / FindAsciiCaseInsensitive
// ─────────────────────────────────────────────

namespace {

// 各開始位置 (末尾の 1 つ先まで) から参照実装 (std::string_view::find) と一致するかを総当たりで確かめる。
void ExpectFindMatchesReference(std::string_view text, std::string_view query)
{
    const auto lower_text = ascii_util::ToLowerAsciiCopy(text);
    const auto lower_query = ascii_util::ToLowerAsciiCopy(query);
    for (size_t start = 0; start <= text.size() + 1; ++start) {
        const size_t expected = text.find(query, start);
        EXPECT_EQ(ascii_util::Find(text, query, start), expected == std::string_view::npos ? ascii_util::npos : expected)
            << "start=" << start << " query=" << query;
        const size_t expected_ci = std::string_view(lower_text).find(lower_query, start);
        EXPECT_EQ(ascii_util::FindAsciiCaseInsensitive(text, lower_query, start), expected_ci == std::string_view::npos ? ascii_util::npos : expected_ci)
            << "start=" << start << " query=" << query;
    }
}

} // namespace

TEST(AsciiUtilFind, MatchesReferenceAcrossSimdBoundaries)
{
    std::string text;
    for (int i = 0; i < 6; ++i) {
        text += "abXab Cab-abc ABC xyzAb \xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88 ";
    }
    for (const std::string_view q : { "a", "ab", "abc", "ABC", "ab ", "Ab", "xyzab", "\xE3\x83\x86\xE3\x82\xB9", "\xE3\x83\x88 ab", "zzz" }) {
        ExpectFindMatchesReference(text, q);
    }
}

// 空のクエリ・空の文字列・クエリより短い文字列、先頭だけ一致する候補、16 byte の境目をまたぐ一致、重なる一致、
// 末尾に収まらない開始位置。
TEST(AsciiUtilFind, MatchesReferenceForEdgeCases)
{
    std::string crossing(40, 'a');
    crossing.replace(15, 3, "xyz");
    const std::string repeated(18, 'a');
    const std::pair<std::string_view, std::string_view> kCases[] = {
        { "hello", "" },
        { "", "a" },
        { "abc", "abcd" },
        { "ABC", "abcd" },
        { "abXabXabXabXabcXabc", "abc" },
        { crossing, "xyz" },
        { crossing, "axyz" },
        { repeated, "aa" },
        { repeated, "aaa" },
        { "hello", "lo" },
    };
    for (const auto& [text, query] : kCases) {
        SCOPED_TRACE(text);
        ExpectFindMatchesReference(text, query);
    }
}

TEST(AsciiUtilFind, CaseInsensitiveDoesNotFoldNonAscii)
{
    // 'Ä' (C3 84) と 'ä' (C3 A4) は ASCII 畳み込みの対象外
    EXPECT_EQ(ascii_util::FindAsciiCaseInsensitive("x\xC3\x84y", "\xC3\xA4", 0), ascii_util::npos);
    EXPECT_EQ(ascii_util::FindAsciiCaseInsensitive("x\xC3\x84Y", "\xC3\x84y", 0), 1u);
}

// SIMD の大文字の範囲の境目と UTF-8 の継続バイトを畳まない。1 バイトのクエリは先頭の比較を SIMD だけで決める。
TEST(AsciiUtilFind, CaseInsensitiveFoldsOnlyUppercaseRange)
{
    const std::string at_signs(32, '@');
    const std::string after_z(32, '[');
    EXPECT_EQ(ascii_util::FindAsciiCaseInsensitive(at_signs, "`", 0), ascii_util::npos);
    EXPECT_EQ(ascii_util::FindAsciiCaseInsensitive(after_z, "{", 0), ascii_util::npos);
    // 「あいうえお一二三四」。継続バイト 0x81〜0x8C に 0x20 を足した値と一致させない
    const std::string_view kana = "\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86\xE3\x81\x88\xE3\x81\x8A"
                                  "\xE4\xB8\x80\xE4\xBA\x8C\xE4\xB8\x89\xE5\x9B\x9B";
    for (const std::string_view q : { "\xA1", "\xA2", "\xA4", "\xAC" }) {
        EXPECT_EQ(ascii_util::FindAsciiCaseInsensitive(kana, q, 0), ascii_util::npos);
    }
    EXPECT_EQ(ascii_util::FindAsciiCaseInsensitive(after_z + "Z", "z", 0), 32u);
}

TEST(AsciiUtilFind, HasAsciiLetter)
{
    EXPECT_TRUE(ascii_util::HasAsciiLetter("abc"));
    EXPECT_TRUE(ascii_util::HasAsciiLetter("1Z2"));
    EXPECT_FALSE(ascii_util::HasAsciiLetter("123 -_"));
    EXPECT_FALSE(ascii_util::HasAsciiLetter("\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88"));
}

TEST(ToLowerAsciiCopy, LowersOnlyAsciiLetters)
{
    struct Case {
        std::string_view in;
        std::string_view expected;
    };
    constexpr Case kCases[] = {
        { "", "" },
        { "HELLO", "hello" },
        { "hello", "hello" },
        { "HeLLo WoRLd", "hello world" },
        { "日本語", "日本語" },
        { "ABC-123_XYZ", "abc-123_xyz" },
        // A(0x41) の直前 @(0x40) と Z(0x5A) の直後 [(0x5B) は変換しない
        { "@A[Z", "@a[z" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(c.in));
        EXPECT_EQ(ascii_util::ToLowerAsciiCopy(c.in), c.expected);
    }
}
