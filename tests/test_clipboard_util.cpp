#include <gtest/gtest.h>
#include "clipboard_util.h"
#include "string_convert.h"
#include <cstdint>
#include <string>
#include <string_view>

namespace {

std::string BuildCfHtmlPayload(std::string_view fragment_utf8)
{
    std::string payload(CfHtmlPayloadSize(fragment_utf8.size()), '\0');
    WriteCfHtmlPayload(payload.data(), fragment_utf8);
    return payload;
}

} // namespace

// ヘッダは CRLF 区切りで、オフセットは 10 桁ゼロ埋め。本体は <html><body> で fragment を挟む。
TEST(BuildCfHtmlPayload, MatchesExpectedPayload)
{
    struct Case {
        std::string_view fragment;
        std::string_view expected;
    };
    constexpr Case kCases[] = {
        { "",
          "Version:0.9\r\nStartHTML:0000000105\r\nEndHTML:0000000177\r\nStartFragment:0000000141\r\nEndFragment:0000000141\r\n"
          "<html>\r\n<body>\r\n<!--StartFragment--><!--EndFragment-->\r\n</body>\r\n</html>" },
        { "<p>x</p>",
          "Version:0.9\r\nStartHTML:0000000105\r\nEndHTML:0000000185\r\nStartFragment:0000000141\r\nEndFragment:0000000149\r\n"
          "<html>\r\n<body>\r\n<!--StartFragment--><p>x</p><!--EndFragment-->\r\n</body>\r\n</html>" },
        // エスケープは呼び出し側で済んでいるので、実体参照も含めてそのまま入れる。
        { "<p>a &amp; b &lt; c</p>",
          "Version:0.9\r\nStartHTML:0000000105\r\nEndHTML:0000000200\r\nStartFragment:0000000141\r\nEndFragment:0000000164\r\n"
          "<html>\r\n<body>\r\n<!--StartFragment--><p>a &amp; b &lt; c</p><!--EndFragment-->\r\n</body>\r\n</html>" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.fragment);
        EXPECT_EQ(BuildCfHtmlPayload(c.fragment), c.expected);
    }
}

// ---- DibTotalBytes / WriteDibHeader (CF_DIB 用ビットマップ構築の部品) ----

TEST(DibTotalBytes, HeaderPlusPixels)
{
    EXPECT_EQ(DibTotalBytes(2, 2), sizeof(BITMAPINFOHEADER) + 2u * 2u * 4u);
    EXPECT_EQ(DibTotalBytes(1, 1), sizeof(BITMAPINFOHEADER) + 4u);
}

TEST(DibTotalBytes, RejectsZeroOrOverflow)
{
    EXPECT_EQ(DibTotalBytes(0, 2), 0u);
    EXPECT_EQ(DibTotalBytes(2, 0), 0u);
    // height * width*4 が size_t を溢れる組み合わせは 0。
    EXPECT_EQ(DibTotalBytes(0xFFFFFFFFu, 0xFFFFFFFFu), 0u);
}

TEST(WriteDibHeader, ProducesTopDown32bppRgb)
{
    alignas(BITMAPINFOHEADER) uint8_t buf[sizeof(BITMAPINFOHEADER)] = {};
    WriteDibHeader(buf, 2, 3);
    const auto* bih = reinterpret_cast<const BITMAPINFOHEADER*>(buf);
    EXPECT_EQ(bih->biSize, sizeof(BITMAPINFOHEADER));
    EXPECT_EQ(bih->biWidth, 2);
    EXPECT_EQ(bih->biHeight, -3); // 負 = トップダウン
    EXPECT_EQ(bih->biPlanes, 1);
    EXPECT_EQ(bih->biBitCount, 32);
    EXPECT_EQ(bih->biCompression, static_cast<DWORD>(BI_RGB));
    EXPECT_EQ(bih->biSizeImage, 2u * 3u * 4u);
}

namespace {

template <typename CharT>
std::basic_string<CharT> ReadGlobal(const UniqueGlobalMem& mem)
{
    const auto* p = static_cast<const CharT*>(GlobalLock(mem.get()));
    std::basic_string<CharT> s = p ? std::basic_string<CharT>(p) : std::basic_string<CharT>{};
    GlobalUnlock(mem.get());
    return s;
}

} // namespace

// UTF-8 → UTF-16 を GlobalAlloc 先へ直接変換し、中間 wstring 版と同じ結果になる。
TEST(BuildGlobalWideFromUtf8, MatchesUtf8ToWide)
{
    const std::string_view utf8 = "abc \xE3\x81\x82\xF0\x9F\x98\x80 xyz";
    const auto mem = BuildGlobalWideFromUtf8(utf8);
    ASSERT_TRUE(mem);
    EXPECT_EQ(ReadGlobal<wchar_t>(mem), std::wstring(string_convert::Utf8ToWide(utf8)));
    EXPECT_FALSE(BuildGlobalWideFromUtf8(""));
}

TEST(BuildGlobalCfHtml, MatchesBuildCfHtmlPayload)
{
    const std::string_view fragment = "<b>\xE3\x81\x82</b>";
    const auto mem = BuildGlobalCfHtml(fragment);
    ASSERT_TRUE(mem);
    EXPECT_EQ(ReadGlobal<char>(mem), BuildCfHtmlPayload(fragment));
}
