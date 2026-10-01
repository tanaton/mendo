#include <gtest/gtest.h>
#include "string_convert.h"

using namespace string_convert;
using namespace std::literals;

namespace {

struct ConvertCase {
    std::string_view utf8;
    std::wstring_view wide;
};

constexpr ConvertCase kConvertCases[] = {
    { ""sv, L""sv },
    { "A"sv, L"A"sv },
    { "test"sv, L"test"sv },
    { "Hello"sv, L"Hello"sv },
    { "Hello, World!"sv, L"Hello, World!"sv },
    { "a\tb\nc\r\nd"sv, L"a\tb\nc\r\nd"sv },
    // string_view ベースなので途中の NUL も保持する
    { "a\0b"sv, L"a\0b"sv },
    { "\xC3\xA9"sv, L"é"sv },
    { "\xE3\x81\x82"sv, L"あ"sv },
    // BMP 外はサロゲートペアになる
    { "\xF0\x9F\x98\x80"sv, L"\xD83D\xDE00"sv },
    { "\xF0\x9F\x98\x80\xF0\x9F\x8E\x89"sv, L"\xD83D\xDE00\xD83C\xDF89"sv },
    { "日本語テスト"sv, L"日本語テスト"sv },
    { "マークダウンビュアー"sv, L"マークダウンビュアー"sv },
    { "Hello, 世界!"sv, L"Hello, 世界!"sv },
    { "# 見出し\n\nHello 世界 123"sv, L"# 見出し\n\nHello 世界 123"sv },

    // 16 byte チャンク境界の前後
    { "0123456789ABCDE"sv, L"0123456789ABCDE"sv },
    { "0123456789ABCDEF"sv, L"0123456789ABCDEF"sv },
    { "0123456789ABCDEFG"sv, L"0123456789ABCDEFG"sv },
    { "0123456789ABCDEFghijklmnopqrstu"sv, L"0123456789ABCDEFghijklmnopqrstu"sv },
    { "0123456789ABCDEFghijklmnopqrstuv"sv, L"0123456789ABCDEFghijklmnopqrstuv"sv },
    { "\xE3\x81\x82" "0123456789ABCDE"sv, L"あ" L"0123456789ABCDE"sv },
    { "abc\xE3\x81\x82" "def" "\xE4\xB8\x96" "ghi"sv, L"abcあ" L"def世" L"ghi"sv },
    // 13 byte ASCII + 3 byte CJK でちょうど 1 チャンク
    { "0123456789ABC" "\xE3\x81\x82"sv, L"0123456789ABCあ"sv },
    // leading byte が境界に来る
    { "0123456789ABCDE" "\xE3\x81\x82"sv, L"0123456789ABCDEあ"sv },
    { "xxxxxxxxxxxxxxx" "\xF0\x9F\x98\x80"sv, L"xxxxxxxxxxxxxxx\xD83D\xDE00"sv },
};

} // namespace

TEST(StringConvert, ConvertsBothDirections)
{
    for (const auto& c : kConvertCases) {
        SCOPED_TRACE(::testing::PrintToString(c.utf8));
        EXPECT_EQ(Utf8ToWide(c.utf8), c.wide);
        EXPECT_EQ(WideToUtf8(c.wide), c.utf8);

        // 出力引数版は既存内容を上書きする
        std::pmr::wstring wide_out = L"old";
        Utf8ToWide(c.utf8, wide_out);
        EXPECT_EQ(wide_out, c.wide);
        std::string utf8_out = "old";
        WideToUtf8(c.wide, utf8_out);
        EXPECT_EQ(utf8_out, c.utf8);
    }
}

TEST(StringConvert, LongAsciiRoundTrip)
{
    for (const size_t size : { size_t{ 1024 }, size_t{ 10000 } }) {
        SCOPED_TRACE(size);
        const std::string utf8(size, 'A');
        const auto wide = Utf8ToWide(utf8);
        EXPECT_EQ(wide, std::pmr::wstring(size, L'A'));
        EXPECT_EQ(WideToUtf8(wide), utf8);
    }
}

TEST(StringConvert, Utf8ToWideAlternatingAsciiCjk)
{
    std::string utf8;
    std::pmr::wstring expected;
    for (int i = 0; i < 50; ++i) {
        utf8 += "Hello world! ";
        utf8 += "\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF ";
        expected += L"Hello world! ";
        expected += L"こんにちは ";
    }
    auto result = Utf8ToWide(utf8);
    EXPECT_EQ(result, expected);
}

TEST(StringConvert, Utf8ToWideAllAsciiCodepoints)
{
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
