#include <gtest/gtest.h>
#include "clipboard_util.h"
#include <charconv>
#include <string>
#include <string_view>

namespace {

// CF_HTML ヘッダのオフセット値 (10 桁ゼロ埋め) をパースする。
size_t ParseOffset(std::string_view payload, std::string_view key)
{
    const auto pos = payload.find(key);
    if (pos == std::string_view::npos) {
        return std::string_view::npos;
    }
    const auto digit_start = pos + key.size();
    size_t value = 0;
    auto [_, ec] = std::from_chars(payload.data() + digit_start,
                                   payload.data() + digit_start + 10, value);
    if (ec != std::errc{}) {
        return std::string_view::npos;
    }
    return value;
}

} // namespace

TEST(BuildCfHtmlPayload, EmptyFragmentStillProducesValidHeader)
{
    const auto p = BuildCfHtmlPayload("");
    EXPECT_NE(p.find("Version:0.9"), std::string::npos);
    EXPECT_NE(p.find("StartHTML:"), std::string::npos);
    EXPECT_NE(p.find("EndHTML:"), std::string::npos);
    EXPECT_NE(p.find("StartFragment:"), std::string::npos);
    EXPECT_NE(p.find("EndFragment:"), std::string::npos);
    EXPECT_NE(p.find("<!--StartFragment-->"), std::string::npos);
    EXPECT_NE(p.find("<!--EndFragment-->"), std::string::npos);
}

TEST(BuildCfHtmlPayload, OffsetsArePositionsOfMarkers)
{
    const std::string fragment = "<p>hello</p>";
    const auto p = BuildCfHtmlPayload(fragment);

    const size_t start_html = ParseOffset(p, "StartHTML:");
    const size_t end_html = ParseOffset(p, "EndHTML:");
    const size_t start_fragment = ParseOffset(p, "StartFragment:");
    const size_t end_fragment = ParseOffset(p, "EndFragment:");

    ASSERT_NE(start_html, std::string_view::npos);
    ASSERT_NE(end_html, std::string_view::npos);
    ASSERT_NE(start_fragment, std::string_view::npos);
    ASSERT_NE(end_fragment, std::string_view::npos);

    // StartHTML は <html> 開始位置 = "<html>\r\n<body>\r\n<!--StartFragment-->" の先頭。
    EXPECT_EQ(p.substr(start_html, 6), "<html>");
    // StartFragment 直後は fragment 本体。
    EXPECT_EQ(p.substr(start_fragment, fragment.size()), fragment);
    // EndFragment は <!--EndFragment--> の開始位置。
    EXPECT_EQ(p.substr(end_fragment, std::string_view{ "<!--EndFragment-->" }.size()),
              "<!--EndFragment-->");
    // EndHTML は payload 末尾 (= </html> の終端)。
    EXPECT_EQ(end_html, p.size());
}

TEST(BuildCfHtmlPayload, OffsetsAreOrderedAscending)
{
    const auto p = BuildCfHtmlPayload("body");
    const size_t sh = ParseOffset(p, "StartHTML:");
    const size_t sf = ParseOffset(p, "StartFragment:");
    const size_t ef = ParseOffset(p, "EndFragment:");
    const size_t eh = ParseOffset(p, "EndHTML:");
    EXPECT_LT(sh, sf);
    EXPECT_LT(sf, ef);
    EXPECT_LT(ef, eh);
}

TEST(BuildCfHtmlPayload, FragmentContentPreservedExactly)
{
    // HTMLエスケープや変換を行わない (呼び出し側で済んでいる前提)
    const std::string fragment = "<p>a &amp; b &lt; c</p>";
    const auto p = BuildCfHtmlPayload(fragment);
    EXPECT_NE(p.find(fragment), std::string::npos);
}

TEST(BuildCfHtmlPayload, OffsetDigitsAreTenZeroPadded)
{
    const auto p = BuildCfHtmlPayload("x");
    // "StartHTML:" の直後 10 文字は数字。
    const auto pos = p.find("StartHTML:");
    ASSERT_NE(pos, std::string::npos);
    for (size_t i = 0; i < 10; ++i) {
        const char c = p[pos + std::string_view{ "StartHTML:" }.size() + i];
        EXPECT_GE(c, '0');
        EXPECT_LE(c, '9');
    }
}

TEST(BuildCfHtmlPayload, HeaderContainsCRLFSeparators)
{
    const auto p = BuildCfHtmlPayload("x");
    // 各 keyword は \r\n で区切られる
    EXPECT_NE(p.find("\r\nStartHTML:"), std::string::npos);
    EXPECT_NE(p.find("\r\nEndHTML:"), std::string::npos);
    EXPECT_NE(p.find("\r\nStartFragment:"), std::string::npos);
    EXPECT_NE(p.find("\r\nEndFragment:"), std::string::npos);
}

TEST(BuildCfHtmlPayload, HtmlBodyWrapperPresent)
{
    const auto p = BuildCfHtmlPayload("X");
    EXPECT_NE(p.find("<html>\r\n<body>\r\n<!--StartFragment-->"), std::string::npos);
    EXPECT_NE(p.find("<!--EndFragment-->\r\n</body>\r\n</html>"), std::string::npos);
}

TEST(BuildCfHtmlPayload, LongFragmentOffsetWithinTenDigitRange)
{
    // 10 桁ゼロ埋めの上限 (10^10 - 1) を fragment 長で踏まないことの確認。
    // %010zu の桁あふれは fragment 1KB 規模でも検出できるので、サイズは抑える。
    const std::string fragment(64 * 1024, 'A');
    const auto p = BuildCfHtmlPayload(fragment);
    const size_t end_html = ParseOffset(p, "EndHTML:");
    EXPECT_EQ(end_html, p.size());
    EXPECT_LT(end_html, 10ULL * 1000ULL * 1000ULL * 1000ULL);
}
