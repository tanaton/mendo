#include <gtest/gtest.h>
#include "nav.h"

TEST(HandleLinkClickTest, ClassifiesLinks)
{
    using Type = LinkClickResult::Type;
    struct Case {
        std::string_view url;
        Type type;
        std::string_view target;
    };
    constexpr Case kCases[] = {
        { "", Type::None, "" },

        { "#section-1", Type::Anchor, "section-1" },
        { "#", Type::Anchor, "" },
        { "#見出し", Type::Anchor, "見出し" },

        { "https://example.com", Type::ExternalUrl, "https://example.com" },
        { "http://example.com", Type::ExternalUrl, "http://example.com" },
        { "mailto:user@example.com", Type::ExternalUrl, "mailto:user@example.com" },
        // スキームは RFC 上 case-insensitive
        { "HTTPS://EXAMPLE.COM", Type::ExternalUrl, "HTTPS://EXAMPLE.COM" },
        { "HtTpS://example.com", Type::ExternalUrl, "HtTpS://example.com" },
        { "HTTP://example.com", Type::ExternalUrl, "HTTP://example.com" },
        { "MAILTO:user@example.com", Type::ExternalUrl, "MAILTO:user@example.com" },
        { "MailTo:user@example.com", Type::ExternalUrl, "MailTo:user@example.com" },
        // prefix 後が空でも通るのが現状の仕様。変えるならここを更新する
        { "http://", Type::ExternalUrl, "http://" },

        { "file:///C:/Windows/System32/cmd.exe", Type::None, "" },
        { "javascript:alert(1)", Type::None, "" },
        { "ftp://example.com/file", Type::None, "" },
        { "other.md", Type::None, "" },
        // 先頭空白でスキーム比較をすり抜ける攻撃
        { " http://example.com", Type::None, "" },
        { "http:example.com", Type::None, "" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(c.url));
        const auto result = HandleLinkClick(c.url);
        EXPECT_EQ(result.type, c.type);
        EXPECT_EQ(result.target, c.target);
    }
}
