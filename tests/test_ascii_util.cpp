#include <gtest/gtest.h>
#include "ascii_util.h"
#include <string>
#include <string_view>

namespace {

std::string ScalarAsciiToLower(std::string_view s)
{
    std::string r;
    r.reserve(s.size());
    for (char ch : s) {
        r.push_back((ch >= 'A' && ch <= 'Z')
            ? static_cast<char>(ch - 'A' + 'a')
            : ch);
    }
    return r;
}

} // namespace

// ── AsciiToLowerOnly ───────────────────────────────────────────

TEST(SimdAsciiAsciiToLowerOnlyTest, Mixed)
{
    const std::string src = "HELLO_World 123 \xE3\x81\x82"; // UTF-8 の「あ」を含む
    std::string dst(src.size(), '\0');
    ascii_util::AsciiToLowerOnly(src.data(), dst.data(), src.size());
    EXPECT_EQ(dst, ScalarAsciiToLower(src));
    // 非 ASCII (UTF-8 multi-byte) はそのまま
    EXPECT_EQ(dst.substr(dst.size() - 3), "\xE3\x81\x82");
}

TEST(SimdAsciiAsciiToLowerOnlyTest, VariousLengths)
{
    // SSE2 16 byte 境界・端数の両方を網羅する
    const std::string_view base = "ZyxWvuTsrQpoNmlKjiHgfEdcBa_KEYWORD";
    for (size_t n = 0; n <= base.size(); ++n) {
        std::string src(base.substr(0, n));
        std::string dst(n, '\0');
        ascii_util::AsciiToLowerOnly(src.data(), dst.data(), n);
        EXPECT_EQ(dst, ScalarAsciiToLower(src)) << "n=" << n;
    }
}

TEST(SimdAsciiAsciiToLowerOnlyTest, KeepsBytesOutsideUppercase)
{
    constexpr std::string_view kCases[] = {
        // 「あいうえお一二三四」の UTF-8 (27 byte)。continuation byte を大文字と誤判定しない
        "\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86\xE3\x81\x88\xE3\x81\x8A"
        "\xE4\xB8\x80\xE4\xBA\x8C\xE4\xB8\x89\xE5\x9B\x9B",
        // 'A' の直前の '@' (0x40) と 'Z' の直後の '[' (0x5B) は範囲外
        "[]@`{|}~[]@`{|}~[]@",
    };
    for (const std::string_view src : kCases) {
        std::string dst(src.size(), '\0');
        ascii_util::AsciiToLowerOnly(src.data(), dst.data(), src.size());
        EXPECT_EQ(dst, src);
    }
}

// ── Find ──────────────────────────────────────────────────────

TEST(SimdAsciiFindTest, EmptyQuery)
{
    EXPECT_EQ(ascii_util::Find("hello", "", 0), 0u);
    EXPECT_EQ(ascii_util::Find("hello", "", 3), 3u);
    EXPECT_EQ(ascii_util::Find("hello", "", 5), 5u);
    EXPECT_EQ(ascii_util::Find("hello", "", 6), ascii_util::npos);
}

TEST(SimdAsciiFindTest, EmptyText)
{
    EXPECT_EQ(ascii_util::Find("", "a", 0), ascii_util::npos);
}

TEST(SimdAsciiFindTest, PartialPrefixOnly)
{
    // 候補位置に最初の文字だけ一致するが残りが違うケースで誤検出しないこと
    const std::string t = "abXabXabXabXabcXabc";
    EXPECT_EQ(ascii_util::Find(t, "abc", 0), 12u);
}

TEST(SimdAsciiFindTest, CrossingSimdBoundary)
{
    // SIMD 境界 (16 byte 単位) を跨ぐマッチを発生させる
    std::string t(40, 'a');
    t.replace(15, 3, "xyz"); // 位置 15..17 に xyz (16 byte 目を跨ぐ)
    EXPECT_EQ(ascii_util::Find(t, "xyz", 0), 15u);
    EXPECT_EQ(ascii_util::Find(t, "axyz", 0), 14u);
}

TEST(SimdAsciiFindTest, RepeatedMatches)
{
    const std::string t = "aaaaaaaaaaaaaaaaaa"; // 18 個の 'a'
    EXPECT_EQ(ascii_util::Find(t, "aa", 0), 0u);
    EXPECT_EQ(ascii_util::Find(t, "aa", 5), 5u);
    EXPECT_EQ(ascii_util::Find(t, "aaa", 0), 0u);
    EXPECT_EQ(ascii_util::Find(t, "aaa", 15), 15u);
    EXPECT_EQ(ascii_util::Find(t, "aaa", 16), ascii_util::npos);
}

TEST(SimdAsciiFindTest, StartBeyondLast)
{
    const std::string t = "hello";
    // start > tlen - qlen のケース
    EXPECT_EQ(ascii_util::Find(t, "lo", 4), ascii_util::npos);
    EXPECT_EQ(ascii_util::Find(t, "lo", 3), 3u);
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

// 各開始位置から参照実装 (std::string_view::find) と一致するかを総当たりで確かめる。
void ExpectFindMatchesReference(std::string_view text, std::string_view query)
{
    const auto lower_text = ascii_util::ToLowerAsciiCopy(text);
    const auto lower_query = ascii_util::ToLowerAsciiCopy(query);
    for (size_t start = 0; start <= text.size(); ++start) {
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

TEST(AsciiUtilFind, QueryLongerThanTextOrEmpty)
{
    EXPECT_EQ(ascii_util::Find("abc", "abcd", 0), ascii_util::npos);
    EXPECT_EQ(ascii_util::FindAsciiCaseInsensitive("ABC", "abcd", 0), ascii_util::npos);
    EXPECT_EQ(ascii_util::Find("abc", "", 2), 2u);
}

TEST(AsciiUtilFind, CaseInsensitiveDoesNotFoldNonAscii)
{
    // 'Ä' (C3 84) と 'ä' (C3 A4) は ASCII 畳み込みの対象外
    EXPECT_EQ(ascii_util::FindAsciiCaseInsensitive("x\xC3\x84y", "\xC3\xA4", 0), ascii_util::npos);
    EXPECT_EQ(ascii_util::FindAsciiCaseInsensitive("x\xC3\x84Y", "\xC3\x84y", 0), 1u);
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
