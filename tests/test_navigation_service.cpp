#include <gtest/gtest.h>
#include "navigation_service.h"

class NavigationServiceTest : public ::testing::Test {
protected:
    NavHistory history_;
    NavigationService service_{history_};
};

TEST_F(NavigationServiceTest, HandleAnchorLink) {
    auto result = service_.HandleLinkClick(L"#section-1", L"C:\\file.md");
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::Anchor);
    EXPECT_EQ(result.target, L"section-1");
}

TEST_F(NavigationServiceTest, HandleExternalLink) {
    auto result = service_.HandleLinkClick(L"https://example.com", L"C:\\file.md");
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::ExternalUrl);
    EXPECT_EQ(result.target, L"https://example.com");
}

TEST_F(NavigationServiceTest, HandleHttpLink) {
    auto result = service_.HandleLinkClick(L"http://example.com", L"C:\\file.md");
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::ExternalUrl);
}

TEST_F(NavigationServiceTest, HandleMailtoLink) {
    auto result = service_.HandleLinkClick(L"mailto:user@example.com", L"C:\\file.md");
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::ExternalUrl);
}

TEST_F(NavigationServiceTest, BlockFileScheme) {
    auto result = service_.HandleLinkClick(L"file:///C:/Windows/System32/cmd.exe", L"C:\\file.md");
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::None);
}

TEST_F(NavigationServiceTest, BlockJavascriptScheme) {
    auto result = service_.HandleLinkClick(L"javascript:alert(1)", L"C:\\file.md");
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::None);
}

TEST_F(NavigationServiceTest, BlockUnknownScheme) {
    auto result = service_.HandleLinkClick(L"ftp://example.com/file", L"C:\\file.md");
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::None);
}

TEST_F(NavigationServiceTest, BlockBareRelativePath) {
    auto result = service_.HandleLinkClick(L"other.md", L"C:\\file.md");
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::None);
}

TEST_F(NavigationServiceTest, HttpsCaseInsensitive) {
    auto result = service_.HandleLinkClick(L"HTTPS://EXAMPLE.COM", L"C:\\file.md");
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::ExternalUrl);
}

TEST_F(NavigationServiceTest, HandleEmptyLink) {
    auto result = service_.HandleLinkClick(L"", L"C:\\file.md");
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::None);
}

TEST_F(NavigationServiceTest, GoBackNoHistory) {
    auto result = service_.GoBack(L"C:\\file.md", 100.0f);
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::None);
}

TEST_F(NavigationServiceTest, GoBackSameFile) {
    service_.PushHistory(L"C:\\file.md", 50.0f);

    auto result = service_.GoBack(L"C:\\file.md", 200.0f);
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::Anchor);
    EXPECT_FLOAT_EQ(result.scroll_y, 50.0f);
}

TEST_F(NavigationServiceTest, GoBackDifferentFile) {
    service_.PushHistory(L"C:\\first.md", 50.0f);

    auto result = service_.GoBack(L"C:\\second.md", 200.0f);
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::LoadFile);
    EXPECT_EQ(result.target, L"C:\\first.md");
    EXPECT_FLOAT_EQ(result.scroll_y, 50.0f);
}

TEST_F(NavigationServiceTest, GoForwardNoHistory) {
    auto result = service_.GoForward(L"C:\\file.md", 100.0f);
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::None);
}

TEST_F(NavigationServiceTest, GoBackThenForward) {
    service_.PushHistory(L"C:\\first.md", 10.0f);

    // 戻る
    auto back_result = service_.GoBack(L"C:\\first.md", 100.0f);
    EXPECT_EQ(back_result.type, NavigationService::NavigateResult::Type::Anchor);
    EXPECT_FLOAT_EQ(back_result.scroll_y, 10.0f);

    // 進む
    auto fwd_result = service_.GoForward(L"C:\\first.md", 10.0f);
    EXPECT_EQ(fwd_result.type, NavigationService::NavigateResult::Type::Anchor);
    EXPECT_FLOAT_EQ(fwd_result.scroll_y, 100.0f);
}

TEST_F(NavigationServiceTest, CanGoBackForward) {
    EXPECT_FALSE(service_.CanGoBack());
    EXPECT_FALSE(service_.CanGoForward());

    service_.PushHistory(L"C:\\file.md", 0.0f);
    EXPECT_TRUE(service_.CanGoBack());
    EXPECT_FALSE(service_.CanGoForward());
}
