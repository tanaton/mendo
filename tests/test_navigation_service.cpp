#include <gtest/gtest.h>
#include "navigation_service.h"

// HandleLinkClick は自由関数のため、フィクスチャ不要
TEST(HandleLinkClickTest, HandleAnchorLink)
{
    auto result = HandleLinkClick(L"#section-1");
    EXPECT_EQ(result.type, LinkClickResult::Type::Anchor);
    EXPECT_EQ(result.target, L"section-1");
}

TEST(HandleLinkClickTest, HandleExternalLink)
{
    auto result = HandleLinkClick(L"https://example.com");
    EXPECT_EQ(result.type, LinkClickResult::Type::ExternalUrl);
    EXPECT_EQ(result.target, L"https://example.com");
}

TEST(HandleLinkClickTest, HandleHttpLink)
{
    auto result = HandleLinkClick(L"http://example.com");
    EXPECT_EQ(result.type, LinkClickResult::Type::ExternalUrl);
}

TEST(HandleLinkClickTest, HandleMailtoLink)
{
    auto result = HandleLinkClick(L"mailto:user@example.com");
    EXPECT_EQ(result.type, LinkClickResult::Type::ExternalUrl);
}

TEST(HandleLinkClickTest, BlockFileScheme)
{
    auto result = HandleLinkClick(L"file:///C:/Windows/System32/cmd.exe");
    EXPECT_EQ(result.type, LinkClickResult::Type::None);
}

TEST(HandleLinkClickTest, BlockJavascriptScheme)
{
    auto result = HandleLinkClick(L"javascript:alert(1)");
    EXPECT_EQ(result.type, LinkClickResult::Type::None);
}

TEST(HandleLinkClickTest, BlockUnknownScheme)
{
    auto result = HandleLinkClick(L"ftp://example.com/file");
    EXPECT_EQ(result.type, LinkClickResult::Type::None);
}

TEST(HandleLinkClickTest, BlockBareRelativePath)
{
    auto result = HandleLinkClick(L"other.md");
    EXPECT_EQ(result.type, LinkClickResult::Type::None);
}

TEST(HandleLinkClickTest, HttpsCaseInsensitive)
{
    auto result = HandleLinkClick(L"HTTPS://EXAMPLE.COM");
    EXPECT_EQ(result.type, LinkClickResult::Type::ExternalUrl);
}

TEST(HandleLinkClickTest, HandleEmptyLink)
{
    auto result = HandleLinkClick(L"");
    EXPECT_EQ(result.type, LinkClickResult::Type::None);
}

// ---- 大小文字混在の URL スキーム ----
// ShellExecute に渡す URL のスキームは RFC 上 case-insensitive。
// 実装は IsSafeUrlScheme で to_lower 比較しているが、混在ケースの
// 回帰がないか直接的に検証する。
TEST(HandleLinkClickTest, HttpsMixedCaseRecognized)
{
    auto r = HandleLinkClick(L"HtTpS://example.com");
    EXPECT_EQ(r.type, LinkClickResult::Type::ExternalUrl);
}

TEST(HandleLinkClickTest, HttpMixedCaseRecognized)
{
    auto r = HandleLinkClick(L"HTTP://example.com");
    EXPECT_EQ(r.type, LinkClickResult::Type::ExternalUrl);
}

TEST(HandleLinkClickTest, MailtoUpperCaseRecognized)
{
    auto r = HandleLinkClick(L"MAILTO:user@example.com");
    EXPECT_EQ(r.type, LinkClickResult::Type::ExternalUrl);
}

TEST(HandleLinkClickTest, MailtoMixedCaseRecognized)
{
    auto r = HandleLinkClick(L"MailTo:user@example.com");
    EXPECT_EQ(r.type, LinkClickResult::Type::ExternalUrl);
}

// ---- アンカーリンクの境界 ----

TEST(HandleLinkClickTest, BareHashTreatedAsAnchorWithEmptyTarget)
{
    // "#" 単体は anchor 扱いだが target は空文字列。
    auto r = HandleLinkClick(L"#");
    EXPECT_EQ(r.type, LinkClickResult::Type::Anchor);
    EXPECT_EQ(r.target, L"");
}

TEST(HandleLinkClickTest, AnchorWithUnicodeTarget)
{
    auto r = HandleLinkClick(L"#見出し");
    EXPECT_EQ(r.type, LinkClickResult::Type::Anchor);
    EXPECT_EQ(r.target, L"見出し");
}

// ---- 偽装されたスキームを弾く ----

TEST(HandleLinkClickTest, LeadingSpaceBeforeHttpIsBlocked)
{
    // 先頭にスペースを入れて IsSafeUrlScheme の比較をすり抜けようとする攻撃を弾く。
    auto r = HandleLinkClick(L" http://example.com");
    EXPECT_EQ(r.type, LinkClickResult::Type::None);
}

TEST(HandleLinkClickTest, HttpWithoutSlashesIsBlocked)
{
    // "http:example.com" のように // が無い場合は IsSafeUrlScheme が false を返す。
    auto r = HandleLinkClick(L"http:example.com");
    EXPECT_EQ(r.type, LinkClickResult::Type::None);
}

TEST(HandleLinkClickTest, JustSchemePrefixWithoutContentIsAllowed)
{
    // "http://" だけでも prefix マッチで通る。これは現状の仕様。
    // 万が一実装が「prefix 後に少なくとも 1 文字」を要求するように変わったら
    // このテストを更新する。
    auto r = HandleLinkClick(L"http://");
    EXPECT_EQ(r.type, LinkClickResult::Type::ExternalUrl);
}
