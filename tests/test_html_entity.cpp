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
    if (view.data() == entity.data()) {
        return std::nullopt;
    }
    return std::string{ view };
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

TEST(HtmlEntity, RejectsInvalidReferences)
{
    constexpr std::string_view kCases[] = {
        "&unknownentity;",
        "&COPY2;",
        // 名前は大文字小文字を区別する
        "&Mdash;",
        "&copy",
        // 二分探索の境界: 前方一致・後方延長・表の両端の外側
        "&AEli;",
        "&zwnjx;",
        "&am;",
        "&AAA;",
        "&zzzz;",
        // サロゲート (U+D800-U+DFFF) は単独では文字にならない
        "&#xD800;",
        "&#xDBFF;",
        "&#xDC00;",
        "&#xDFFF;",
        "&#0;",
        "&#x0;",
        "&#x110000;",
        "&#x7FFFFFFF;",
        // 32 bit 整数に収まらない数は wrap (例: 4294967297 → 1) せずに弾く。
        "&#4294967296;", // 2^32 (10 桁、wrap で 0)
        "&#4294967297;", // 2^32 + 1 (10 桁、wrap で 1)
        "&#x100000000;", // 2^32 (9 桁 hex、wrap で 0)
        "&#x100000001;", // 2^32 + 1 (9 桁 hex、wrap で 1)
        "&unknown;",
        "&#xZZ;",
        "&#;",
        "",
    };
    for (const std::string_view entity : kCases) {
        EXPECT_EQ(Resolve(entity), std::nullopt) << entity;
    }
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
