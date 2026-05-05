#include <gtest/gtest.h>
#include "parser.h"

namespace {

std::optional<mendo::doc_string_std> Resolve(mendo::doc_string_view entity)
{
    mendo::doc_char buf[4];
    const auto view = ResolveHtmlEntity(entity, buf);
    if (!view) {
        return std::nullopt;
    }
    return mendo::doc_string_std{ *view };
}

} // namespace

TEST(HtmlEntity, NamedLiterals)
{
    EXPECT_EQ(Resolve(MENDO_LIT("&amp;")), MENDO_LIT("&"));
    EXPECT_EQ(Resolve(MENDO_LIT("&lt;")), MENDO_LIT("<"));
    EXPECT_EQ(Resolve(MENDO_LIT("&gt;")), MENDO_LIT(">"));
    EXPECT_EQ(Resolve(MENDO_LIT("&quot;")), MENDO_LIT("\""));
    EXPECT_EQ(Resolve(MENDO_LIT("&apos;")), MENDO_LIT("'"));
    EXPECT_EQ(Resolve(MENDO_LIT("&nbsp;")), MENDO_LIT(" "));
}

TEST(HtmlEntity, NumericDecimal)
{
    EXPECT_EQ(Resolve(MENDO_LIT("&#65;")), MENDO_LIT("A"));
    EXPECT_EQ(Resolve(MENDO_LIT("&#9;")), MENDO_LIT("\t"));
}

TEST(HtmlEntity, NumericHex)
{
    EXPECT_EQ(Resolve(MENDO_LIT("&#x41;")), MENDO_LIT("A"));
    EXPECT_EQ(Resolve(MENDO_LIT("&#X41;")), MENDO_LIT("A"));
    EXPECT_EQ(Resolve(MENDO_LIT("&#x4E00;")), MENDO_LIT("一"));
    EXPECT_EQ(Resolve(MENDO_LIT("&#xFFFF;")), MENDO_LIT("￿"));
}

TEST(HtmlEntity, SupplementaryPlaneSurrogatePair)
{
    // U+1F600 GRINNING FACE → UTF-8: F0 9F 98 80
    const auto result = Resolve(MENDO_LIT("&#x1F600;"));
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 4u);
    EXPECT_EQ(static_cast<unsigned char>((*result)[0]), 0xF0);
    EXPECT_EQ(static_cast<unsigned char>((*result)[1]), 0x9F);
    EXPECT_EQ(static_cast<unsigned char>((*result)[2]), 0x98);
    EXPECT_EQ(static_cast<unsigned char>((*result)[3]), 0x80);
}

TEST(HtmlEntity, RejectsLoneSurrogateHigh)
{
    // U+D800-U+DBFF (high surrogate) は単独で UTF-16 として不正
    EXPECT_EQ(Resolve(MENDO_LIT("&#xD800;")), std::nullopt);
    EXPECT_EQ(Resolve(MENDO_LIT("&#xDBFF;")), std::nullopt);
}

TEST(HtmlEntity, RejectsLoneSurrogateLow)
{
    // U+DC00-U+DFFF (low surrogate) は単独で UTF-16 として不正
    EXPECT_EQ(Resolve(MENDO_LIT("&#xDC00;")), std::nullopt);
    EXPECT_EQ(Resolve(MENDO_LIT("&#xDFFF;")), std::nullopt);
}

TEST(HtmlEntity, RejectsZero)
{
    EXPECT_EQ(Resolve(MENDO_LIT("&#0;")), std::nullopt);
    EXPECT_EQ(Resolve(MENDO_LIT("&#x0;")), std::nullopt);
}

TEST(HtmlEntity, RejectsOutOfRange)
{
    EXPECT_EQ(Resolve(MENDO_LIT("&#x110000;")), std::nullopt);
    EXPECT_EQ(Resolve(MENDO_LIT("&#x7FFFFFFF;")), std::nullopt);
}

TEST(HtmlEntity, RejectsMalformed)
{
    EXPECT_EQ(Resolve(MENDO_LIT("&unknown;")), std::nullopt);
    EXPECT_EQ(Resolve(MENDO_LIT("&#xZZ;")), std::nullopt);
    EXPECT_EQ(Resolve(MENDO_LIT("&#;")), std::nullopt);
    EXPECT_EQ(Resolve(MENDO_LIT("")), std::nullopt);
}
