#include <gtest/gtest.h>
#include "parser.h"

namespace {

std::optional<std::wstring> Resolve(std::wstring_view entity)
{
    wchar_t buf[2];
    const auto view = ResolveHtmlEntity(entity, buf);
    if (!view) {
        return std::nullopt;
    }
    return std::wstring{ *view };
}

} // namespace

TEST(HtmlEntity, NamedLiterals)
{
    EXPECT_EQ(Resolve(L"&amp;"), L"&");
    EXPECT_EQ(Resolve(L"&lt;"), L"<");
    EXPECT_EQ(Resolve(L"&gt;"), L">");
    EXPECT_EQ(Resolve(L"&quot;"), L"\"");
    EXPECT_EQ(Resolve(L"&apos;"), L"'");
    EXPECT_EQ(Resolve(L"&nbsp;"), L" ");
}

TEST(HtmlEntity, NumericDecimal)
{
    EXPECT_EQ(Resolve(L"&#65;"), L"A");
    EXPECT_EQ(Resolve(L"&#9;"), L"\t");
}

TEST(HtmlEntity, NumericHex)
{
    EXPECT_EQ(Resolve(L"&#x41;"), L"A");
    EXPECT_EQ(Resolve(L"&#X41;"), L"A");
    EXPECT_EQ(Resolve(L"&#x4E00;"), L"一");
    EXPECT_EQ(Resolve(L"&#xFFFF;"), L"￿");
}

TEST(HtmlEntity, SupplementaryPlaneSurrogatePair)
{
    // U+1F600 GRINNING FACE → サロゲートペア (D83D, DE00)
    const auto result = Resolve(L"&#x1F600;");
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 2u);
    EXPECT_EQ((*result)[0], static_cast<wchar_t>(0xD83D));
    EXPECT_EQ((*result)[1], static_cast<wchar_t>(0xDE00));
}

TEST(HtmlEntity, RejectsLoneSurrogateHigh)
{
    // U+D800-U+DBFF (high surrogate) は単独で UTF-16 として不正
    EXPECT_EQ(Resolve(L"&#xD800;"), std::nullopt);
    EXPECT_EQ(Resolve(L"&#xDBFF;"), std::nullopt);
}

TEST(HtmlEntity, RejectsLoneSurrogateLow)
{
    // U+DC00-U+DFFF (low surrogate) は単独で UTF-16 として不正
    EXPECT_EQ(Resolve(L"&#xDC00;"), std::nullopt);
    EXPECT_EQ(Resolve(L"&#xDFFF;"), std::nullopt);
}

TEST(HtmlEntity, RejectsZero)
{
    EXPECT_EQ(Resolve(L"&#0;"), std::nullopt);
    EXPECT_EQ(Resolve(L"&#x0;"), std::nullopt);
}

TEST(HtmlEntity, RejectsOutOfRange)
{
    EXPECT_EQ(Resolve(L"&#x110000;"), std::nullopt);
    EXPECT_EQ(Resolve(L"&#x7FFFFFFF;"), std::nullopt);
}

TEST(HtmlEntity, RejectsMalformed)
{
    EXPECT_EQ(Resolve(L"&unknown;"), std::nullopt);
    EXPECT_EQ(Resolve(L"&#xZZ;"), std::nullopt);
    EXPECT_EQ(Resolve(L"&#;"), std::nullopt);
    EXPECT_EQ(Resolve(L""), std::nullopt);
}
