#include <gtest/gtest.h>
#include "parser.h"
#include "html_entity_table.h"
#include "utf8_codec.h"
#include <iterator>

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

TEST(HtmlEntity, NamedHtml5)
{
    EXPECT_EQ(Resolve("&copy;"), "©");
    EXPECT_EQ(Resolve("&mdash;"), "—");
    EXPECT_EQ(Resolve("&hellip;"), "…");
    EXPECT_EQ(Resolve("&rarr;"), "→");
    EXPECT_EQ(Resolve("&times;"), "×");
    EXPECT_EQ(Resolve("&AElig;"), "Æ");
    EXPECT_EQ(Resolve("&zwnj;"), "‌");
    // 2 コードポイントに展開される参照
    EXPECT_EQ(Resolve("&NotEqualTilde;"), "≂̸");
}

TEST(HtmlEntity, RejectsUnknownNamed)
{
    EXPECT_EQ(Resolve("&unknownentity;"), std::nullopt);
    EXPECT_EQ(Resolve("&COPY2;"), std::nullopt);
    // 名前は大文字小文字を区別する
    EXPECT_EQ(Resolve("&Mdash;"), std::nullopt);
    EXPECT_EQ(Resolve("&copy"), std::nullopt);
}

// 表は生成物で、utf8_len や 16 進エスケープのずれは整列の static_assert では捕まらない。
// 1 件ずれると以降の名前と展開値がすべて食い違うので、全件を隣接オフセットと往復で確かめる。
TEST(HtmlEntity, GeneratedTableIsContiguousAndRoundTrips)
{
    constexpr size_t kCount = std::size(kHtmlNamedEntities);
    EXPECT_EQ(kCount, 2125u);
    for (size_t i = 0; i < kCount; ++i) {
        const auto& e = kHtmlNamedEntities[i];
        const size_t next = i + 1 < kCount ? kHtmlNamedEntities[i + 1].offset : sizeof(kHtmlEntityData) - 1;
        ASSERT_EQ(e.offset + e.name_len + e.utf8_len, next) << "entry " << i;

        const std::string_view name{ kHtmlEntityData + e.offset, e.name_len };
        const std::string_view value{ kHtmlEntityData + e.offset + e.name_len, e.utf8_len };
        SCOPED_TRACE(name);
        for (uint32_t pos = 0; pos < value.size();) {
            const auto d = utf8_codec::DecodeAt(value, pos);
            ASSERT_NE(d.cp, utf8_codec::kReplacement);
            pos += d.len;
        }
        EXPECT_EQ(Resolve("&" + std::string(name) + ";"), std::string(value));
    }
}

// 二分探索の境界: 前方一致・後方延長・表の両端の外側
TEST(HtmlEntity, RejectsNearMissNames)
{
    EXPECT_EQ(Resolve("&AEli;"), std::nullopt);
    EXPECT_EQ(Resolve("&zwnjx;"), std::nullopt);
    EXPECT_EQ(Resolve("&am;"), std::nullopt);
    EXPECT_EQ(Resolve("&AAA;"), std::nullopt);
    EXPECT_EQ(Resolve("&zzzz;"), std::nullopt);
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
    // 非文字 U+FFFF も拒否せず 3 byte で返す
    EXPECT_EQ(Resolve("&#xFFFF;"), "\xEF\xBF\xBF");
    // 追加面 (U+1F600) は UTF-8 の 4 byte
    EXPECT_EQ(Resolve("&#x1F600;"), "\xF0\x9F\x98\x80");
}

// サロゲート (U+D800-U+DFFF) は単独では文字にならない
TEST(HtmlEntity, RejectsSurrogates)
{
    EXPECT_EQ(Resolve("&#xD800;"), std::nullopt);
    EXPECT_EQ(Resolve("&#xDBFF;"), std::nullopt);
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
