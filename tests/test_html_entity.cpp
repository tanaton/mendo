#include <gtest/gtest.h>
#include "html_entities.h"

namespace {

std::optional<std::string> Resolve(std::string_view entity)
{
    char buf[4];
    const auto view = ResolveHtmlEntity(entity, buf);
    if (!view) {
        return std::nullopt;
    }
    return std::string{ *view };
}

} // namespace

TEST(HtmlEntity, NamedLiterals)
{
    EXPECT_EQ(Resolve("&amp;"), "&");
    EXPECT_EQ(Resolve("&lt;"), "<");
    EXPECT_EQ(Resolve("&gt;"), ">");
    EXPECT_EQ(Resolve("&quot;"), "\"");
    EXPECT_EQ(Resolve("&apos;"), "'");
    EXPECT_EQ(Resolve("&nbsp;"), " ");
}

TEST(HtmlEntity, NumericDecimal)
{
    EXPECT_EQ(Resolve("&#65;"), "A");
    EXPECT_EQ(Resolve("&#9;"), "\t");
}

TEST(HtmlEntity, NumericHex)
{
    EXPECT_EQ(Resolve("&#x41;"), "A");
    EXPECT_EQ(Resolve("&#X41;"), "A");
    EXPECT_EQ(Resolve("&#x4E00;"), "一");
    EXPECT_EQ(Resolve("&#xFFFF;"), "￿");
}

TEST(HtmlEntity, SupplementaryPlaneSurrogatePair)
{
    // U+1F600 GRINNING FACE → UTF-8: F0 9F 98 80
    const auto result = Resolve("&#x1F600;");
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
    EXPECT_EQ(Resolve("&#xD800;"), std::nullopt);
    EXPECT_EQ(Resolve("&#xDBFF;"), std::nullopt);
}

TEST(HtmlEntity, RejectsLoneSurrogateLow)
{
    // U+DC00-U+DFFF (low surrogate) は単独で UTF-16 として不正
    EXPECT_EQ(Resolve("&#xDC00;"), std::nullopt);
    EXPECT_EQ(Resolve("&#xDFFF;"), std::nullopt);
}

TEST(HtmlEntity, RejectsZero)
{
    EXPECT_EQ(Resolve("&#0;"), std::nullopt);
    EXPECT_EQ(Resolve("&#x0;"), std::nullopt);
}

TEST(HtmlEntity, RejectsOutOfRange)
{
    EXPECT_EQ(Resolve("&#x110000;"), std::nullopt);
    EXPECT_EQ(Resolve("&#x7FFFFFFF;"), std::nullopt);
}

TEST(HtmlEntity, RejectsDigitOverflow)
{
    // 32 bit 整数の wrap (例: 4294967297 → 1) を桁数で先に弾く。
    EXPECT_EQ(Resolve("&#4294967296;"), std::nullopt); // 2^32 (10 桁、wrap で 0)
    EXPECT_EQ(Resolve("&#4294967297;"), std::nullopt); // 2^32 + 1 (10 桁、wrap で 1)
    EXPECT_EQ(Resolve("&#x100000000;"), std::nullopt); // 2^32 (9 桁 hex、wrap で 0)
    EXPECT_EQ(Resolve("&#x100000001;"), std::nullopt); // 2^32 + 1 (9 桁 hex、wrap で 1)
}

TEST(HtmlEntity, RejectsMalformed)
{
    EXPECT_EQ(Resolve("&unknown;"), std::nullopt);
    EXPECT_EQ(Resolve("&#xZZ;"), std::nullopt);
    EXPECT_EQ(Resolve("&#;"), std::nullopt);
    EXPECT_EQ(Resolve(""), std::nullopt);
}
