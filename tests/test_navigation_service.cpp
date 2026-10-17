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
        // ブラウザからコピーしたリンクはパーセントエンコードされている。
        { "#%E8%A6%8B%E5%87%BA%E3%81%97", Type::Anchor, "見出し" },
        { "#read%2Dme", Type::Anchor, "read-me" },

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
        // 関連付けのコマンドラインへ引数を差し込めないよう、引用符・空白・制御文字は %XX にする。
        { "mailto:a\" /m \"x", Type::ExternalUrl, "mailto:a%22%20/m%20%22x" },
        { "https://example.com/a b\tc\x7F", Type::ExternalUrl, "https://example.com/a%20b%09c%7F" },
        { "https://example.com/?q=%22", Type::ExternalUrl, "https://example.com/?q=%22" },
        { "https://例え.jp/パス", Type::ExternalUrl, "https://例え.jp/パス" },

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

TEST(PercentDecodeTest, DecodesHexPairsAndKeepsInvalidSequences)
{
    struct Case {
        std::string_view in;
        std::string_view out;
    };
    constexpr Case kCases[] = {
        { "", "" },
        { "plain/path.png", "plain/path.png" },
        { "my%20images/a.png", "my images/a.png" },
        { "%E6%97%A5%E6%9C%AC", "\xE6\x97\xA5\xE6\x9C\xAC" },
        { "%e6%97%a5", "\xE6\x97\xA5" },
        { "100%", "100%" },
        { "%2", "%2" },
        { "%zz%20", "%zz " },
        { "%%41", "%A" },
        { "a%2Fb", "a/b" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.in);
        EXPECT_EQ(PercentDecode(c.in), c.out);
    }
}
