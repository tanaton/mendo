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
