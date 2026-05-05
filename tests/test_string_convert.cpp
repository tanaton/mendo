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
// StripUtf8Bom
// ═══════════════════════════════════════════════

TEST(StringConvert, StripUtf8BomStripsLeadingBom)
{
    EXPECT_EQ(StripUtf8Bom("\xEF\xBB\xBFHello"), std::string_view("Hello"));
}

TEST(StringConvert, StripUtf8BomNoBomPassthrough)
{
    EXPECT_EQ(StripUtf8Bom("Hello"), std::string_view("Hello"));
}

TEST(StringConvert, StripUtf8BomEmptyInput)
{
    EXPECT_TRUE(StripUtf8Bom("").empty());
}

TEST(StringConvert, StripUtf8BomBomOnlyResultsEmpty)
{
    EXPECT_TRUE(StripUtf8Bom("\xEF\xBB\xBF").empty());
}

TEST(StringConvert, StripUtf8BomOnlyFirstBomStripped)
{
    // 連続 BOM は最初の 1 つだけ除去される。
    EXPECT_EQ(StripUtf8Bom("\xEF\xBB\xBF\xEF\xBB\xBF" "A"),
              std::string_view("\xEF\xBB\xBF" "A"));
}

TEST(StringConvert, StripUtf8BomBomWithJapanese)
{
    EXPECT_EQ(StripUtf8Bom("\xEF\xBB\xBF日本語"), std::string_view("日本語"));
}

TEST(StringConvert, StripUtf8BomBomInMiddleNotStripped)
{
    // BOM の出現位置が先頭以外なら除去しない。
    EXPECT_EQ(StripUtf8Bom("A\xEF\xBB\xBF" "B"),
              std::string_view("A\xEF\xBB\xBF" "B"));
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

// \u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550
// 16 \u30d0\u30a4\u30c8\u5883\u754c / \u5404\u7a2e\u9577\u3055\u30fb\u6df7\u5728\u30d1\u30bf\u30fc\u30f3\u306e\u56de\u5e30\u30c6\u30b9\u30c8
// \u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550

TEST(StringConvert, Utf8ToWide16ByteExact)
{
    std::string utf8 = "0123456789ABCDEF";
    EXPECT_EQ(utf8.size(), 16u);
    auto result = Utf8ToWide(utf8);
    EXPECT_EQ(result, L"0123456789ABCDEF");
    EXPECT_EQ(result.size(), 16u);
}

TEST(StringConvert, Utf8ToWide32ByteExact)
{
    std::string utf8 = "0123456789ABCDEFghijklmnopqrstuv";
    EXPECT_EQ(utf8.size(), 32u);
    auto result = Utf8ToWide(utf8);
    EXPECT_EQ(result.size(), 32u);
    EXPECT_EQ(result, L"0123456789ABCDEFghijklmnopqrstuv");
}

TEST(StringConvert, Utf8ToWide15ByteJustUnderBoundary)
{
    std::string utf8 = "0123456789ABCDE";
    EXPECT_EQ(utf8.size(), 15u);
    auto result = Utf8ToWide(utf8);
    EXPECT_EQ(result, L"0123456789ABCDE");
}

TEST(StringConvert, Utf8ToWide17ByteJustOverBoundary)
{
    std::string utf8 = "0123456789ABCDEFG";
    auto result = Utf8ToWide(utf8);
    EXPECT_EQ(result, L"0123456789ABCDEFG");
}

TEST(StringConvert, Utf8ToWide31ByteJustUnderTwoChunks)
{
    std::string utf8 = "0123456789ABCDEFghijklmnopqrstu";
    auto result = Utf8ToWide(utf8);
    EXPECT_EQ(result.size(), 31u);
    EXPECT_EQ(result, L"0123456789ABCDEFghijklmnopqrstu");
}

TEST(StringConvert, Utf8ToWide1024Byte)
{
    std::string utf8(1024, 'A');
    auto result = Utf8ToWide(utf8);
    EXPECT_EQ(result.size(), 1024u);
    for (size_t i = 0; i < result.size(); ++i) {
        EXPECT_EQ(result[i], L'A');
    }
}

TEST(StringConvert, Utf8ToWideNonAsciiAtBoundaryStart)
{
    // \u5148\u982d\u30d0\u30a4\u30c8\u5373\u975e ASCII\u3002U+3042 (\xE3\x81\x82) + ASCII 15 byte
    std::string utf8 = "\xE3\x81\x82" "0123456789ABCDE";
    auto result = Utf8ToWide(utf8);
    EXPECT_EQ(result, L"\u3042" L"0123456789ABCDE");
}

TEST(StringConvert, Utf8ToWideNonAsciiInMiddle)
{
    // "abc" + U+3042 + "def" + U+4E16 + "ghi"
    std::string utf8 = "abc\xE3\x81\x82" "def" "\xE4\xB8\x96" "ghi";
    auto result = Utf8ToWide(utf8);
    EXPECT_EQ(result, L"abc\u3042" L"def\u4e16" L"ghi");
}

TEST(StringConvert, Utf8ToWideNonAsciiAtBoundaryEnd)
{
    // 13 byte ASCII + 3 byte CJK = 16 byte (1 \u30c1\u30e3\u30f3\u30af)
    std::string utf8 = "0123456789ABC" "\xE3\x81\x82";
    auto result = Utf8ToWide(utf8);
    EXPECT_EQ(result, L"0123456789ABC\u3042");
}

TEST(StringConvert, Utf8ToWideNonAsciiSpansBoundary)
{
    // 15 byte ASCII + 3 byte CJK = 18 byte\u3002\u5883\u754c\u306b leading byte \u304c\u6765\u308b\u3002
    std::string utf8 = "0123456789ABCDE" "\xE3\x81\x82";
    auto result = Utf8ToWide(utf8);
    EXPECT_EQ(result, L"0123456789ABCDE\u3042");
}

TEST(StringConvert, Utf8ToWideAlternatingAsciiCjk)
{
    // U+3053 U+3093 U+306B U+3061 U+306F = "\u3053\u3093\u306b\u3061\u306f"
    std::string utf8;
    std::pmr::wstring expected;
    for (int i = 0; i < 50; ++i) {
        utf8 += "Hello world! ";
        utf8 += "\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF ";
        expected += L"Hello world! ";
        expected += L"\u3053\u3093\u306b\u3061\u306f ";
    }
    auto result = Utf8ToWide(utf8);
    EXPECT_EQ(result, expected);
}

TEST(StringConvert, Utf8ToWideEmojiAtBoundary)
{
    // \u88dc\u52a9\u9762\u6587\u5b57\u3092 16 byte \u5883\u754c\u306b\u7f6e\u3044\u3066\u30b5\u30ed\u30b2\u30fc\u30c8\u30da\u30a2\u304c\u6b63\u3057\u304f\u51fa\u529b\u3055\u308c\u308b\u3053\u3068
    std::string utf8(15, 'x');
    utf8 += "\xF0\x9F\x98\x80"; // U+1F600 (4 byte UTF-8 \u2192 2 wchar surrogate pair)
    auto result = Utf8ToWide(utf8);
    EXPECT_EQ(result.size(), 17u);
    EXPECT_EQ(result.substr(0, 15), std::pmr::wstring(15, L'x'));
    EXPECT_EQ(result[15], static_cast<wchar_t>(0xD83D));
    EXPECT_EQ(result[16], static_cast<wchar_t>(0xDE00));
}

TEST(StringConvert, Utf8ToWideAllAsciiCodepoints)
{
    // 0x00 \u301c 0x7F \u306e\u5168 ASCII \u7bc4\u56f2\u3092\u542b\u3080\u6587\u5b57\u5217\u3092\u5909\u63db\u3057\u3066\u3082 1:1 \u3067\u5bfe\u5fdc
    std::string utf8;
    utf8.reserve(128);
    for (int c = 0; c < 128; ++c) {
        utf8.push_back(static_cast<char>(c));
    }
    auto result = Utf8ToWide(utf8);
    EXPECT_EQ(result.size(), 128u);
    for (int c = 0; c < 128; ++c) {
        EXPECT_EQ(result[static_cast<size_t>(c)], static_cast<wchar_t>(c));
    }
}
