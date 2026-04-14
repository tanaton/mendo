#include <gtest/gtest.h>
#include "string_convert.h"

using namespace string_convert;

// ═══════════════════════════════════════════════
// Utf8ToWide
// ═══════════════════════════════════════════════

TEST(StringConvert, Utf8ToWideAscii)
{
    auto result = Utf8ToWide("Hello");
    EXPECT_EQ(result, L"Hello");
}

TEST(StringConvert, Utf8ToWideEmpty)
{
    auto result = Utf8ToWide("");
    EXPECT_TRUE(result.empty());
}

TEST(StringConvert, Utf8ToWideJapanese)
{
    auto result = Utf8ToWide("日本語テスト");
    EXPECT_EQ(result, L"日本語テスト");
}

TEST(StringConvert, Utf8ToWideEmoji)
{
    // BMP外の絵文字（サロゲートペアになる）
    auto result = Utf8ToWide("\xF0\x9F\x98\x80"); // 😀
    EXPECT_EQ(result.size(), 2u); // サロゲートペア
}

TEST(StringConvert, Utf8ToWideMixed)
{
    auto result = Utf8ToWide("Hello, 世界!");
    EXPECT_EQ(result, L"Hello, 世界!");
}

TEST(StringConvert, Utf8ToWideRefVersion)
{
    std::pmr::wstring out;
    Utf8ToWide("test", out);
    EXPECT_EQ(out, L"test");
}

TEST(StringConvert, Utf8ToWideRefClearsOnEmpty)
{
    std::pmr::wstring out = L"old";
    Utf8ToWide("", out);
    EXPECT_TRUE(out.empty());
}

// ═══════════════════════════════════════════════
// WideToUtf8
// ═══════════════════════════════════════════════

TEST(StringConvert, WideToUtf8Ascii)
{
    auto result = WideToUtf8(L"Hello");
    EXPECT_EQ(result, "Hello");
}

TEST(StringConvert, WideToUtf8Empty)
{
    auto result = WideToUtf8(L"");
    EXPECT_TRUE(result.empty());
}

TEST(StringConvert, WideToUtf8Japanese)
{
    auto result = WideToUtf8(L"日本語テスト");
    EXPECT_EQ(result, "日本語テスト");
}

TEST(StringConvert, WideToUtf8RefVersion)
{
    std::string out;
    WideToUtf8(L"test", out);
    EXPECT_EQ(out, "test");
}

TEST(StringConvert, WideToUtf8RefClearsOnEmpty)
{
    std::string out = "old";
    WideToUtf8(L"", out);
    EXPECT_TRUE(out.empty());
}

// ═══════════════════════════════════════════════
// ラウンドトリップ
// ═══════════════════════════════════════════════

TEST(StringConvert, RoundTripAscii)
{
    std::string original = "Hello, World!";
    auto wide = Utf8ToWide(original);
    auto back = WideToUtf8(wide);
    EXPECT_EQ(back, original);
}

TEST(StringConvert, RoundTripJapanese)
{
    std::string original = "マークダウンビュアー";
    auto wide = Utf8ToWide(original);
    auto back = WideToUtf8(wide);
    EXPECT_EQ(back, original);
}

TEST(StringConvert, RoundTripMixed)
{
    std::string original = "# 見出し\n\nHello 世界 123";
    auto wide = Utf8ToWide(original);
    auto back = WideToUtf8(wide);
    EXPECT_EQ(back, original);
}

TEST(StringConvert, RoundTripSpecialChars)
{
    std::string original = "a\tb\nc\r\nd";
    auto wide = Utf8ToWide(original);
    auto back = WideToUtf8(wide);
    EXPECT_EQ(back, original);
}

TEST(StringConvert, RoundTripEmoji)
{
    std::string original = "\xF0\x9F\x98\x80\xF0\x9F\x8E\x89"; // 😀🎉
    auto wide = Utf8ToWide(original);
    auto back = WideToUtf8(wide);
    EXPECT_EQ(back, original);
}

// ═══════════════════════════════════════════════
// エッジケース
// ═══════════════════════════════════════════════

TEST(StringConvert, SingleCharUtf8ToWide)
{
    auto result = Utf8ToWide("A");
    EXPECT_EQ(result, L"A");
    EXPECT_EQ(result.size(), 1u);
}

TEST(StringConvert, SingleCharWideToUtf8)
{
    auto result = WideToUtf8(L"A");
    EXPECT_EQ(result, "A");
    EXPECT_EQ(result.size(), 1u);
}

TEST(StringConvert, LongString)
{
    std::string utf8(10000, 'x');
    auto wide = Utf8ToWide(utf8);
    EXPECT_EQ(wide.size(), 10000u);
    auto back = WideToUtf8(wide);
    EXPECT_EQ(back, utf8);
}

TEST(StringConvert, NullByteInMiddle)
{
    // string_view ベースなので null バイトも扱える
    std::string utf8("a\0b", 3);
    auto wide = Utf8ToWide(utf8);
    EXPECT_EQ(wide.size(), 3u);
    EXPECT_EQ(wide[0], L'a');
    EXPECT_EQ(wide[1], L'\0');
    EXPECT_EQ(wide[2], L'b');
}

TEST(StringConvert, MultiByteBoundary)
{
    // 2バイトUTF-8文字 (U+00E9 é)
    auto result = Utf8ToWide("\xC3\xA9");
    EXPECT_EQ(result, L"\u00E9");
    EXPECT_EQ(result.size(), 1u);
}

TEST(StringConvert, ThreeByteBoundary)
{
    // 3バイトUTF-8文字 (U+3042 あ)
    auto result = Utf8ToWide("\xE3\x81\x82");
    EXPECT_EQ(result, L"\u3042");
    EXPECT_EQ(result.size(), 1u);
}
